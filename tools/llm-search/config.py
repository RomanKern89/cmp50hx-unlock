"""Configuration for the LLM search service (env-driven, safe defaults)."""
import os
from pathlib import Path


def _bool(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def _int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


BASE_DIR = Path(os.environ.get("SEARCH_BASE", "/data/search-svc"))
DATA_DIR = Path(os.environ.get("SEARCH_DATA", str(BASE_DIR / "data")))
DATA_DIR.mkdir(parents=True, exist_ok=True)

VERSION = "1.0.0"

# Auth: empty token means "no auth" — acceptable only because we bind 127.0.0.1.
TOKEN = os.environ.get("SEARCH_TOKEN", "").strip()

# Network
UA = os.environ.get(
    "SEARCH_UA",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36",
)
CONNECT_TIMEOUT = _int("SEARCH_CONNECT_TIMEOUT", 5)
READ_TIMEOUT = _int("SEARCH_READ_TIMEOUT", 12)
MAX_REDIRECTS = _int("SEARCH_MAX_REDIRECTS", 4)
MAX_BYTES = _int("SEARCH_MAX_BYTES", 2 * 1024 * 1024)
MAX_CHARS = _int("SEARCH_MAX_CHARS", 20000)
ALLOWED_PORTS = {80, 443}
RESPECT_ROBOTS = _bool("SEARCH_RESPECT_ROBOTS", True)

# Cache
CACHE_MAX_MB = _int("SEARCH_CACHE_MB", 64)
FETCH_TTL = _int("SEARCH_FETCH_TTL", 6 * 3600)
SEARCH_TTL = _int("SEARCH_TTL", 600)

# Search backends
SEARX_URL = os.environ.get("SEARCH_SEARX_URL", "").strip().rstrip("/")
BING_RSS = _bool("SEARCH_BING_RSS", True)

# Optional JS-rendering fallback (sends the URL to Cloudflare). Off by default.
KITESURF_FALLBACK = _bool("SEARCH_KITESURF_FALLBACK", False)
KITESURF_URL = os.environ.get("SEARCH_KITESURF_URL", "https://kitesurf.dev/html")

# Limits
RATE_PER_MIN = _int("SEARCH_RATE_PER_MIN", 60)
FETCH_CONCURRENCY = _int("SEARCH_FETCH_CONCURRENCY", 6)
