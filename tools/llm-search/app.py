"""LLM Search & Fetch service — safe web tools for local models.

Endpoints (all on 127.0.0.1 by default):
  GET  /health                     service and cache state
  GET  /search?q=...&count=8       metasearch (Bing RSS -> Bing HTML -> SearXNG)
  GET  /fetch?url=...&max_chars=   safe fetch + readable text
  GET  /research?q=...&count=4     search + fetch top results (LLM-ready context)
  GET  /tools.json                 OpenAI-style tool schemas

Auth: if SEARCH_TOKEN is set, every request must send "Authorization: Bearer <token>".
"""
import asyncio
import json
import logging
import time
from collections import defaultdict, deque

from fastapi import Depends, FastAPI, Header, HTTPException, Query, Request
from fastapi.responses import JSONResponse

import fetch as fetch_mod
from config import (
    FETCH_TTL,
    MAX_CHARS,
    RATE_PER_MIN,
    SEARX_URL,
    TOKEN,
    VERSION,
)
from cache import cache
from safety import UnsafeURLError
from search import search as do_search

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("search-svc")

STARTED = time.time()
_hits: dict[str, deque[float]] = defaultdict(deque)

app = FastAPI(
    title="LLM Search & Fetch",
    version=VERSION,
    description="Safe web search/fetch/research API for local LLMs. Binds to 127.0.0.1.",
)


@app.on_event("startup")
async def _startup() -> None:
    fetch_mod.client()
    log.info("started v%s, searx=%s, token=%s", VERSION, bool(SEARX_URL), bool(TOKEN))


@app.on_event("shutdown")
async def _shutdown() -> None:
    await fetch_mod.close()


@app.middleware("http")
async def _rate_limit(request: Request, call_next):
    ip = request.client.host if request.client else "?"
    now = time.time()
    bucket = _hits[ip]
    while bucket and now - bucket[0] > 60:
        bucket.popleft()
    if len(bucket) >= RATE_PER_MIN:
        return JSONResponse({"detail": "rate limit exceeded"}, status_code=429)
    bucket.append(now)
    return await call_next(request)


def _auth(authorization: str | None = Header(default=None)) -> None:
    if not TOKEN:
        return
    if authorization != "Bearer %s" % TOKEN:
        raise HTTPException(status_code=401, detail="invalid token")


def _err(exc: Exception) -> HTTPException:
    if isinstance(exc, UnsafeURLError):
        return HTTPException(status_code=400, detail=str(exc))
    if isinstance(exc, ValueError):
        return HTTPException(status_code=400, detail=str(exc))
    return HTTPException(status_code=502, detail="upstream error: %s" % exc)


@app.get("/health")
async def health() -> dict:
    return {
        "status": "ok",
        "version": VERSION,
        "uptime_s": int(time.time() - STARTED),
        "auth_required": bool(TOKEN),
        "searx": bool(SEARX_URL),
        "cache": cache.stats(),
        "fetch_ttl_s": FETCH_TTL,
    }


@app.get("/search", dependencies=[Depends(_auth)])
async def search_get(
    q: str = Query(..., min_length=1, max_length=300),
    count: int = Query(8, ge=1, le=20),
    lang: str | None = Query(None, max_length=8),
) -> dict:
    try:
        return await do_search(q, count=count, lang=lang)
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


@app.post("/search", dependencies=[Depends(_auth)])
async def search_post(body: dict) -> dict:
    try:
        return await do_search(
            str(body.get("query") or ""),
            count=min(int(body.get("count") or 8), 20),
            lang=body.get("lang"),
        )
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


@app.get("/fetch", dependencies=[Depends(_auth)])
async def fetch_get(
    url: str = Query(..., min_length=5, max_length=2000),
    max_chars: int = Query(8000, ge=500, le=MAX_CHARS),
    js: bool = Query(False),
) -> dict:
    try:
        return await fetch_mod.fetch_page(url, max_chars=max_chars, use_js=js)
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


@app.post("/fetch", dependencies=[Depends(_auth)])
async def fetch_post(body: dict) -> dict:
    try:
        return await fetch_mod.fetch_page(
            str(body.get("url") or ""),
            max_chars=min(int(body.get("max_chars") or 8000), MAX_CHARS),
            use_js=bool(body.get("js")),
        )
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


async def _research(q: str, count: int, budget: int) -> dict:
    started = time.perf_counter()
    found = await do_search(q, count=max(count * 2, count), lang=None)
    candidates = found.get("results", [])[: max(count * 2, count)]
    if not candidates:
        return {"query": q, "sources": [], "error": "no search results", "took_ms": int((time.perf_counter() - started) * 1000)}

    picks = candidates[:count]
    tasks = [fetch_mod.fetch_page(c["url"], max_chars=budget // max(1, len(picks)), use_js=False) for c in picks]
    results = await asyncio.gather(*tasks, return_exceptions=True)

    per_source = max(800, budget // max(1, len(picks)))
    sources = []
    for cand, res in zip(picks, results):
        if isinstance(res, Exception):
            sources.append({
                "title": cand.get("title", ""),
                "url": cand.get("url", ""),
                "snippet": cand.get("snippet", ""),
                "ok": False,
                "error": str(res),
            })
            continue
        text = res.get("text", "")
        if len(text) > per_source:
            text = text[:per_source].rsplit(" ", 1)[0] + " ..."
        sources.append({
            "title": res.get("title") or cand.get("title", ""),
            "url": res.get("final_url") or cand.get("url", ""),
            "snippet": cand.get("snippet", ""),
            "text": text,
            "chars": len(text),
            "ok": bool(text),
        })
    return {
        "query": q,
        "sources": sources,
        "ok_count": sum(1 for s in sources if s.get("ok")),
        "took_ms": int((time.perf_counter() - started) * 1000),
    }


@app.get("/research", dependencies=[Depends(_auth)])
async def research_get(
    q: str = Query(..., min_length=1, max_length=300),
    count: int = Query(4, ge=1, le=8),
    budget: int = Query(12000, ge=2000, le=40000),
) -> dict:
    try:
        return await _research(q, count, budget)
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


@app.post("/research", dependencies=[Depends(_auth)])
async def research_post(body: dict) -> dict:
    try:
        return await _research(
            str(body.get("query") or ""),
            count=min(int(body.get("count") or 4), 8),
            budget=min(int(body.get("budget") or 12000), 40000),
        )
    except Exception as exc:  # noqa: BLE001
        raise _err(exc) from exc


TOOLS = {
    "search_web": {
        "type": "function",
        "function": {
            "name": "search_web",
            "description": "Search the web. Returns titles, URLs and snippets.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "search query"},
                    "count": {"type": "integer", "minimum": 1, "maximum": 20, "default": 8},
                },
                "required": ["query"],
            },
        },
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
                    "max_chars": {"type": "integer", "minimum": 500, "maximum": MAX_CHARS, "default": 8000},
                },
                "required": ["url"],
            },
        },
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
                    "budget": {"type": "integer", "minimum": 2000, "maximum": 40000, "default": 12000},
                },
                "required": ["query"],
            },
        },
    },
}


@app.get("/tools.json")
async def tools_json() -> JSONResponse:
    return JSONResponse(
        {
            "base_url": "http://127.0.0.1:8099",
            "openapi": "/openapi.json",
            "note": "GET endpoints take query parameters; POST endpoints take a JSON body.",
            "tools": TOOLS,
        }
    )
