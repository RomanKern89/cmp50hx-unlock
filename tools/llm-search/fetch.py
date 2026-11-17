"""Safe HTTP fetch + text extraction (no JS by default, hard limits)."""
import asyncio
import json
import re
import time
from urllib.parse import quote, urljoin, urlsplit

import httpx
import trafilatura
from lxml import html as lxml_html

from cache import cache
from config import (
    CONNECT_TIMEOUT,
    FETCH_CONCURRENCY,
    FETCH_TTL,
    KITESURF_FALLBACK,
    KITESURF_URL,
    MAX_BYTES,
    MAX_REDIRECTS,
    READ_TIMEOUT,
    RESPECT_ROBOTS,
    UA,
)
from safety import UnsafeURLError, normalize_url, robots_allowed_sync, robots_key, validate_url

_ALLOWED_TYPES = ("text/html", "application/xhtml+xml", "text/plain")
_sem = asyncio.Semaphore(FETCH_CONCURRENCY)
_robots_text: dict[str, tuple[float, str | None]] = {}
_ROBOTS_TTL = 3600.0

_client: httpx.AsyncClient | None = None


def client() -> httpx.AsyncClient:
    global _client
    if _client is None:
        _client = httpx.AsyncClient(
            timeout=httpx.Timeout(connect=CONNECT_TIMEOUT, read=READ_TIMEOUT, write=10, pool=3),
            limits=httpx.Limits(max_connections=24, max_keepalive_connections=12),
            headers={
                "User-Agent": UA,
                "Accept": "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.1",
                "Accept-Language": "ru,en;q=0.8",
            },
            follow_redirects=False,
            verify=True,
        )
    return _client


async def close() -> None:
    global _client
    if _client is not None:
        await _client.aclose()
        _client = None


def _strip_tags(text: str) -> str:
    text = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", text)
    text = re.sub(r"(?s)<[^>]+>", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def _fallback_extract(html_text: str) -> str:
    try:
        doc = lxml_html.fromstring(html_text)
    except Exception:
        return _strip_tags(html_text)
    for bad in doc.xpath("//script|//style|//noscript|//nav|//header|//footer|//aside|//form|//iframe|//svg"):
        bad.drop_tree()
    parts = [line.strip() for line in doc.text_content().splitlines()]
    lines = [p for p in parts if len(p) > 2]
    return "\n".join(lines)


def extract(html_text: str) -> tuple[str, str]:
    """Return (title, text). trafilatura first, lxml fallback."""
    title = ""
    try:
        doc = lxml_html.fromstring(html_text)
        nodes = doc.xpath("//title/text()")
        if nodes:
            title = re.sub(r"\s+", " ", nodes[0]).strip()
    except Exception:
        pass

    text = ""
    try:
        text = trafilatura.extract(
            html_text,
            include_comments=False,
            include_tables=True,
            favor_precision=True,
            output_format="txt",
        ) or ""
    except Exception:
        text = ""
    if len(text) < 200:
        alt = _fallback_extract(html_text)
        if len(alt) > len(text):
            text = alt
    return title, text.strip()


async def _robots_txt(url: str) -> str | None:
    if not RESPECT_ROBOTS:
        return None
    key = robots_key(url)
    now = time.time()
    hit = _robots_text.get(key)
    if hit and now - hit[0] < _ROBOTS_TTL:
        return hit[1]
    body = None
    try:
        resp = await client().get(key, timeout=httpx.Timeout(connect=4, read=6, write=4, pool=2))
        if resp.status_code == 200:
            body = resp.text[:200_000]
    except Exception:
        body = None
    _robots_text[key] = (now, body)
    return body


async def _read_limited(resp: httpx.Response) -> bytes:
    buf = bytearray()
    async for chunk in resp.aiter_bytes(65536):
        buf.extend(chunk)
        if len(buf) > MAX_BYTES:
            raise ValueError("response exceeds %d bytes" % MAX_BYTES)
    return bytes(buf)


async def _kitesurf_html(url: str) -> str | None:
    """Optional JS renderer fallback (external service; opt-in)."""
    if not KITESURF_FALLBACK:
        return None
    try:
        resp = await client().get(
            "%s?url=%s" % (KITESURF_URL, quote(url, safe="")),
            timeout=httpx.Timeout(connect=6, read=25, write=6, pool=3),
            headers={"User-Agent": UA, "Accept": "text/html"},
        )
        if resp.status_code == 200:
            return resp.text[:MAX_BYTES]
    except Exception:
        return None
    return None


async def fetch_page(url: str, max_chars: int = 8000, use_js: bool = False) -> dict:
    started = time.perf_counter()
    raw_url = url.strip()
    cache_key = "fetch:%s" % raw_url

    cached = cache.get(cache_key)
    if cached:
        try:
            payload = json.loads(cached)
            payload["cached"] = True
            payload["text"] = payload.get("text", "")[:max_chars]
            payload["chars"] = len(payload["text"])
            payload["took_ms"] = int((time.perf_counter() - started) * 1000)
            return payload
        except Exception:
            pass

    current = await validate_url(raw_url)
    async with _sem:
        robots = await _robots_txt(current)
        if not robots_allowed_sync(current, robots):
            raise UnsafeURLError("robots.txt disallows fetching this URL")

        resp: httpx.Response | None = None
        for _hop in range(MAX_REDIRECTS + 1):
            resp = await client().get(current)
            if resp.status_code in (301, 302, 303, 307, 308) and resp.headers.get("location"):
                nxt = normalize_url(urljoin(current, resp.headers["location"]))
                current = await validate_url(nxt)
                continue
            break

        if resp is None:
            raise ValueError("no response")
        if resp.status_code >= 400:
            raise ValueError("upstream HTTP %d" % resp.status_code)

        ctype = (resp.headers.get("content-type") or "").lower()
        if ctype and not any(t in ctype for t in _ALLOWED_TYPES):
            raise ValueError("unsupported content-type: %s" % ctype.split(";")[0])

        body = await _read_limited(resp)
        charset = resp.charset_encoding or "utf-8"
        try:
            html_text = body.decode(charset, "replace")
        except LookupError:
            html_text = body.decode("utf-8", "replace")

        title, text = extract(html_text)

        if use_js and len(text) < 300:
            rendered = await _kitesurf_html(current)
            if rendered:
                js_title, js_text = extract(rendered)
                if len(js_text) > len(text):
                    title, text = js_title or title, js_text

        payload = {
            "url": raw_url,
            "final_url": current,
            "status": resp.status_code,
            "title": title,
            "text": text,
            "chars": len(text),
            "cached": False,
        }
        cache.put(cache_key, json.dumps(payload, ensure_ascii=False), FETCH_TTL)

    payload["text"] = payload["text"][:max_chars]
    payload["chars"] = len(payload["text"])
    payload["took_ms"] = int((time.perf_counter() - started) * 1000)
    return payload
