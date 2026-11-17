//! LLM Search & Fetch service (Rust) — safe web tools for local models.
//!
//! Endpoints (bound to 127.0.0.1 by default):
//!   GET  /health                     service and cache state
//!   GET  /search?q=...&count=8       metasearch (Bing RSS -> Bing HTML -> SearXNG)
//!   GET  /fetch?url=...&max_chars=   safe fetch + readable text
//!   GET  /research?q=...&count=4     search + read top results (LLM-ready context)
//!   GET  /tools.json                 OpenAI-style tool schemas
//!   GET  /openapi.json               minimal OpenAPI document
//!
//! Auth: if SEARCH_TOKEN is set, request must send `Authorization: Bearer <token>`.

mod cache;
mod extract;
mod fetch;
mod safety;
mod search;

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use axum::extract::{Query, Request, State};
use axum::http::{header, HeaderMap, StatusCode};
use axum::middleware::{self, Next};
use axum::response::{IntoResponse, Json, Response};
use axum::routing::get;
use axum::Router;
use serde::Deserialize;
use serde_json::{json, Value};

const VERSION: &str = "1.0.0";

fn env_str(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| default.to_string())
}

fn env_usize(name: &str, default: usize) -> usize {
    std::env::var(name)
        .ok()
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(default)
}

fn env_u64(name: &str, default: u64) -> u64 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.trim().parse().ok())
        .unwrap_or(default)
}

fn env_bool(name: &str, default: bool) -> bool {
    match std::env::var(name) {
        Ok(v) => matches!(
            v.trim().to_ascii_lowercase().as_str(),
            "1" | "true" | "yes" | "on"
        ),
        Err(_) => default,
    }
}

pub struct Config {
    pub token: String,
    pub ua: String,
    pub connect_timeout: Duration,
    pub read_timeout: Duration,
    pub max_redirects: usize,
    pub max_bytes: usize,
    pub max_chars: usize,
    pub respect_robots: bool,
    pub cache_max_bytes: usize,
    pub fetch_ttl: u64,
    pub search_ttl: u64,
    pub searx_url: String,
    pub rate_per_min: usize,
    pub fetch_concurrency: usize,
    pub kitesurf_fallback: bool,
    pub kitesurf_url: String,
}

impl Config {
    fn load() -> Self {
        Config {
            token: env_str("SEARCH_TOKEN", "").trim().to_string(),
            ua: env_str(
                "SEARCH_UA",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 \
                 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
            ),
            connect_timeout: Duration::from_secs(env_u64("SEARCH_CONNECT_TIMEOUT", 5)),
            read_timeout: Duration::from_secs(env_u64("SEARCH_READ_TIMEOUT", 12)),
            max_redirects: env_usize("SEARCH_MAX_REDIRECTS", 4),
            max_bytes: env_usize("SEARCH_MAX_BYTES", 2 * 1024 * 1024),
            max_chars: env_usize("SEARCH_MAX_CHARS", 20_000),
            respect_robots: env_bool("SEARCH_RESPECT_ROBOTS", true),
            cache_max_bytes: env_usize("SEARCH_CACHE_MB", 64) * 1024 * 1024,
            fetch_ttl: env_u64("SEARCH_FETCH_TTL", 6 * 3600),
            search_ttl: env_u64("SEARCH_TTL", 600),
            searx_url: env_str("SEARCH_SEARX_URL", "")
                .trim()
                .trim_end_matches('/')
                .to_string(),
            rate_per_min: env_usize("SEARCH_RATE_PER_MIN", 60),
            fetch_concurrency: env_usize("SEARCH_FETCH_CONCURRENCY", 6),
            kitesurf_fallback: env_bool("SEARCH_KITESURF_FALLBACK", false),
            kitesurf_url: env_str("SEARCH_KITESURF_URL", "https://kitesurf.dev/html"),
        }
    }
}

pub struct AppState {
    pub cfg: Config,
    pub cache: cache::TtlCache,
    pub http: reqwest::Client,
    pub sem: tokio::sync::Semaphore,
    pub rate: Mutex<VecDeque<Instant>>,
    pub ddg_warm: Mutex<Option<Instant>>,
    pub started: Instant,
}

pub struct ApiError {
    pub status: StatusCode,
    pub msg: String,
}

impl ApiError {
    pub fn bad(msg: impl Into<String>) -> Self {
        Self {
            status: StatusCode::BAD_REQUEST,
            msg: msg.into(),
        }
    }

    pub fn upstream(msg: impl Into<String>) -> Self {
        Self {
            status: StatusCode::BAD_GATEWAY,
            msg: msg.into(),
        }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> Response {
        (self.status, Json(json!({ "detail": self.msg }))).into_response()
    }
}

#[derive(Deserialize)]
struct SearchParams {
    q: String,
    #[serde(default)]
    count: Option<usize>,
    #[serde(default)]
    lang: Option<String>,
}

#[derive(Deserialize)]
struct FetchParams {
    url: String,
    #[serde(default)]
    max_chars: Option<usize>,
    #[serde(default)]
    js: Option<bool>,
}

#[derive(Deserialize)]
struct ResearchParams {
    q: String,
    #[serde(default)]
    count: Option<usize>,
    #[serde(default)]
    budget: Option<usize>,
}

async fn rate_limit(
    State(state): State<Arc<AppState>>,
    req: Request,
    next: Next,
) -> Response {
    {
        let mut bucket = state.rate.lock().unwrap();
        let now = Instant::now();
        while let Some(first) = bucket.front() {
            if now.duration_since(*first) > Duration::from_secs(60) {
                bucket.pop_front();
            } else {
                break;
            }
        }
        if bucket.len() >= state.cfg.rate_per_min {
            return (
                StatusCode::TOO_MANY_REQUESTS,
                Json(json!({ "detail": "rate limit exceeded" })),
            )
                .into_response();
        }
        bucket.push_back(now);
    }
    next.run(req).await
}

async fn auth(
    State(state): State<Arc<AppState>>,
    headers: HeaderMap,
    req: Request,
    next: Next,
) -> Response {
    if !state.cfg.token.is_empty() {
        let expected = format!("Bearer {}", state.cfg.token);
        let got = headers
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .unwrap_or("");
        if got != expected {
            return (
                StatusCode::UNAUTHORIZED,
                Json(json!({ "detail": "invalid token" })),
            )
                .into_response();
        }
    }
    next.run(req).await
}

async fn health(State(state): State<Arc<AppState>>) -> Json<Value> {
    let (entries, bytes) = state.cache.stats();
    Json(json!({
        "status": "ok",
        "version": VERSION,
        "impl": "rust",
        "uptime_s": state.started.elapsed().as_secs(),
        "auth_required": !state.cfg.token.is_empty(),
        "searx": !state.cfg.searx_url.is_empty(),
        "cache": {
            "entries": entries,
            "mb": ((bytes as f64) / 1048576.0 * 100.0).round() / 100.0,
            "limit_mb": state.cfg.cache_max_bytes / 1048576,
        },
        "fetch_ttl_s": state.cfg.fetch_ttl,
    }))
}

async fn search_get(
    State(state): State<Arc<AppState>>,
    Query(params): Query<SearchParams>,
) -> Result<Json<Value>, ApiError> {
    let count = params.count.unwrap_or(8).clamp(1, 20);
    let result =
        search::search(&state, &params.q, count, params.lang.as_deref()).await?;
    Ok(Json(result))
}

async fn search_post(
    State(state): State<Arc<AppState>>,
    Json(body): Json<Value>,
) -> Result<Json<Value>, ApiError> {
    let query = body.get("query").and_then(|v| v.as_str()).unwrap_or("");
    let count = body.get("count").and_then(|v| v.as_u64()).unwrap_or(8) as usize;
    let lang = body
        .get("lang")
        .and_then(|v| v.as_str())
        .map(|s| s.to_string());
    let result = search::search(&state, query, count.clamp(1, 20), lang.as_deref()).await?;
    Ok(Json(result))
}

async fn fetch_get(
    State(state): State<Arc<AppState>>,
    Query(params): Query<FetchParams>,
) -> Result<Json<Value>, ApiError> {
    let max_chars = params
        .max_chars
        .unwrap_or(8000)
        .clamp(500, state.cfg.max_chars);
    let result = fetch::fetch_page(
        &state,
        &params.url,
        max_chars,
        params.js.unwrap_or(false),
    )
    .await?;
    Ok(Json(result))
}

async fn fetch_post(
    State(state): State<Arc<AppState>>,
    Json(body): Json<Value>,
) -> Result<Json<Value>, ApiError> {
    let url = body.get("url").and_then(|v| v.as_str()).unwrap_or("");
    let max_chars = body
        .get("max_chars")
        .and_then(|v| v.as_u64())
        .map(|v| v as usize)
        .unwrap_or(8000)
        .clamp(500, state.cfg.max_chars);
    let js = body.get("js").and_then(|v| v.as_bool()).unwrap_or(false);
    let result = fetch::fetch_page(&state, url, max_chars, js).await?;
    Ok(Json(result))
}

fn truncate_words(text: &str, limit: usize) -> String {
    if text.chars().count() <= limit {
        return text.to_string();
    }
    let mut cut: String = text.chars().take(limit).collect();
    if let Some(pos) = cut.rfind(' ') {
        cut.truncate(pos);
    }
    cut.push_str(" ...");
    cut
}

async fn research(
    state: &Arc<AppState>,
    query: &str,
    count: usize,
    budget: usize,
) -> Result<Value, ApiError> {
    let started = Instant::now();
    let found = search::search(state, query, count * 2, None).await?;
    let candidates: Vec<Value> = found
        .get("results")
        .and_then(|v| v.as_array())
        .map(|items| items.iter().take(count).cloned().collect())
        .unwrap_or_default();

    if candidates.is_empty() {
        return Ok(json!({
            "query": query,
            "sources": [],
            "error": "no search results",
            "took_ms": started.elapsed().as_millis() as u64,
        }));
    }

    let per_source = (budget / candidates.len()).max(800);
    let mut tasks = tokio::task::JoinSet::new();
    for (index, candidate) in candidates.iter().enumerate() {
        let url = candidate
            .get("url")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let state = state.clone();
        tasks.spawn(async move {
            let result = fetch::fetch_page(&state, &url, per_source, false).await;
            (index, result)
        });
    }

    let mut slots: Vec<Option<Result<Value, ApiError>>> =
        candidates.iter().map(|_| None).collect();
    while let Some(joined) = tasks.join_next().await {
        if let Ok((index, result)) = joined {
            if index < slots.len() {
                slots[index] = Some(result);
            }
        }
    }

    let mut sources: Vec<Value> = Vec::new();
    for (candidate, slot) in candidates.iter().zip(slots.into_iter()) {
        let title = candidate
            .get("title")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let url = candidate
            .get("url")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let snippet = candidate
            .get("snippet")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();

        match slot {
            Some(Ok(page)) => {
                let text = page
                    .get("text")
                    .and_then(|v| v.as_str())
                    .unwrap_or("")
                    .to_string();
                let text = truncate_words(&text, per_source);
                let page_title = page
                    .get("title")
                    .and_then(|v| v.as_str())
                    .filter(|s| !s.is_empty())
                    .unwrap_or(title.as_str())
                    .to_string();
                let final_url = page
                    .get("final_url")
                    .and_then(|v| v.as_str())
                    .unwrap_or(url.as_str())
                    .to_string();
                sources.push(json!({
                    "title": page_title,
                    "url": final_url,
                    "snippet": snippet,
                    "text": text,
                    "chars": text.chars().count(),
                    "ok": !text.is_empty(),
                }));
            }
            Some(Err(err)) => sources.push(json!({
                "title": title,
                "url": url,
                "snippet": snippet,
                "ok": false,
                "error": err.msg,
            })),
            None => sources.push(json!({
                "title": title,
                "url": url,
                "snippet": snippet,
                "ok": false,
                "error": "fetch task failed",
            })),
        }
    }

    let ok_count = sources
        .iter()
        .filter(|s| s.get("ok").and_then(|v| v.as_bool()).unwrap_or(false))
        .count();

    Ok(json!({
        "query": query,
        "sources": sources,
        "ok_count": ok_count,
        "took_ms": started.elapsed().as_millis() as u64,
    }))
}

async fn research_get(
    State(state): State<Arc<AppState>>,
    Query(params): Query<ResearchParams>,
) -> Result<Json<Value>, ApiError> {
    let count = params.count.unwrap_or(4).clamp(1, 8);
    let budget = params.budget.unwrap_or(12_000).clamp(2_000, 40_000);
    let result = research(&state, &params.q, count, budget).await?;
    Ok(Json(result))
}

async fn research_post(
    State(state): State<Arc<AppState>>,
    Json(body): Json<Value>,
) -> Result<Json<Value>, ApiError> {
    let query = body.get("query").and_then(|v| v.as_str()).unwrap_or("");
    let count = body
        .get("count")
        .and_then(|v| v.as_u64())
        .unwrap_or(4)
        .clamp(1, 8) as usize;
    let budget = body
        .get("budget")
        .and_then(|v| v.as_u64())
        .unwrap_or(12_000)
        .clamp(2_000, 40_000) as usize;
    let result = research(&state, query, count, budget).await?;
    Ok(Json(result))
}

async fn tools_json() -> Json<Value> {
    Json(json!({
        "base_url": "http://127.0.0.1:8099",
        "openapi": "/openapi.json",
        "note": "GET endpoints take query parameters; POST endpoints take a JSON body.",
        "tools": {
            "search_web": {
                "type": "function",
                "function": {
                    "name": "search_web",
                    "description": "Search the web. Returns titles, URLs and snippets.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "query": {"type": "string", "description": "search query"},
                            "count": {"type": "integer", "minimum": 1, "maximum": 20, "default": 8}
                        },
                        "required": ["query"]
                    }
                }
            },
            "fetch_url": {
                "type": "function",
                "function": {
                    "name": "fetch_url",
                    "description": "Fetch one public web page and return its readable text.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "url": {"type": "string", "description": "public http(s) URL"},
                            "max_chars": {"type": "integer", "minimum": 500, "maximum": 20000, "default": 8000}
                        },
                        "required": ["url"]
                    }
                }
            },
            "research": {
                "type": "function",
                "function": {
                    "name": "research",
                    "description": "Search, open the best results and return extracted text with sources.",
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "query": {"type": "string"},
                            "count": {"type": "integer", "minimum": 1, "maximum": 8, "default": 4},
                            "budget": {"type": "integer", "minimum": 2000, "maximum": 40000, "default": 12000}
                        },
                        "required": ["query"]
                    }
                }
            }
        }
    }))
}

async fn openapi() -> Json<Value> {
    Json(json!({
        "openapi": "3.0.3",
        "info": {
            "title": "LLM Search & Fetch",
            "version": VERSION,
            "description": "Safe web search/fetch/research API for local LLMs."
        },
        "paths": {
            "/search": {"get": {"operationId": "search_web", "summary": "Web search",
                "parameters": [{
                    "name": "q", "in": "query", "required": true,
                    "description": "search query", "schema": {"type": "string"}
                }]
            }},
            "/fetch": {"get": {"operationId": "fetch_url", "summary": "Fetch page text",
                "parameters": [
                    {"name": "url", "in": "query", "required": true,
                     "description": "public http(s) URL", "schema": {"type": "string"}},
                    {"name": "max_chars", "in": "query", "required": false,
                     "description": "max returned characters",
                     "schema": {"type": "integer", "default": 8000}}
                ]
            }},
            "/research": {"get": {"operationId": "research", "summary": "Search and read top results",
                "parameters": [
                    {"name": "q", "in": "query", "required": true,
                     "description": "search query", "schema": {"type": "string"}},
                    {"name": "count", "in": "query", "required": false,
                     "description": "how many pages to read",
                     "schema": {"type": "integer", "default": 4}}
                ]
            }},
            "/health": {"get": {"operationId": "health", "summary": "Service status",
                "parameters": []
            }}
        }
    }))
}

#[tokio::main]
async fn main() {
    let cfg = Config::load();
    let bind = env_str("SEARCH_BIND", "127.0.0.1:8099");

    let http = reqwest::Client::builder()
        .connect_timeout(cfg.connect_timeout)
        .timeout(cfg.read_timeout + cfg.connect_timeout)
        .user_agent(cfg.ua.clone())
        .redirect(reqwest::redirect::Policy::limited(4))
        .no_proxy()
        .cookie_store(true)
        .gzip(true)
        .brotli(true)
        .build()
        .expect("http client");

    let state = Arc::new(AppState {
        cache: cache::TtlCache::new(cfg.cache_max_bytes),
        sem: tokio::sync::Semaphore::new(cfg.fetch_concurrency),
        rate: Mutex::new(VecDeque::new()),
        ddg_warm: Mutex::new(None),
        started: Instant::now(),
        http,
        cfg,
    });

    let protected = Router::new()
        .route("/search", get(search_get).post(search_post))
        .route("/fetch", get(fetch_get).post(fetch_post))
        .route("/research", get(research_get).post(research_post))
        .layer(middleware::from_fn_with_state(state.clone(), auth));

    let app = Router::new()
        .route("/health", get(health))
        .route("/tools.json", get(tools_json))
        .route("/openapi.json", get(openapi))
        .merge(protected)
        .layer(middleware::from_fn_with_state(state.clone(), rate_limit))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind(&bind)
        .await
        .unwrap_or_else(|e| panic!("cannot bind {bind}: {e}"));
    println!("search-svc {VERSION} (rust) listening on {bind}");
    axum::serve(listener, app).await.expect("server error");
}
