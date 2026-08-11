//! aerogram-html-sanitize: the ONE email-HTML sanitizer for Aerogram.
//!
//! Wraps Proton's battle-tested `Transformer` pipeline
//! (proton-mail-html-transformer) as a standalone library — no account
//! machinery, no Proton coupling. Used by the C++ app via the C ABI and
//! by proton-core as a normal Rust dependency (rlib).
//!
//! Pipeline: whitelist-strip (scripts/forms/dangerous attrs) →
//! noreferrer links → disable remote + embedded content (tracking
//! pixels, cid:/data: images a text engine can't resolve) →
//! text-engine cleanup (dead <img> tags, zero-width padding).

use std::ffi::{c_char, CStr, CString};

use proton_mail_html_transformer::sanitizer::StripStyleSheets;
use proton_mail_html_transformer::{Html2TextOptions, Transformer};
use serde_json::json;

/// The shared "safe for Qt's text engine" transform, Rust-callable.
/// Returns (sanitized_html, plain_text, blocked_remote_content).
pub fn sanitize_for_display(input: &str) -> (String, String, bool) {
    let mut t = Transformer::new(input);
    t.add_noreferrer();
    t.strip_whitelist(StripStyleSheets::No);
    let out = t.disable_content(true, true);
    let blocked = !out.remote_urls.is_empty() || !out.embedded_urls.is_empty();

    let html = clean_for_text_engine(&t.to_string());
    let plain = Transformer::html2text(
        std::io::Cursor::new(input.as_bytes()),
        Html2TextOptions { decorate_links: true, decorate_images: false },
    )
    .unwrap_or_else(|_| input.to_string());

    (html, plain, blocked)
}

/// Just the plain-text transform (no sanitize pass needed for text).
pub fn to_plain_text(input: &str) -> String {
    Transformer::html2text(
        std::io::Cursor::new(input.as_bytes()),
        Html2TextOptions { decorate_links: true, decorate_images: false },
    )
    .unwrap_or_else(|_| input.to_string())
}

/// Post-sanitize cleanup for Qt's text engine (QTextDocument renders a
/// safe HTML4 subset). Removes:
///
/// 1. Dead `<img>` tags — content-disabling neuters their src but the
///    tag survives, and Qt renders it as a ￼ placeholder box.
/// 2. Zero-width marketing padding (ZWSP/ZWNJ/CGJ/WJ/BOM) — preview-text
///    hacks that show up as stray marks. ZWJ (U+200D) is KEPT: emoji
///    sequences need it.
pub fn clean_for_text_engine(html: &str) -> String {
    let mut out = html.to_string();

    let lower = out.to_lowercase();
    let mut result = String::with_capacity(out.len());
    let mut rest = 0usize;
    while let Some(pos) = lower[rest..].find("<img") {
        let abs = rest + pos;
        if let Some(end) = out[abs..].find('>') {
            result.push_str(&out[rest..abs]);
            rest = abs + end + 1;
        } else {
            break;
        }
    }
    result.push_str(&out[rest..]);
    out = result;

    out.retain(|c| {
        !matches!(c, '\u{200B}' | '\u{200C}' | '\u{034F}' | '\u{2060}' | '\u{FEFF}')
    });

    out
}

// ---------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------
fn cstr_arg<'a>(ptr: *const c_char, what: &str) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err(format!("{what} is null"));
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|e| format!("{what} is not UTF-8: {e}"))
}

fn into_c(value: serde_json::Value) -> *mut c_char {
    CString::new(value.to_string())
        .unwrap_or_else(|_| CString::new(r#"{"err":"nul byte"}"#).unwrap())
        .into_raw()
}

/// Sanitize an HTML document for display. Returns a JSON string:
/// `{"html": "...", "plain": "...", "blocked_remote": bool}`.
/// # Safety: `input` must be a valid NUL-terminated UTF-8 C string.
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
#[no_mangle]
pub unsafe extern "C" fn html_to_plain(input: *const c_char) -> *mut c_char {
    match cstr_arg(input, "input") {
        Ok(html) => into_c(json!({ "plain": to_plain_text(html) })),
        Err(e) => into_c(json!({ "err": e })),
    }
}

/// # Safety: frees a string returned by sanitize_html/html_to_plain.
#[no_mangle]
pub unsafe extern "C" fn sanitize_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
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
}
