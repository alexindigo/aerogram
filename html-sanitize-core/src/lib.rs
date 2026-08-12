//! aerogram-html-sanitize: the ONE email-HTML sanitizer for Aerogram.
//!
//! Engine: lol-html (Cloudflare's streaming rewriter) — clean bytes
//! flow out as raw bytes arrive. Used by the C++ app via the C ABI,
//! by proton-core as a Rust dependency, and (via SanitizerStream) by
//! the content pipeline for progressive rendering.
//!
//! Policy: remove script/form/iframe/object/embed/link/meta/base
//! elements; strip on* event attributes and javascript: URLs; links
//! get noreferrer/noopener + target=_blank; utm_*/tracker params are
//! stripped from link hrefs; all <img> are removed (remote + embedded
//! content disabled — tracking pixels, cid:/data: images a text engine
//! can't resolve); zero-width marketing padding is stripped from text
//! (ZWJ/FE0F kept: emoji need them).

use std::cell::RefCell;
use std::ffi::{c_char, CStr, CString};
use std::rc::Rc;

use lol_html::{doc_text, element, HtmlRewriter, Settings};
#[cfg(feature = "ffi")]
use serde_json::json;

// ---------------------------------------------------------------------
// The streaming core
// ---------------------------------------------------------------------

/// Sink type for the rewriter: an owned boxed closure (OutputSink is
/// implemented for FnMut(&[u8])). Local (non-Send) handlers are fine —
/// a stream lives on one worker thread.
type Rewriter = HtmlRewriter<'static, Box<dyn FnMut(&[u8])>>;

/// Streaming sanitizer state. Feed chunks with `write()` (returns the
/// newly flushed clean bytes), `finish()` at EOF.
pub struct SanitizerStream {
    rewriter: Option<Rewriter>,  // Option because end() consumes it
    out: Rc<RefCell<Vec<u8>>>,
    out_read: usize,
    blocked: Rc<RefCell<bool>>,
}

fn build_rewriter(
    out: Rc<RefCell<Vec<u8>>>,
    blocked: Rc<RefCell<bool>>,
) -> Rewriter {
    let blocked_img = blocked.clone();
    HtmlRewriter::new(
        Settings::new()
            .with_strict(false)
            // Dangerous / structural elements gone entirely.
            .append_element_content_handler(element!(
                "script, form, iframe, object, embed, link, meta, base",
                |el| {
                    el.remove();
                    Ok(())
                }
            ))
            // No images (remote + embedded disabled); flag when the src
            // was remote/data — the UI shows "remote content blocked".
            .append_element_content_handler(element!("img", move |el| {
                let src = el.get_attribute("src").unwrap_or_default();
                if src.starts_with("http") || src.starts_with("data:") {
                    *blocked_img.borrow_mut() = true;
                }
                el.remove();
                Ok(())
            }))
            // Links: noreferrer + new window; strip tracker params.
            .append_element_content_handler(element!("a", |el| {
                el.set_attribute("rel", "noreferrer noopener")?;
                el.set_attribute("target", "_blank")?;
                if let Some(href) = el.get_attribute("href") {
                    if let Some(clean) = clean_link(&href) {
                        el.set_attribute("href", &clean)?;
                    }
                }
                Ok(())
            }))
            // Event handlers + javascript: URLs gone everywhere.
            .append_element_content_handler(element!("*", |el| {
                // Collect first (attributes() borrows immutably; removal
                // borrows mutably — never both at once).
                let bad: Vec<String> = el.attributes().iter()
                    .filter(|a| {
                        let name = a.name();
                        name.starts_with("on")
                            || (name == "href" || name == "src")
                                && a.value().trim_start().starts_with("javascript:")
                    })
                    .map(|a| a.name())
                    .collect();
                for name in bad {
                    el.remove_attribute(&name);
                }
                Ok(())
            }))
            // Zero-width marketing padding stripped from ALL text nodes
            // (ZWJ U+200D and FE0F kept — emoji need them). Per-chunk
            // filter: the chars are self-contained, chunking is safe.
            // doc_text! is document-level (text!("*") misses text not
            // wrapped in an element).
            .append_document_content_handler(doc_text!(|t| {
                let s = t.as_str();
                if s.contains(['\u{200B}', '\u{200C}', '\u{034F}', '\u{2060}', '\u{FEFF}']) {
                    let filtered: String = s.chars()
                        .filter(|c| !matches!(c,
                            '\u{200B}' | '\u{200C}' | '\u{034F}' | '\u{2060}' | '\u{FEFF}'))
                        .collect();
                    t.set_str(filtered);
                }
                Ok(())
            })),
        {
            let out = out.clone();
            Box::new(move |chunk: &[u8]| out.borrow_mut().extend_from_slice(chunk))
                as Box<dyn FnMut(&[u8])>
        },
    )
}

impl SanitizerStream {
    pub fn new() -> Self {
        let out = Rc::new(RefCell::new(Vec::new()));
        let blocked = Rc::new(RefCell::new(false));
        let rewriter = build_rewriter(out.clone(), blocked.clone());
        Self { rewriter: Some(rewriter), out, out_read: 0, blocked }
    }

    /// Feed raw bytes; returns the clean bytes flushed since the last
    /// call (lol-html emits as it parses — this is the stream).
    pub fn write(&mut self, chunk: &[u8]) -> Result<Vec<u8>, String> {
        self.rewriter
            .as_mut()
            .ok_or("write after finish")?
            .write(chunk)
            .map_err(|e| format!("sanitize stream write: {e:?}"))?;
        let out = self.out.borrow();
        let fresh = out[self.out_read..].to_vec();
        self.out_read = out.len();
        Ok(fresh)
    }

    /// Finish; returns remaining clean bytes + the blocked flag.
    pub fn finish(&mut self) -> Result<(Vec<u8>, bool), String> {
        self.rewriter
            .take()
            .ok_or("finish called twice")?
            .end()
            .map_err(|e| format!("sanitize stream end: {e:?}"))?;
        let out = self.out.borrow();
        let rest = out[self.out_read..].to_vec();
        self.out_read = out.len();
        Ok((rest, *self.blocked.borrow()))
    }
}

/// Strip tracker query params (utm_*, fbclid, gclid, …) from a link
/// target. Returns None when the link needs no change.
fn clean_link(href: &str) -> Option<String> {
    const TRACKERS: &[&str] = &[
        "fbclid", "gclid", "dclid", "msclkid", "mc_cid", "mc_eid", "igshid", "ref",
    ];
    let q = href.find('?')?;
    let (base, query) = href.split_at(q);
    let kept: Vec<&str> = query[1..]
        .split('&')
        .filter(|pair| {
            let key = pair.split('=').next().unwrap_or("");
            let lk = key.to_ascii_lowercase();
            !lk.starts_with("utm_") && !TRACKERS.contains(&lk.as_str())
        })
        .collect();
    if kept.len() == query[1..].split('&').count() {
        return None; // nothing stripped
    }
    if kept.is_empty() {
        return Some(base.to_string());
    }
    Some(format!("{}?{}", base, kept.join("&")))
}

// ---------------------------------------------------------------------
// One-shot API (unchanged surface)
// ---------------------------------------------------------------------

/// The shared "safe for Qt's text engine" transform.
/// Returns (sanitized_html, plain_text, blocked_remote_content).
pub fn sanitize_for_display(input: &str) -> (String, String, bool) {
    let mut stream = SanitizerStream::new();
    let mut html = match stream.write(input.as_bytes()) {
        Ok(c) => c,
        Err(_) => Vec::new(),
    };
    let (mut rest, blocked) = stream.finish().unwrap_or((Vec::new(), false));
    html.append(&mut rest);
    let html = String::from_utf8_lossy(&html).into_owned();

    let plain = html2text::from_read(input.as_bytes(), 80)
        .unwrap_or_else(|_| input.to_string());

    (html, plain, blocked)
}

/// Just the plain-text transform (no sanitize pass needed for text).
pub fn to_plain_text(input: &str) -> String {
    html2text::from_read(input.as_bytes(), 80)
        .unwrap_or_else(|_| input.to_string())
}

// ---------------------------------------------------------------------
// C ABI (built unless the `ffi` feature is off — proton-core uses the
// Rust API and must not re-export these symbols into its staticlib)
// ---------------------------------------------------------------------
#[cfg(feature = "ffi")]
fn cstr_arg<'a>(ptr: *const c_char, what: &str) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err(format!("{what} is null"));
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|e| format!("{what} is not UTF-8: {e}"))
}

#[cfg(feature = "ffi")]
fn into_c(value: serde_json::Value) -> *mut c_char {
    CString::new(value.to_string())
        .unwrap_or_else(|_| CString::new(r#"{"err":"nul byte"}"#).unwrap())
        .into_raw()
}

/// Sanitize an HTML document for display. Returns a JSON string:
/// `{"html": "...", "plain": "...", "blocked_remote": bool}`.
/// # Safety: `input` must be a valid NUL-terminated UTF-8 C string.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_html(input: *const c_char) -> *mut c_char {
    match cstr_arg(input, "input") {
        Ok(html) => {
            let (h, p, blocked) = sanitize_for_display(html);
            into_c(json!({ "html": h, "plain": p, "blocked_remote": blocked }))
        }
        Err(e) => into_c(json!({ "err": e })),
    }
}

/// Plain-text-only transform. Returns a JSON string `{"plain": "..."}`.
/// # Safety: `input` must be a valid NUL-terminated UTF-8 C string.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn html_to_plain(input: *const c_char) -> *mut c_char {
    match cstr_arg(input, "input") {
        Ok(html) => into_c(json!({ "plain": to_plain_text(html) })),
        Err(e) => into_c(json!({ "err": e })),
    }
}

/// # Safety: frees a string returned by sanitize_html/html_to_plain.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

// ---------------------------------------------------------------------
// Streaming C ABI — clean bytes flush as raw bytes arrive.
// ---------------------------------------------------------------------

/// Create a streaming sanitizer. Free with sanitize_stream_free (or
/// finish, which frees).
/// # Safety: the returned pointer must not be used after free/finish.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_stream_new() -> *mut SanitizerStream {
    Box::into_raw(Box::new(SanitizerStream::new()))
}

/// Feed a raw chunk; returns the newly flushed clean bytes as a JSON
/// string `{"chunk": "..."}` (may be empty — lol-html buffers across
/// tag boundaries).
/// # Safety: `s` from sanitize_stream_new; `chunk` a valid C string.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_stream_write(
    s: *mut SanitizerStream,
    chunk: *const c_char,
) -> *mut c_char {
    let Some(stream) = s.as_mut() else {
        return into_c(json!({ "err": "stream is null" }));
    };
    let Ok(text) = cstr_arg(chunk, "chunk") else {
        return into_c(json!({ "err": "chunk not UTF-8" }));
    };
    match stream.write(text.as_bytes()) {
        Ok(fresh) => into_c(json!({ "chunk": String::from_utf8_lossy(&fresh) })),
        Err(e) => into_c(json!({ "err": e })),
    }
}

/// Finish the stream; returns `{"chunk": "...", "blocked_remote": bool}`
/// and FREES the stream (do not use the pointer after).
/// # Safety: `s` from sanitize_stream_new, not yet finished/freed.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_stream_finish(s: *mut SanitizerStream) -> *mut c_char {
    let Some(stream) = s.as_mut() else {
        return into_c(json!({ "err": "stream is null" }));
    };
    let result = stream.finish();
    drop(Box::from_raw(s));
    match result {
        Ok((rest, blocked)) => into_c(json!({
            "chunk": String::from_utf8_lossy(&rest),
            "blocked_remote": blocked,
        })),
        Err(e) => into_c(json!({ "err": e })),
    }
}

/// Abort and free without finishing.
/// # Safety: `s` from sanitize_stream_new, not yet finished.
#[cfg(feature = "ffi")]
#[no_mangle]
pub unsafe extern "C" fn sanitize_stream_free(s: *mut SanitizerStream) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn poison_is_stripped() {
        let dirty = r#"<html><head><style>body{x}</style></head><body>
            <p onclick="x()">Hi</p>
            <script>alert(1)</script>
            <form action="http://evil"></form>
            <img src="https://tracker.example/pixel.gif">
            <img src="data:image/png;base64,AAAA">
            <a href="https://example.com/?utm_source=track">link</a>
            </body></html>"#;
        let (html, _plain, blocked) = sanitize_for_display(dirty);
        assert!(!html.contains("<script"), "script survived: {html}");
        assert!(!html.contains("<form"), "form survived");
        assert!(!html.contains("onclick"), "event handler survived");
        assert!(!html.contains("tracker.example"), "remote pixel survived");
        assert!(!html.contains("data:image"), "embedded data img survived");
        assert!(!html.contains("<img"), "dead img tag survived");
        assert!(blocked, "blocked flag should be set");
    }

    #[test]
    fn zero_width_padding_gone_but_emoji_survive() {
        let s = "pad\u{200C}ded\u{034F} \u{FEFF}text 👨\u{200D}💻 ok ❤\u{FE0F}";
        let (html, _, _) = sanitize_for_display(s);
        assert!(html.contains("padded"));
        assert!(!html.contains('\u{200C}'));
        assert!(html.contains("👨\u{200D}💻")); // ZWJ emoji intact
        assert!(html.contains("❤\u{FE0F}"));   // variation selector intact
    }

    #[test]
    fn plain_text_transform() {
        let (_h, plain, _) =
            sanitize_for_display("<p>Hello <b>world</b></p><p>line two</p>");
        assert!(plain.contains("Hello world"));
        assert!(plain.contains("line two"));
    }

    #[test]
    fn tracker_params_stripped_from_links() {
        let (html, _, _) = sanitize_for_display(
            r#"<a href="https://example.com/page?utm_source=nl&id=42&fbclid=x">x</a>"#,
        );
        assert!(html.contains("id=42"), "real param lost: {html}");
        assert!(!html.contains("utm_source"), "utm param survived: {html}");
        assert!(!html.contains("fbclid"), "fbclid survived: {html}");
    }

    #[test]
    fn streaming_matches_one_shot() {
        let dirty = r#"<p>Hello <b>world</b></p><script>x()</script><p>tail</p>"#;
        let (one_shot, _, _) = sanitize_for_display(dirty);

        let mut stream = SanitizerStream::new();
        let mut collected = Vec::new();
        for chunk in dirty.as_bytes().chunks(7) {  // awkward size on purpose
            collected.extend(stream.write(chunk).unwrap());
        }
        let (rest, _) = stream.finish().unwrap();
        collected.extend(rest);
        let streamed = String::from_utf8_lossy(&collected).into_owned();

        assert_eq!(one_shot, streamed);
        assert!(streamed.contains("Hello"));
        assert!(!streamed.contains("<script"));
    }
}
