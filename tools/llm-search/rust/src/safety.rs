//! URL safety: SSRF guard (public IPs only, 80/443 only) plus a small
//! robots.txt parser.
//!
//! The validated target carries the resolved address, so the fetcher can pin the
//! connection to exactly the IP that was checked (no DNS-rebinding window).

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};

use tokio::net::lookup_host;
use url::Url;

const BLOCKED_HOSTS: &[&str] = &[
    "localhost",
    "localhost.localdomain",
    "metadata.google.internal",
    "metadata",
    "instance-data",
];

pub struct Target {
    pub url: Url,
    pub host: String,
    pub port: u16,
    pub ip: IpAddr,
}

impl Target {
    pub fn origin(&self) -> String {
        let default_port = if self.url.scheme() == "https" { 443 } else { 80 };
        if self.port == default_port {
            format!("{}://{}", self.url.scheme(), self.host)
        } else {
            format!("{}://{}:{}", self.url.scheme(), self.host, self.port)
        }
    }

    pub fn path_and_query(&self) -> String {
        let mut out = self.url.path().to_string();
        if out.is_empty() {
            out.push('/');
        }
        if let Some(q) = self.url.query() {
            out.push('?');
            out.push_str(q);
        }
        out
    }
}

fn host_allowed(host: &str) -> bool {
    let h = host.trim_end_matches('.').to_ascii_lowercase();
    if h.is_empty() {
        return false;
    }
    if BLOCKED_HOSTS.contains(&h.as_str()) {
        return false;
    }
    !(h.ends_with(".local")
        || h.ends_with(".internal")
        || h.ends_with(".lan")
        || h.ends_with(".localhost"))
}

fn is_global_v4(ip: Ipv4Addr) -> bool {
    let o = ip.octets();
    if ip.is_unspecified()
        || ip.is_loopback()
        || ip.is_private()
        || ip.is_link_local()
        || ip.is_multicast()
        || ip.is_broadcast()
    {
        return false;
    }
    // 100.64.0.0/10 CGNAT
    if o[0] == 100 && (64..=127).contains(&o[1]) {
        return false;
    }
    // 192.0.0.0/24 (IETF protocol assignments)
    if o[0] == 192 && o[1] == 0 && o[2] == 0 {
        return false;
    }
    // 192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24 documentation
    if o[0] == 192 && o[1] == 0 && o[2] == 2 {
        return false;
    }
    if o[0] == 198 && o[1] == 51 && o[2] == 100 {
        return false;
    }
    if o[0] == 203 && o[1] == 0 && o[2] == 113 {
        return false;
    }
    // 198.18.0.0/15 benchmarking
    if o[0] == 198 && (o[1] == 18 || o[1] == 19) {
        return false;
    }
    // 240.0.0.0/4 reserved
    if o[0] >= 240 {
        return false;
    }
    true
}

fn is_global_v6(ip: Ipv6Addr) -> bool {
    if ip.is_unspecified() || ip.is_loopback() || ip.is_multicast() {
        return false;
    }
    if let Some(v4) = ip.to_ipv4_mapped() {
        return is_global_v4(v4);
    }
    let s = ip.segments();
    // fc00::/7 unique local
    if (s[0] & 0xfe00) == 0xfc00 {
        return false;
    }
    // fe80::/10 link local
    if (s[0] & 0xffc0) == 0xfe80 {
        return false;
    }
    // 2001:db8::/32 documentation
    if s[0] == 0x2001 && s[1] == 0x0db8 {
        return false;
    }
    true
}

pub fn is_global(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v4) => is_global_v4(v4),
        IpAddr::V6(v6) => is_global_v6(v6),
    }
}

pub async fn validate(raw: &str) -> Result<Target, String> {
    let raw = raw.trim();
    if raw.is_empty() {
        return Err("empty url".to_string());
    }
    let mut url = Url::parse(raw).map_err(|e| format!("invalid url: {e}"))?;
    match url.scheme() {
        "http" | "https" => {}
        other => return Err(format!("only http/https are allowed (got {other})")),
    }
    if !url.username().is_empty() || url.password().is_some() {
        return Err("credentials in URL are not allowed".to_string());
    }
    let host = url
        .host_str()
        .ok_or_else(|| "missing host".to_string())?
        .trim_end_matches('.')
        .to_ascii_lowercase();
    if !host_allowed(&host) {
        return Err("host is not allowed".to_string());
    }
    let port = url
        .port_or_known_default()
        .ok_or_else(|| "unknown port".to_string())?;
    if port != 80 && port != 443 {
        return Err(format!("port {port} is not allowed"));
    }

    // Literal addresses never go through DNS; hostnames are resolved and every
    // answer must be public.
    let mut ips: Vec<IpAddr> = Vec::new();
    match url.host() {
        Some(url::Host::Ipv4(v4)) => ips.push(IpAddr::V4(v4)),
        Some(url::Host::Ipv6(v6)) => ips.push(IpAddr::V6(v6)),
        _ => {
            let addrs = lookup_host((host.as_str(), port))
                .await
                .map_err(|e| format!("dns lookup failed: {e}"))?;
            for addr in addrs {
                ips.push(addr.ip());
            }
        }
    }
    if ips.is_empty() {
        return Err("host does not resolve".to_string());
    }
    for ip in &ips {
        if !is_global(*ip) {
            return Err("host resolves to a non-public address".to_string());
        }
    }

    url.set_fragment(None);
    Ok(Target {
        url,
        host,
        port,
        ip: ips[0],
    })
}

/// Minimal robots.txt support: rules for `User-agent: *` only, longest match wins.
pub struct Robots {
    rules: Vec<(bool, String)>,
}

impl Robots {
    pub fn parse(text: &str) -> Self {
        let mut rules: Vec<(bool, String)> = Vec::new();
        let mut applies = false;
        for line in text.lines() {
            let line = line.split('#').next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            let (key, val) = match line.split_once(':') {
                Some(kv) => kv,
                None => continue,
            };
            let key = key.trim().to_ascii_lowercase();
            let val = val.trim();
            match key.as_str() {
                "user-agent" => applies = val == "*",
                "allow" => {
                    if applies && !val.is_empty() {
                        rules.push((true, val.to_string()));
                    }
                }
                "disallow" => {
                    if applies && !val.is_empty() {
                        rules.push((false, val.to_string()));
                    }
                }
                _ => {}
            }
        }
        Self { rules }
    }

    pub fn allowed(&self, path_and_query: &str) -> bool {
        let mut best_len = 0usize;
        let mut verdict = true;
        for (allow, pattern) in &self.rules {
            if !path_and_query.starts_with(pattern.as_str()) {
                continue;
            }
            let len = pattern.len();
            if len >= best_len {
                best_len = len;
                verdict = *allow;
            }
        }
        verdict
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_private_ranges() {
        for ip in [
            "127.0.0.1",
            "10.1.2.3",
            "172.16.0.1",
            "192.168.1.215",
            "169.254.169.254",
            "100.64.0.1",
            "0.0.0.0",
            "::1",
            "fc00::1",
            "fe80::1",
        ] {
            assert!(!is_global(ip.parse().unwrap()), "{ip} must be blocked");
        }
        for ip in ["1.1.1.1", "93.184.216.34", "2606:4700::1111"] {
            assert!(is_global(ip.parse().unwrap()), "{ip} must be allowed");
        }
    }

    #[test]
    fn robots_longest_match_wins() {
        let robots = Robots::parse("User-agent: *\nDisallow: /private\nAllow: /private/ok\n");
        assert!(robots.allowed("/"));
        assert!(!robots.allowed("/private/x"));
        assert!(robots.allowed("/private/ok/1"));
    }

    #[tokio::test]
    async fn rejects_private_literals_and_schemes() {
        for bad in [
            "http://127.0.0.1/",
            "http://[::1]/",
            "http://[::ffff:127.0.0.1]/",
            "http://192.168.1.215/",
            "http://10.0.0.1:80/",
            "file:///etc/passwd",
            "https://example.com:8443/",
        ] {
            assert!(validate(bad).await.is_err(), "{bad} must be rejected");
        }
    }
}
