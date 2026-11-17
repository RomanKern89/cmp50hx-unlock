//! Safe page fetching: SSRF-validated, DNS-pinned, redirect-checked, size-capped.

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Instant;

use reqwest::header::{HeaderMap, HeaderValue, ACCEPT, ACCEPT_LANGUAGE, CONTENT_TYPE, LOCATION};
use reqwest::redirect::Policy;
use serde_json::{json, Value};
use url::Url;

use crate::extract;
use crate::safety::{self, Target};
use crate::{ApiError, AppState};

/// Connection pinned to the address that passed validation (blocks DNS rebinding).
async fn pinned_client(st: &Arc<AppState>, target: &Target) -> Result<reqwest::Client, ApiError> {
    let mut headers = HeaderMap::new();
    headers.insert(
        ACCEPT,
        HeaderValue::from_static("text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.1"),
    );
    headers.insert(ACCEPT_LANGUAGE, HeaderValue::from_static("ru,en;q=0.8"));
    reqwest::Client::builder()
        .connect_timeout(st.cfg.connect_timeout)
        .timeout(st.cfg.read_timeout + st.cfg.connect_timeout)
        .user_agent(st.cfg.ua.clone())
        .default_headers(headers)
        .redirect(Policy::none())
        .no_proxy()
        .resolve(&target.host, SocketAddr::new(target.ip, target.port))
        .build()
        .map_err(|e| ApiError::upstream(format!("client build failed: {e}")))
}

async fn robots_text(st: &Arc<AppState>, target: &Target) -> Option<String> {
    if !st.cfg.respect_robots {
        return None;
    }
    let key = format!("robots:{}", target.origin());
    if let Some(hit) = st.cache.get(&key) {
        return if hit.is_empty() { None } else { Some(hit) };
    }

    let mut body = String::new();
    if let Ok(client) = pinned_client(st, target).await {
        let url = format!("{}/robots.txt", target.origin());
        if let Ok(mut resp) = client.get(url).send().await {
            if resp.status().is_success() {
                let mut buf: Vec<u8> = Vec::new();
                while let Ok(Some(chunk)) = resp.chunk().await {
                    if buf.len() + chunk.len() > 200_000 {
                        break;
                    }
                    buf.extend_from_slice(&chunk);
                }
                body = String::from_utf8_lossy(&buf).into_owned();
            }
        }
    }
    st.cache.put(&key, body.clone(), 3600);
    if body.is_empty() {
        None
    } else {
        Some(body)
    }
}

async fn kitesurf_html(st: &Arc<AppState>, url: &str) -> Option<String> {
    if !st.cfg.kitesurf_fallback {
        return None;
    }
    let mut endpoint = Url::parse(&st.cfg.kitesurf_url).ok()?;
    endpoint.query_pairs_mut().append_pair("url", url);
    let resp = st
        .http
        .get(endpoint)
        .timeout(std::time::Duration::from_secs(25))
        .send()
        .await
        .ok()?;
    if !resp.status().is_success() {
        return None;
    }
    resp.text().await.ok()
}

fn charset_label(content_type: &str) -> Option<String> {
    let lower = content_type.to_ascii_lowercase();
    let idx = lower.find("charset=")?;
    let rest = &content_type[idx + "charset=".len()..];
    let value = rest
        .split(';')
        .next()
        .unwrap_or("")
        .trim()
        .trim_matches(|c| c == '"' || c == '\'');
    if value.is_empty() {
        None
    } else {
        Some(value.to_string())
    }
}

fn sniff_meta_charset(bytes: &[u8]) -> Option<String> {
    let head = &bytes[..bytes.len().min(4096)];
    let text = String::from_utf8_lossy(head).to_ascii_lowercase();
    let idx = text.find("charset=")?;
    let rest = &text[idx + "charset=".len()..];
    let label: String = rest
        .chars()
        .take_while(|c| c.is_ascii_alphanumeric() || *c == '-' || *c == '_')
        .collect();
    if label.is_empty() {
        None
    } else {
        Some(label)
    }
}

fn decode_body(bytes: &[u8], content_type: &str) -> String {
    let label = charset_label(content_type).or_else(|| sniff_meta_charset(bytes));
    let encoding = label
        .as_deref()
        .and_then(|l| encoding_rs::Encoding::for_label(l.as_bytes()))
        .unwrap_or(encoding_rs::UTF_8);
    let (text, _, _) = encoding.decode(bytes);
    text.into_owned()
}

fn truncate_chars(text: &str, limit: usize) -> String {
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

pub async fn fetch_page(
    st: &Arc<AppState>,
    raw_url: &str,
    max_chars: usize,
    use_js: bool,
) -> Result<Value, ApiError> {
    let started = Instant::now();
    let key = format!("fetch:{}", raw_url.trim());

    if let Some(hit) = st.cache.get(&key) {
        if let Ok(mut payload) = serde_json::from_str::<Value>(&hit) {
            let text = payload
                .get("text")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string();
            let text = truncate_chars(&text, max_chars);
            payload["text"] = json!(text);
            payload["chars"] = json!(text.chars().count());
            payload["cached"] = json!(true);
            payload["took_ms"] = json!(started.elapsed().as_millis() as u64);
            return Ok(payload);
        }
    }

    let mut target = safety::validate(raw_url).await.map_err(ApiError::bad)?;
    let _permit = st
        .sem
        .acquire()
        .await
        .map_err(|_| ApiError::upstream("service shutting down"))?;

    let mut hops = 0usize;
    loop {
        if let Some(robots) = robots_text(st, &target).await {
            if !safety::Robots::parse(&robots).allowed(&target.path_and_query()) {
                return Err(ApiError::bad("robots.txt disallows fetching this URL"));
            }
        }

        let client = pinned_client(st, &target).await?;
        let mut resp = client
            .get(target.url.clone())
            .send()
            .await
            .map_err(|e| ApiError::upstream(format!("request failed: {e}")))?;

        let status = resp.status();
        if status.is_redirection() {
            if hops >= st.cfg.max_redirects {
                return Err(ApiError::upstream("too many redirects"));
            }
            let location = resp
                .headers()
                .get(LOCATION)
                .and_then(|v| v.to_str().ok())
                .map(|s| s.to_string());
            let location = location
                .ok_or_else(|| ApiError::upstream(format!("upstream HTTP {}", status.as_u16())))?;
            let next = target
                .url
                .join(&location)
                .map_err(|e| ApiError::bad(format!("bad redirect location: {e}")))?;
            target = safety::validate(next.as_str()).await.map_err(ApiError::bad)?;
            hops += 1;
            continue;
        }
        if status.is_client_error() || status.is_server_error() {
            return Err(ApiError::upstream(format!(
                "upstream HTTP {}",
                status.as_u16()
            )));
        }

        let content_type = resp
            .headers()
            .get(CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .unwrap_or("")
            .to_ascii_lowercase();
        if !content_type.is_empty()
            && !["text/html", "application/xhtml", "text/plain"]
                .iter()
                .any(|t| content_type.contains(t))
        {
            let short = content_type.split(';').next().unwrap_or("").to_string();
            return Err(ApiError::bad(format!("unsupported content-type: {short}")));
        }

        let mut buf: Vec<u8> = Vec::new();
        while let Some(chunk) = resp
            .chunk()
            .await
            .map_err(|e| ApiError::upstream(format!("read failed: {e}")))?
        {
            if buf.len() + chunk.len() > st.cfg.max_bytes {
                return Err(ApiError::bad(format!(
                    "response exceeds {} bytes",
                    st.cfg.max_bytes
                )));
            }
            buf.extend_from_slice(&chunk);
        }

        let body = decode_body(&buf, &content_type);
        let (mut title, mut text) = extract::extract(&body);

        if use_js && text.chars().count() < 300 {
            if let Some(rendered) = kitesurf_html(st, target.url.as_str()).await {
                let (js_title, js_text) = extract::extract(&rendered);
                if js_text.chars().count() > text.chars().count() {
                    if !js_title.is_empty() {
                        title = js_title;
                    }
                    text = js_text;
                }
            }
        }

        let text = truncate_chars(&text, max_chars);
        let mut payload = json!({
            "url": raw_url.trim(),
            "final_url": target.url.as_str(),
            "status": status.as_u16(),
            "title": title,
            "chars": text.chars().count(),
            "text": text,
            "cached": false,
        });
        st.cache.put(
            &key,
            serde_json::to_string(&payload).unwrap_or_default(),
            st.cfg.fetch_ttl,
        );
        payload["took_ms"] = json!(started.elapsed().as_millis() as u64);
        return Ok(payload);
    }
}
