"""SQLite-backed cache with TTL and a size cap (keeps disk usage bounded)."""
import os
import sqlite3
import threading
import time

from config import CACHE_MAX_MB, DATA_DIR


class Cache:
    def __init__(self, path: str | None = None, max_mb: int = CACHE_MAX_MB):
        self.path = path or str(DATA_DIR / "cache.sqlite3")
        self.max_bytes = max_mb * 1024 * 1024
        self._lock = threading.Lock()
        self._db = sqlite3.connect(self.path, check_same_thread=False, timeout=10)
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA synchronous=NORMAL")
        self._db.execute(
            "CREATE TABLE IF NOT EXISTS cache ("
            " key TEXT PRIMARY KEY, value TEXT NOT NULL, created REAL NOT NULL,"
            " expires REAL NOT NULL, size INTEGER NOT NULL)"
        )
        self._db.commit()

    def get(self, key: str) -> str | None:
        now = time.time()
        with self._lock:
            row = self._db.execute(
                "SELECT value, expires FROM cache WHERE key = ?", (key,)
            ).fetchone()
            if not row:
                return None
            value, expires = row
            if expires < now:
                self._db.execute("DELETE FROM cache WHERE key = ?", (key,))
                self._db.commit()
                return None
            return value

    def put(self, key: str, value: str, ttl: int) -> None:
        now = time.time()
        size = len(value.encode("utf-8", "replace"))
        with self._lock:
            self._db.execute(
                "INSERT OR REPLACE INTO cache (key, value, created, expires, size) "
                "VALUES (?, ?, ?, ?, ?)",
                (key, value, now, now + ttl, size),
            )
            self._db.commit()
            self._maybe_evict()

    def _maybe_evict(self) -> None:
        try:
            total = os.path.getsize(self.path)
        except OSError:
            return
        if total <= self.max_bytes:
            return
        # Drop the oldest third and compact the file.
        self._db.execute(
            "DELETE FROM cache WHERE key IN ("
            " SELECT key FROM cache ORDER BY created ASC"
            " LIMIT MAX(1, (SELECT COUNT(*) FROM cache) / 3))"
        )
        self._db.commit()
        try:
            self._db.execute("VACUUM")
        except sqlite3.Error:
            pass

    def stats(self) -> dict:
        with self._lock:
            count = self._db.execute("SELECT COUNT(*) FROM cache").fetchone()[0]
        try:
            size = os.path.getsize(self.path)
        except OSError:
            size = 0
        return {"entries": count, "mb": round(size / 1048576, 2), "limit_mb": CACHE_MAX_MB}

    def clear(self) -> None:
        with self._lock:
            self._db.execute("DELETE FROM cache")
            self._db.commit()


cache = Cache()
