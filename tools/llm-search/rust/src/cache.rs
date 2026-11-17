//! TTL cache with a hard byte cap held in memory.
//!
//! The Python build kept the cache in SQLite. Here it is a plain in-memory map:
//! the service writes nothing to disk, so `ProtectSystem=strict` stays on and
//! there is no cache file to corrupt or back up. TTLs and the byte cap match.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

pub fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

struct Entry {
    value: String,
    expires: u64,
    weight: usize,
    seq: u64,
}

struct Inner {
    map: HashMap<String, Entry>,
    total: usize,
    seq: u64,
}

pub struct TtlCache {
    inner: Mutex<Inner>,
    max_bytes: usize,
}

impl TtlCache {
    pub fn new(max_bytes: usize) -> Self {
        Self {
            inner: Mutex::new(Inner {
                map: HashMap::new(),
                total: 0,
                seq: 0,
            }),
            max_bytes,
        }
    }

    pub fn get(&self, key: &str) -> Option<String> {
        let now = now_secs();
        let mut inner = self.inner.lock().unwrap();
        let expired = match inner.map.get(key) {
            Some(entry) => {
                if entry.expires < now {
                    true
                } else {
                    return Some(entry.value.clone());
                }
            }
            None => return None,
        };
        if expired {
            if let Some(entry) = inner.map.remove(key) {
                inner.total = inner.total.saturating_sub(entry.weight);
            }
        }
        None
    }

    pub fn put(&self, key: &str, value: String, ttl_secs: u64) {
        let weight = value.len();
        if weight > self.max_bytes {
            return;
        }
        let mut inner = self.inner.lock().unwrap();
        inner.seq += 1;
        let seq = inner.seq;
        let entry = Entry {
            value,
            expires: now_secs() + ttl_secs.max(1),
            weight,
            seq,
        };
        if let Some(old) = inner.map.insert(key.to_string(), entry) {
            inner.total = inner.total.saturating_sub(old.weight);
        }
        inner.total += weight;
        while inner.total > self.max_bytes {
            let victim = inner
                .map
                .iter()
                .min_by_key(|(_, e)| e.seq)
                .map(|(k, _)| k.clone());
            match victim {
                Some(k) => {
                    if let Some(old) = inner.map.remove(&k) {
                        inner.total = inner.total.saturating_sub(old.weight);
                    }
                }
                None => break,
            }
        }
    }

    pub fn stats(&self) -> (usize, usize) {
        let inner = self.inner.lock().unwrap();
        (inner.map.len(), inner.total)
    }

    pub fn clear(&self) {
        let mut inner = self.inner.lock().unwrap();
        inner.map.clear();
        inner.total = 0;
    }
}
