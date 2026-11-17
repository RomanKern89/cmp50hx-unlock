"""URL safety: block private networks (SSRF), unusual ports, and honour robots.txt."""
import asyncio
import ipaddress
import socket
import time
import urllib.robotparser
from urllib.parse import urlsplit, urlunsplit

from config import ALLOWED_PORTS, RESPECT_ROBOTS, UA

BLOCKED_HOSTNAMES = {
    "localhost",
    "localhost.localdomain",
    "metadata.google.internal",
    "metadata",
    "instance-data",
}

_dns_cache: dict[str, tuple[float, list[str]]] = {}
_DNS_TTL = 300.0
_robots_cache: dict[str, tuple[float, urllib.robotparser.RobotFileParser | None]] = {}
_ROBOTS_TTL = 3600.0


class UnsafeURLError(ValueError):
    pass


def normalize_url(url: str) -> str:
    parts = urlsplit(url.strip())
    scheme = parts.scheme.lower()
    host = (parts.hostname or "").lower().rstrip(".")
    if not host:
        raise UnsafeURLError("missing host")
    port = parts.port
    if port is None:
        port = 443 if scheme == "https" else 80
    netloc = host if port in (80, 443) else "%s:%d" % (host, port)
    return urlunsplit((scheme, netloc, parts.path or "/", parts.query, ""))


def _resolve_ips(host: str) -> list[str]:
    now = time.time()
    hit = _dns_cache.get(host)
    if hit and now - hit[0] < _DNS_TTL:
        return hit[1]
    infos = socket.getaddrinfo(host, None, proto=socket.IPPROTO_TCP)
    ips = sorted({info[4][0] for info in infos})
    if not ips:
        raise UnsafeURLError("host does not resolve")
    _dns_cache[host] = (now, ips)
    return ips


def _ip_is_public(ip: str) -> bool:
    try:
        addr = ipaddress.ip_address(ip.split("%", 1)[0])
    except ValueError:
        return False
    if isinstance(addr, ipaddress.IPv6Address) and addr.ipv4_mapped:
        addr = addr.ipv4_mapped
    return addr.is_global


async def validate_url(url: str) -> str:
    """Return a normalized URL or raise UnsafeURLError."""
    parts = urlsplit(url.strip())
    if parts.scheme.lower() not in ("http", "https"):
        raise UnsafeURLError("only http/https are allowed")
    if parts.username or parts.password:
        raise UnsafeURLError("credentials in URL are not allowed")
    host = (parts.hostname or "").strip().lower().rstrip(".")
    if not host or host in BLOCKED_HOSTNAMES or host.endswith((".local", ".internal", ".lan")):
        raise UnsafeURLError("host is not allowed")
    port = parts.port or (443 if parts.scheme.lower() == "https" else 80)
    if port not in ALLOWED_PORTS:
        raise UnsafeURLError("port %d is not allowed" % port)

    ips = await asyncio.to_thread(_resolve_ips, host)
    for ip in ips:
        if not _ip_is_public(ip):
            raise UnsafeURLError("host resolves to a non-public address")
    return normalize_url(url)


def robots_allowed_sync(url: str, robots_txt: str | None) -> bool:
    if not RESPECT_ROBOTS or robots_txt is None:
        return True
    parts = urlsplit(url)
    key = "%s://%s" % (parts.scheme, parts.netloc)
    now = time.time()
    hit = _robots_cache.get(key)
    if hit and now - hit[0] < _ROBOTS_TTL:
        parser = hit[1]
    else:
        parser = urllib.robotparser.RobotFileParser()
        parser.parse(robots_txt.splitlines())
        _robots_cache[key] = (now, parser)
    try:
        return parser.can_fetch(UA, url)
    except Exception:
        return True


def robots_key(url: str) -> str:
    parts = urlsplit(url)
    return "%s://%s/robots.txt" % (parts.scheme, parts.netloc)
