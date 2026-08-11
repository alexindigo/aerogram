//! Dump a sanitized HTML body to a file for inspection + time the call.
//!   cargo run --bin inspect_html -- <data_dir> [out.html]

use aerogram_proton_core::*;
use std::time::Instant;

fn main() {
    let dir = std::env::args().nth(1).expect("usage: inspect_html <data_dir> [out]");
    let out = std::env::args().nth(2).unwrap_or_else(|| "/tmp/aerogram-sanitized.html".into());
    let core = unsafe { proton_core_new(std::ffi::CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());

    let call = |method: &str, params: &str| -> serde_json::Value {
        let raw = unsafe {
            proton_call(core, std::ffi::CString::new(method).unwrap().as_ptr(),
                        std::ffi::CString::new(params).unwrap().as_ptr())
        };
        let s = unsafe { std::ffi::CStr::from_ptr(raw) }.to_string_lossy().into_owned();
        unsafe { proton_free_string(raw) };
        serde_json::from_str(&s).expect("json")
    };

    call("restore_session", "{}");
    let labels = call("list_labels", "{}");
    for l in labels["ok"].as_array().unwrap() {
        let id = l["id"].as_u64().unwrap();
        let msgs = call("list_messages", &format!(r#"{{"label_id":{id},"limit":15}}"#));
        for m in msgs["ok"].as_array().unwrap() {
            let Some(mid) = m["id"].as_u64() else { continue };
            let t0 = Instant::now();
            let b = call("message_body", &format!(r#"{{"id":{mid}}}"#));
            let elapsed = t0.elapsed();
            let ok = &b["ok"];
            if ok["mime_type"] != "TextHtml" { continue; }
            let html = ok["html"].as_str().unwrap_or("");
            std::fs::write(&out, html).unwrap();
            println!("msg {mid}: body fetch+transform took {:?}; html {} bytes -> {}",
                     elapsed, html.len(), out);
            // head of the doc, tags only
            let head: String = html.chars().take(600).collect();
            println!("--- head 600 chars ---\n{}", head);
            unsafe { proton_core_free(core) };
            return;
        }
    }
    unsafe { proton_core_free(core) };
}
