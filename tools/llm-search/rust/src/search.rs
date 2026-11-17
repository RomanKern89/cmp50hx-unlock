//! Metasearch: Bing RSS (structured) -> Bing HTML fallback -> optional SearXNG.

use std::collections::HashSet;
use std::sync::Arc;
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use url::Url;

use crate::{ApiError, AppState};

const DDG_WARM_TTL: Duration = Duration::from_secs(6 * 3600);

const SKIP_HOSTS: &[&str] = &[
    "bing.com",
    "microsoft.com",
    "microsofttranslator.com",
    "go.microsoft.com",
];

fn collapse(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// base64url (no padding) decoder — Bing wraps result links in `ck/a?u=a1<base64>`.
fn base64url_decode(input: &str) -> Option<Vec<u8>> {
    fn value(byte: u8) -> Option<u8> {
        match byte {
            b'A'..=b'Z' => Some(byte - b'A'),
            b'a'..=b'z' => Some(byte - b'a' + 26),
            b'0'..=b'9' => Some(byte - b'0' + 52),
            b'-' | b'+' => Some(62),
            b'_' | b'/' => Some(63),
            _ => None,
        }
    }
    let bytes: Vec<u8> = input.bytes().filter(|b| *b != b'=').collect();
    let mut out: Vec<u8> = Vec::with_capacity(bytes.len() * 3 / 4 + 3);
    let mut chunk = 0u32;
    let mut bits = 0u32;
    for byte in bytes {
        let v = value(byte)? as u32;
        chunk = (chunk << 6) | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((chunk >> bits) as u8);
        }
    }
    Some(out)
}

/// Turns a Bing click-tracking link into the real destination URL.
fn decode_bing_href(href: &str) -> Option<String> {
    let trimmed = href.trim();
    if !trimmed.contains("bing.com/ck/a") {
        return if trimmed.starts_with("http") {
            Some(trimmed.to_string())
        } else {
            None
        };
    }
    let url = Url::parse(trimmed).ok()?;
    let encoded = url
        .query_pairs()
        .find(|(k, _)| k == "u")
        .map(|(_, v)| v.into_owned())?;
    let encoded = encoded.strip_prefix("a1").unwrap_or(&encoded);
    let bytes = base64url_decode(encoded)?;
    let decoded = String::from_utf8(bytes).ok()?;
    if decoded.starts_with("http") {
        Some(decoded)
    } else {
        None
    }
}

fn strip_tags(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut inside = false;
    for ch in text.chars() {
        match ch {
            '<' => inside = true,
            '>' => {
                inside = false;
                out.push(' ');
            }
            _ => {
                if !inside {
                    out.push(ch);
                }
            }
        }
    }
    out
}

fn is_tracking(key: &str) -> bool {
    let k = key.to_ascii_lowercase();
    k.starts_with("utm_")
        || matches!(
            k.as_str(),
            "fbclid" | "gclid" | "yclid" | "msclkid" | "mc_cid" | "mc_eid" | "ref" | "from"
        )
        || k.starts_with("ref_")
}

fn clean_url(raw: &str) -> Option<String> {
    let mut url = Url::parse(raw).ok()?;
    if url.scheme() != "http" && url.scheme() != "https" {
        return None;
    }
    url.set_fragment(None);
    let kept: Vec<(String, String)> = url
        .query_pairs()
        .filter(|(k, _)| !is_tracking(k))
        .map(|(k, v)| (k.into_owned(), v.into_owned()))
        .collect();
    url.set_query(None);
    if !kept.is_empty() {
        url.query_pairs_mut().extend_pairs(kept);
    }
    let mut out = url.to_string();
    if out.ends_with('/') && !out.ends_with("://") {
        out.pop();
    }
    Some(out)
}

fn host_of(url: &str) -> String {
    Url::parse(url)
        .ok()
        .and_then(|u| u.host_str().map(|h| h.to_ascii_lowercase()))
        .unwrap_or_default()
}

fn dedupe(items: Vec<Value>, limit: usize) -> Vec<Value> {
    let mut seen: HashSet<String> = HashSet::new();
    let mut out: Vec<Value> = Vec::new();
    for mut item in items {
        let raw = item
            .get("url")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let clean = match clean_url(&raw) {
            Some(url) => url,
            None => continue,
        };
        let host = host_of(&clean);
        let skipped = SKIP_HOSTS
            .iter()
            .any(|h| host == *h || host.ends_with(&format!(".{h}")));
        if skipped {
            continue;
        }
        if !seen.insert(clean.to_ascii_lowercase()) {
            continue;
        }
        item["url"] = json!(clean);
        out.push(item);
        if out.len() >= limit {
            break;
        }
    }
    out
}

fn bing_url(query: &str, count: usize, lang: Option<&str>, rss: bool) -> Url {
    let mut url = Url::parse("https://www.bing.com/search").expect("static url");
    {
        let mut pairs = url.query_pairs_mut();
        pairs.append_pair("q", query);
        if rss {
            pairs.append_pair("format", "rss");
        }
        pairs.append_pair("count", &(count * 2).max(10).to_string());
        if let Some(l) = lang {
            pairs.append_pair("setlang", l);
        }
    }
    url
}

/// DuckDuckGo wraps result links: `//duckduckgo.com/l/?uddg=<percent-encoded>`.
fn ddg_unwrap(href: &str) -> Option<String> {
    let trimmed = href.trim();
    if trimmed.is_empty() {
        return None;
    }
    let full = if trimmed.starts_with("//") {
        format!("https:{trimmed}")
    } else if trimmed.starts_with('/') {
        format!("https://duckduckgo.com{trimmed}")
    } else {
        trimmed.to_string()
    };
    let url = Url::parse(&full).ok()?;
    if url
        .host_str()
        .map(|h| h.ends_with("duckduckgo.com"))
        .unwrap_or(false)
    {
        return url
            .query_pairs()
            .find(|(k, _)| k == "uddg")
            .map(|(_, v)| v.into_owned());
    }
    Some(full)
}

/// DuckDuckGo HTML endpoint. Needs a cookie handshake first (a GET of the home
/// page), otherwise it answers 202 with an anti-bot page.
async fn duckduckgo(
    st: &Arc<AppState>,
    query: &str,
    lang: Option<&str>,
) -> Result<Vec<Value>, ApiError> {
    let needs_warm = {
        let warm = st.ddg_warm.lock().unwrap();
        warm.map(|t| t.elapsed() > DDG_WARM_TTL).unwrap_or(true)
    };
    if needs_warm {
        if let Ok(resp) = st.http.get("https://duckduckgo.com/").send().await {
            let _ = resp.bytes().await;
        }
        *st.ddg_warm.lock().unwrap() = Some(Instant::now());
    }

    let cyrillic = query
        .chars()
        .any(|c| ('\u{0400}'..='\u{04ff}').contains(&c));
    let region = match lang.map(|l| l.to_ascii_lowercase()) {
        Some(l) if l.starts_with("ru") => "ru-ru".to_string(),
        Some(l) if l.starts_with("en") => "us-en".to_string(),
        _ => {
            if cyrillic {
                "ru-ru".to_string()
            } else {
                "wt-wt".to_string()
            }
        }
    };

    let resp = st
        .http
        .post("https://html.duckduckgo.com/html/")
        .header(reqwest::header::REFERER, "https://duckduckgo.com/")
        .form(&[("q", query), ("kl", region.as_str())])
        .send()
        .await
        .map_err(|e| ApiError::upstream(format!("duckduckgo: {e}")))?;

    if !resp.status().is_success() {
        return Err(ApiError::upstream(format!(
            "duckduckgo HTTP {}",
            resp.status().as_u16()
        )));
    }
    let body = resp
        .text()
        .await
        .map_err(|e| ApiError::upstream(format!("duckduckgo read: {e}")))?;
    if body.contains("anomaly-modal") || body.contains("If this error persists") {
        return Err(ApiError::upstream("duckduckgo: anti-bot page"));
    }

    let doc = scraper::Html::parse_document(&body);
    let anchor_sel =
        scraper::Selector::parse("a.result__a").expect("static selector");
    let snippet_sel =
        scraper::Selector::parse(".result__snippet").expect("static selector");
    let snippets: Vec<String> = doc
        .select(&snippet_sel)
        .map(|node| collapse(&strip_tags(&node.text().collect::<Vec<_>>().join(" "))))
        .collect();

    let mut items = Vec::new();
    for (index, anchor) in doc.select(&anchor_sel).enumerate() {
        let href = anchor.value().attr("href").unwrap_or("");
        let url = match ddg_unwrap(href) {
            Some(url) => url,
            None => continue,
        };
        let title = collapse(&anchor.text().collect::<Vec<_>>().join(" "));
        if title.is_empty() {
            continue;
        }
        items.push(json!({
            "title": title,
            "url": url,
            "snippet": snippets.get(index).cloned().unwrap_or_default(),
            "engine": "duckduckgo",
        }));
    }
    Ok(items)
}
fn child_text(node: &roxmltree::Node<'_, '_>, tag: &str) -> String {
    node.children()
        .find(|c| c.has_tag_name(tag))
        .and_then(|c| c.text())
        .unwrap_or("")
        .trim()
        .to_string()
}

async fn bing_rss(
    st: &Arc<AppState>,
    query: &str,
    count: usize,
    lang: Option<&str>,
) -> Result<Vec<Value>, ApiError> {
    let resp = st
        .http
        .get(bing_url(query, count, lang, true))
        .send()
        .await
        .map_err(|e| ApiError::upstream(format!("bing rss: {e}")))?;
    if !resp.status().is_success() {
        return Err(ApiError::upstream(format!(
            "bing rss HTTP {}",
            resp.status().as_u16()
        )));
    }
    let body = resp
        .text()
        .await
        .map_err(|e| ApiError::upstream(format!("bing rss read: {e}")))?;
    let doc = roxmltree::Document::parse(&body)
        .map_err(|e| ApiError::upstream(format!("bing rss xml: {e}")))?;

    let mut items = Vec::new();
    for item in doc.descendants().filter(|n| n.has_tag_name("item")) {
        let title = child_text(&item, "title");
        let link = child_text(&item, "link");
        let snippet = collapse(&strip_tags(&child_text(&item, "description")));
        if !title.is_empty() && !link.is_empty() {
            items.push(json!({
                "title": title,
                "url": link,
                "snippet": snippet,
                "engine": "bing-rss",
            }));
        }
    }
    Ok(items)
}

async fn bing_html(
    st: &Arc<AppState>,
    query: &str,
    count: usize,
    lang: Option<&str>,
) -> Result<Vec<Value>, ApiError> {
    let resp = st
        .http
        .get(bing_url(query, count, lang, false))
        .send()
        .await
        .map_err(|e| ApiError::upstream(format!("bing html: {e}")))?;
    if !resp.status().is_success() {
        return Err(ApiError::upstream(format!(
            "bing html HTTP {}",
            resp.status().as_u16()
        )));
    }
    let body = resp
        .text()
        .await
        .map_err(|e| ApiError::upstream(format!("bing html read: {e}")))?;

    let doc = scraper::Html::parse_document(&body);
    let li_sel = scraper::Selector::parse("li.b_algo")
        .map_err(|e| ApiError::upstream(format!("selector: {e:?}")))?;
    let a_sel = scraper::Selector::parse("h2 a").expect("static selector");
    let p_sel = scraper::Selector::parse("p").expect("static selector");

    let mut items = Vec::new();
    for node in doc.select(&li_sel) {
        let anchor = match node.select(&a_sel).next() {
            Some(a) => a,
            None => continue,
        };
        let raw_href = anchor.value().attr("href").unwrap_or("");
        let link = match decode_bing_href(raw_href) {
            Some(link) => link,
            None => continue,
        };
        let title = collapse(&anchor.text().collect::<Vec<_>>().join(" "));
        let snippet = node
            .select(&p_sel)
            .next()
            .map(|p| collapse(&p.text().collect::<Vec<_>>().join(" ")))
            .unwrap_or_default();
        items.push(json!({
            "title": title,
            "url": link,
            "snippet": snippet,
            "engine": "bing-html",
        }));
    }
    Ok(items)
}

async fn searx(
    st: &Arc<AppState>,
    query: &str,
    lang: Option<&str>,
) -> Result<Vec<Value>, ApiError> {
    let mut url = Url::parse(&format!("{}/search", st.cfg.searx_url))
        .map_err(|e| ApiError::bad(format!("bad searx url: {e}")))?;
    {
        let mut pairs = url.query_pairs_mut();
        pairs.append_pair("q", query);
        pairs.append_pair("format", "json");
        if let Some(l) = lang {
            pairs.append_pair("language", l);
        }
    }
    let resp = st
        .http
        .get(url)
        .send()
        .await
        .map_err(|e| ApiError::upstream(format!("searx: {e}")))?;
    if !resp.status().is_success() {
        return Err(ApiError::upstream(format!(
            "searx HTTP {}",
            resp.status().as_u16()
        )));
    }
    let data: Value = resp
        .json()
        .await
        .map_err(|e| ApiError::upstream(format!("searx json: {e}")))?;

    let mut items = Vec::new();
    if let Some(results) = data.get("results").and_then(|v| v.as_array()) {
        for r in results {
            let link = r.get("url").and_then(|v| v.as_str()).unwrap_or("");
            if link.is_empty() {
                continue;
            }
            let engines = r
                .get("engines")
                .and_then(|v| v.as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(|e| e.as_str())
                        .collect::<Vec<_>>()
                        .join(",")
                })
                .unwrap_or_default();
            items.push(json!({
                "title": r.get("title").and_then(|v| v.as_str()).unwrap_or("").trim(),
                "url": link,
                "snippet": r.get("content").and_then(|v| v.as_str()).unwrap_or("").trim(),
                "engine": format!("searx:{engines}"),
            }));
        }
    }
    Ok(items)
}

/// Encyclopedic fallback: Wikipedia search API (no key, no captcha, stable).
async fn wikipedia(
    st: &Arc<AppState>,
    query: &str,
    count: usize,
    lang: Option<&str>,
) -> Result<Vec<Value>, ApiError> {
    let code = match lang {
        Some(l) if !l.is_empty() => l.to_ascii_lowercase(),
        _ => {
            if query.chars().any(|c| ('\u{0400}'..='\u{04ff}').contains(&c)) {
                "ru".to_string()
            } else {
                "en".to_string()
            }
        }
    };

    let mut url = Url::parse(&format!("https://{code}.wikipedia.org/w/api.php"))
        .map_err(|e| ApiError::bad(format!("bad wikipedia url: {e}")))?;
    {
        let mut pairs = url.query_pairs_mut();
        pairs.append_pair("action", "query");
        pairs.append_pair("list", "search");
        pairs.append_pair("srsearch", query);
        pairs.append_pair("format", "json");
        pairs.append_pair("srlimit", &count.to_string());
    }

    let resp = st
        .http
        .get(url)
        .send()
        .await
        .map_err(|e| ApiError::upstream(format!("wikipedia: {e}")))?;
    if !resp.status().is_success() {
        return Err(ApiError::upstream(format!(
            "wikipedia HTTP {}",
            resp.status().as_u16()
        )));
    }
    let data: Value = resp
        .json()
        .await
        .map_err(|e| ApiError::upstream(format!("wikipedia json: {e}")))?;

    let mut items = Vec::new();
    if let Some(hits) = data
        .get("query")
        .and_then(|q| q.get("search"))
        .and_then(|s| s.as_array())
    {
        for hit in hits {
            let title = hit.get("title").and_then(|v| v.as_str()).unwrap_or("");
            if title.is_empty() {
                continue;
            }
            let mut page = Url::parse(&format!("https://{code}.wikipedia.org/"))
                .map_err(|e| ApiError::bad(format!("bad wikipedia url: {e}")))?;
            page.set_path(&format!("/wiki/{}", title.replace(' ', "_")));
            let snippet = collapse(&strip_tags(
                hit.get("snippet").and_then(|v| v.as_str()).unwrap_or(""),
            ));
            items.push(json!({
                "title": title,
                "url": page.to_string(),
                "snippet": snippet,
                "engine": format!("wikipedia:{code}"),
            }));
        }
    }
    Ok(items)
}


pub async fn search(
    st: &Arc<AppState>,
    query: &str,
    count: usize,
    lang: Option<&str>,
) -> Result<Value, ApiError> {
    let query = query.trim();
    if query.is_empty() {
        return Err(ApiError::bad("empty query"));
    }
    let started = Instant::now();
    let key = format!(
        "search:{}:{}:{}",
        query.to_lowercase(),
        count,
        lang.unwrap_or("")
    );

    if let Some(hit) = st.cache.get(&key) {
        if let Ok(mut payload) = serde_json::from_str::<Value>(&hit) {
            payload["cached"] = json!(true);
            payload["took_ms"] = json!(started.elapsed().as_millis() as u64);
            return Ok(payload);
        }
    }

    let mut collected: Vec<Value> = Vec::new();
    let mut engines: Vec<String> = Vec::new();
    let mut errors: Vec<String> = Vec::new();

    if !st.cfg.searx_url.is_empty() {
        match searx(st, query, lang).await {
            Ok(mut items) => {
                engines.push("searx".to_string());
                collected.append(&mut items);
            }
            Err(e) => errors.push(format!("searx: {}", e.msg)),
        }
    }

    // Bing HTML first (real result links after decoding click-tracking wrappers),
    // then Bing RSS, then Wikipedia as an encyclopedic fallback.
    for attempt in ["duckduckgo", "bing-html", "bing-rss", "wikipedia"] {
        if !collected.is_empty() {
            break;
        }
        let outcome = match attempt {
            "duckduckgo" => duckduckgo(st, query, lang).await,
            "bing-html" => bing_html(st, query, count, lang).await,
            "bing-rss" => bing_rss(st, query, count, lang).await,
            _ => wikipedia(st, query, count, lang).await,
        };
        match outcome {
            Ok(mut items) if !items.is_empty() => {
                engines.push(attempt.to_string());
                collected.append(&mut items);
            }
            Ok(_) => errors.push(format!("{attempt}: no results")),
            Err(e) => errors.push(format!("{attempt}: {}", e.msg)),
        }
    }

    let results = dedupe(collected, count);
    let found = results.len();
    let payload = json!({
        "query": query,
        "results": results,
        "count": found,
        "engines": engines,
        "errors": errors,
        "cached": false,
        "took_ms": started.elapsed().as_millis() as u64,
    });
    if found > 0 {
        st.cache.put(
            &key,
            serde_json::to_string(&payload).unwrap_or_default(),
            st.cfg.search_ttl,
        );
    }
    Ok(payload)
}

#[cfg(test)]
mod tests {
    use super::{base64url_decode, ddg_unwrap, decode_bing_href};

    #[test]
    fn unwraps_duckduckgo_redirect() {
        let href = "//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpage&rut=xyz";
        assert_eq!(
            ddg_unwrap(href).as_deref(),
            Some("https://example.com/page")
        );
        assert_eq!(
            ddg_unwrap("https://example.com/x").as_deref(),
            Some("https://example.com/x")
        );
        assert_eq!(ddg_unwrap(""), None);
    }

    #[test]
    fn decodes_bing_click_wrapper() {
        let href = "https://www.bing.com/ck/a?!&&p=abc123&u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9wYWdl&ntb=1";
        assert_eq!(
            decode_bing_href(href).as_deref(),
            Some("https://example.com/page")
        );
    }

    #[test]
    fn passes_plain_links_through() {
        assert_eq!(
            decode_bing_href("https://example.com/x").as_deref(),
            Some("https://example.com/x")
        );
        assert_eq!(decode_bing_href("/relative"), None);
    }

    #[test]
    fn base64url_decodes() {
        assert_eq!(
            base64url_decode("aHR0cHM6Ly9leGFtcGxlLmNvbS9wYWdl").unwrap(),
            b"https://example.com/page".to_vec()
        );
    }
}

