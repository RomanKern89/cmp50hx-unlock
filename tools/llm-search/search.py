"""Metasearch backends: DuckDuckGo HTML (POST + cookies), Bing, Wikipedia, SearXNG."""
import base64
import json
import re
import time
import xml.etree.ElementTree as ET
from urllib.parse import parse_qsl, parse_qs, quote, urlencode, urlsplit, urlunsplit

import httpx
from lxml import html as lxml_html

from cache import cache
from config import SEARCH_TTL, SEARX_URL
from fetch import client

_TRACKING = re.compile(r"^(utm_|fbclid|gclid|yclid|msclkid|mc_cid|mc_eid|ref_|ref$|from$)", re.I)
_SKIP_HOSTS = ("bing.com", "microsoft.com", "microsofttranslator.com", "go.microsoft.com")
_CYRILLIC = re.compile(r"[\u0400-\u04FF]")

_DDG_WARM_TTL = 6 * 3600
_ddg_warmed = 0.0


def _clean_url(url: str) -> str:
    try:
        parts = urlsplit(url)
    except ValueError:
        return url
    if parts.scheme not in ("http", "https"):
        return ""
    query = [(k, v) for k, v in parse_qsl(parts.query, keep_blank_values=False)
             if not _TRACKING.match(k)]
    path = parts.path.rstrip("/") or "/"
    return urlunsplit((parts.scheme, parts.netloc.lower(), path, urlencode(query), ""))


def _host(url: str) -> str:
    try:
        return urlsplit(url).netloc.lower()
    except ValueError:
        return ""


def _dedupe(items: list[dict], limit: int) -> list[dict]:
    seen: set[str] = set()
    out: list[dict] = []
    for it in items:
        url = _clean_url(it.get("url") or "")
        if not url:
            continue
        key = url.lower()
        if key in seen:
            continue
        host = _host(url)
        if any(host == h or host.endswith("." + h) for h in _SKIP_HOSTS):
            continue
        seen.add(key)
        it["url"] = url
        out.append(it)
        if len(out) >= limit:
            break
    return out


def _ddg_unwrap(href: str) -> str:
    href = (href or "").strip()
    if not href:
        return ""
    if href.startswith("//"):
        full = "https:" + href
    elif href.startswith("/"):
        full = "https://duckduckgo.com" + href
    else:
        full = href
    try:
        parts = urlsplit(full)
    except ValueError:
        return ""
    if parts.netloc.endswith("duckduckgo.com"):
        found = parse_qs(parts.query).get("uddg")
        return found[0] if found else ""
    return full


def _bing_unwrap(href: str) -> str:
    href = (href or "").strip()
    if "bing.com/ck/a" not in href:
        return href if href.startswith("http") else ""
    try:
        encoded = (parse_qs(urlsplit(href).query).get("u") or [""])[0]
    except ValueError:
        return ""
    if encoded.startswith("a1"):
        encoded = encoded[2:]
    encoded += "=" * (-len(encoded) % 4)
    try:
        decoded = base64.urlsafe_b64decode(encoded).decode("utf-8", "replace")
    except Exception:
        return ""
    return decoded if decoded.startswith("http") else ""


async def _ddg_warmup() -> None:
    """DuckDuckGo HTML answers 202 unless the session has cookies first."""
    global _ddg_warmed
    now = time.time()
    if _ddg_warmed and now - _ddg_warmed < _DDG_WARM_TTL:
        return
    try:
        await client().get("https://duckduckgo.com/",
                           timeout=httpx.Timeout(connect=4, read=8, write=4, pool=3))
    except Exception:
        pass
    _ddg_warmed = now


async def _duckduckgo(query: str, count: int, lang: str | None) -> list[dict]:
    await _ddg_warmup()
    if lang and lang.lower().startswith("ru"):
        region = "ru-ru"
    elif lang and lang.lower().startswith("en"):
        region = "us-en"
    elif _CYRILLIC.search(query):
        region = "ru-ru"
    else:
        region = "wt-wt"

    resp = await client().post(
        "https://html.duckduckgo.com/html/",
        data={"q": query, "kl": region},
        headers={"Referer": "https://duckduckgo.com/"},
    )
    if resp.status_code != 200:
        raise ValueError("duckduckgo HTTP %d" % resp.status_code)
    if "anomaly-modal" in resp.text or "If this error persists" in resp.text:
        raise ValueError("duckduckgo: anti-bot page")

    doc = lxml_html.fromstring(resp.text)
    anchors = doc.xpath("//a[contains(@class,'result__a')]")
    snippets = [
        re.sub(r"\s+", " ", " ".join(node.xpath(".//text()"))).strip()
        for node in doc.xpath("//*[contains(@class,'result__snippet')]")
    ]
    items = []
    for index, anchor in enumerate(anchors):
        url = _ddg_unwrap(anchor.get("href") or "")
        title = re.sub(r"\s+", " ", " ".join(anchor.xpath(".//text()"))).strip()
        if not url or not title:
            continue
        items.append({
            "title": title,
            "url": url,
            "snippet": snippets[index] if index < len(snippets) else "",
            "engine": "duckduckgo",
        })
    return items


def _bing_params(query: str, count: int, lang: str | None, rss: bool) -> dict:
    params = {"q": query, "count": str(max(count * 2, 10))}
    if rss:
        params["format"] = "rss"
    if lang:
        params["setlang"] = lang
    return params


async def _bing_rss(query: str, count: int, lang: str | None) -> list[dict]:
    resp = await client().get("https://www.bing.com/search",
                              params=_bing_params(query, count, lang, True))
    if resp.status_code != 200:
        raise ValueError("bing rss HTTP %d" % resp.status_code)
    root = ET.fromstring(resp.text)
    items = []
    for item in root.iter("item"):
        title = (item.findtext("title") or "").strip()
        link = (item.findtext("link") or "").strip()
        desc = re.sub(r"(?s)<[^>]+>", " ", item.findtext("description") or "")
        desc = re.sub(r"\s+", " ", desc).strip()
        if title and link:
            items.append({"title": title, "url": link, "snippet": desc, "engine": "bing-rss"})
    return items


async def _bing_html(query: str, count: int, lang: str | None) -> list[dict]:
    resp = await client().get("https://www.bing.com/search",
                              params=_bing_params(query, count, lang, False))
    if resp.status_code != 200:
        raise ValueError("bing html HTTP %d" % resp.status_code)
    doc = lxml_html.fromstring(resp.text)
    items = []
    for node in doc.xpath("//li[contains(@class,'b_algo')]"):
        links = node.xpath(".//h2//a/@href")
        if not links:
            continue
        url = _bing_unwrap(links[0])
        if not url:
            continue
        title = re.sub(r"\s+", " ", " ".join(node.xpath(".//h2//a//text()"))).strip()
        snippet = re.sub(r"\s+", " ", " ".join(node.xpath(".//p//text()"))).strip()
        items.append({"title": title, "url": url, "snippet": snippet, "engine": "bing-html"})
    return items


async def _wikipedia(query: str, count: int, lang: str | None) -> list[dict]:
    code = (lang or ("ru" if _CYRILLIC.search(query) else "en")).lower()
    resp = await client().get(
        "https://%s.wikipedia.org/w/api.php" % code,
        params={
            "action": "query",
            "list": "search",
            "srsearch": query,
            "format": "json",
            "srlimit": str(count),
        },
    )
    if resp.status_code != 200:
        raise ValueError("wikipedia HTTP %d" % resp.status_code)
    data = resp.json()
    items = []
    for hit in data.get("query", {}).get("search", []):
        title = (hit.get("title") or "").strip()
        if not title:
            continue
        snippet = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", hit.get("snippet") or "")).strip()
        items.append({
            "title": title,
            "url": "https://%s.wikipedia.org/wiki/%s" % (code, quote(title.replace(" ", "_"))),
            "snippet": snippet,
            "engine": "wikipedia:%s" % code,
        })
    return items


async def _searx(query: str, count: int, lang: str | None) -> list[dict]:
    params = {"q": query, "format": "json"}
    if lang:
        params["language"] = lang
    resp = await client().get(SEARX_URL + "/search", params=params)
    if resp.status_code != 200:
        raise ValueError("searx HTTP %d" % resp.status_code)
    data = resp.json()
    items = []
    for r in data.get("results", []):
        url = r.get("url") or ""
        if not url:
            continue
        items.append({
            "title": (r.get("title") or "").strip(),
            "url": url,
            "snippet": (r.get("content") or "").strip(),
            "engine": "searx:" + ",".join(r.get("engines") or []),
        })
    return items


async def search(query: str, count: int = 8, lang: str | None = None) -> dict:
    query = query.strip()
    if not query:
        raise ValueError("empty query")
    started = time.perf_counter()
    cache_key = "search:%s:%s:%s" % (query.lower(), count, lang or "")
    cached = cache.get(cache_key)
    if cached:
        payload = json.loads(cached)
        payload["cached"] = True
        payload["took_ms"] = int((time.perf_counter() - started) * 1000)
        return payload

    errors: list[str] = []
    collected: list[dict] = []
    engines: list[str] = []

    if SEARX_URL:
        try:
            collected.extend(await _searx(query, count, lang))
            engines.append("searx")
        except Exception as exc:  # noqa: BLE001
            errors.append("searx: %s" % exc)

    # DuckDuckGo HTML first (needs its cookie handshake), then Bing HTML,
    # Bing RSS and Wikipedia as fallbacks.
    for name, backend in (
        ("duckduckgo", _duckduckgo),
        ("bing-html", _bing_html),
        ("bing-rss", _bing_rss),
        ("wikipedia", _wikipedia),
    ):
        if collected:
            break
        try:
            items = await backend(query, count, lang)
        except Exception as exc:  # noqa: BLE001
            errors.append("%s: %s" % (name, exc))
            continue
        if items:
            engines.append(name)
            collected.extend(items)
        else:
            errors.append("%s: no results" % name)

    results = _dedupe(collected, count)
    payload = {
        "query": query,
        "results": results,
        "count": len(results),
        "engines": engines,
        "errors": errors,
        "cached": False,
        "took_ms": int((time.perf_counter() - started) * 1000),
    }
    if results:
        cache.put(cache_key, json.dumps(payload, ensure_ascii=False), SEARCH_TTL)
    return payload
