//! Readable-text extraction: drop chrome (scripts, nav, forms) and keep the
//! main content when the page has one.

use ego_tree::NodeRef;
use scraper::{Html, Node, Selector};

const DROP_TAGS: &[&str] = &[
    "script",
    "style",
    "noscript",
    "template",
    "svg",
    "iframe",
    "canvas",
    "object",
    "embed",
    "nav",
    "header",
    "footer",
    "aside",
    "form",
    "button",
    "select",
    "textarea",
    "label",
    "input",
];

const BLOCK_TAGS: &[&str] = &[
    "address",
    "article",
    "blockquote",
    "br",
    "dd",
    "div",
    "dl",
    "dt",
    "figcaption",
    "figure",
    "h1",
    "h2",
    "h3",
    "h4",
    "h5",
    "h6",
    "hr",
    "li",
    "main",
    "ol",
    "p",
    "pre",
    "section",
    "table",
    "tbody",
    "td",
    "th",
    "thead",
    "tr",
    "ul",
];

fn collect(node: NodeRef<'_, Node>, buf: &mut String) {
    match node.value() {
        Node::Element(el) => {
            let name = el.name();
            if DROP_TAGS.contains(&name) {
                return;
            }
            let block = BLOCK_TAGS.contains(&name);
            if block {
                buf.push('\n');
            }
            for child in node.children() {
                collect(child, buf);
            }
            if block {
                buf.push('\n');
            }
        }
        Node::Text(text) => {
            buf.push_str(&text.text);
            buf.push(' ');
        }
        _ => {
            for child in node.children() {
                collect(child, buf);
            }
        }
    }
}

fn collect_text(root: NodeRef<'_, Node>) -> String {
    let mut buf = String::new();
    collect(root, &mut buf);
    let mut lines: Vec<String> = Vec::new();
    for line in buf.split('\n') {
        let line = line.split_whitespace().collect::<Vec<_>>().join(" ");
        if line.is_empty() {
            continue;
        }
        if lines.last().map(|l| l == &line).unwrap_or(false) {
            continue;
        }
        lines.push(line);
    }
    lines.join("\n")
}

/// Returns (title, text).
pub fn extract(html: &str) -> (String, String) {
    let doc = Html::parse_document(html);

    let title = Selector::parse("title")
        .ok()
        .and_then(|sel| doc.select(&sel).next())
        .map(|node| {
            node.text()
                .collect::<Vec<_>>()
                .join(" ")
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ")
        })
        .unwrap_or_default();

    // Prefer a real content container when it holds enough text.
    let mut best = String::new();
    if let Ok(sel) = Selector::parse("article, main, [role=main], .post-content, .article-body, #content")
    {
        for node in doc.select(&sel) {
            let text = collect_text(*node);
            if text.chars().count() > best.chars().count() {
                best = text;
            }
        }
    }
    if best.chars().count() < 400 {
        if let Ok(sel) = Selector::parse("body") {
            if let Some(node) = doc.select(&sel).next() {
                let text = collect_text(*node);
                if text.chars().count() > best.chars().count() {
                    best = text;
                }
            }
        }
    }
    if best.is_empty() {
        if let Ok(sel) = Selector::parse("html") {
            if let Some(node) = doc.select(&sel).next() {
                best = collect_text(*node);
            }
        }
    }

    (title, best)
}

#[cfg(test)]
mod tests {
    use super::extract;

    #[test]
    fn drops_scripts_and_keeps_text() {
        let html = "<html><head><title> T 1 </title></head><body>\
            <script>var x = 1;</script><nav>menu</nav>\
            <article><p>Hello world.</p><p>Second line.</p></article>\
            <footer>bye</footer></body></html>";
        let (title, text) = extract(html);
        assert_eq!(title, "T 1");
        assert!(text.contains("Hello world."));
        assert!(text.contains("Second line."));
        assert!(!text.contains("var x"));
        assert!(!text.contains("menu"));
        assert!(!text.contains("bye"));
    }
}
