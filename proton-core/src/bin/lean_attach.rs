//! Lean-core attachment probe: restore → find a message WITH
//! attachments → fetch + decrypt one via the FFI (the app's path).
//! Prints only metadata + byte count, never content.
//!   cargo run --bin lean_attach -- <data_dir>

use aerogram_proton_core::*;
use std::ffi::CString;

fn call(core: *mut ProtonCore, method: &str, params: &str) -> serde_json::Value {
    let raw = unsafe {
        proton_call(core, CString::new(method).unwrap().as_ptr(),
                    CString::new(params).unwrap().as_ptr())
    };
    let s = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
    unsafe { proton_free_string(raw) };
    serde_json::from_str(&s).expect("json")
}

use std::ffi::CStr;

fn main() {
    let dir = std::env::args().nth(1).expect("usage: lean_attach <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());

    println!("restore: {}", call(core, "restore_session", "{}"));

    // scan INBOX pages for a message with attachments
    let mut target = None;
    let mut page = 0u64;
    while target.is_none() && page < 5 {
        let r = call(core, "list_messages",
                     &format!(r#"{{"label_id":"0","limit":50,"page":{page}}}"#));
        let Some(msgs) = r["ok"]["messages"].as_array() else { break };
        if msgs.is_empty() { break }
        for m in msgs {
            if m["attachments"].as_u64().unwrap_or(0) > 0 {
                target = Some(m["id"].as_str().unwrap().to_string());
                break;
            }
        }
        page += 1;
    }
    let Some(mid) = target else {
        println!("no attachment-bearing message found in first INBOX pages");
        unsafe { proton_core_free(core) };
        return;
    };
    println!("message with attachments: {}", &mid[..24.min(mid.len())]);

    let body = call(core, "message_body", &format!(r#"{{"id":"{mid}"}}"#));
    let atts = body["ok"]["attachments"].as_array().cloned().unwrap_or_default();
    println!("attachments on it: {}", atts.len());
    for a in &atts {
        println!("  {} ({} bytes, {})", a["name"], a["size"], a["mime"]);
    }
    if let Some(first) = atts.first() {
        let id = first["id"].as_str().unwrap();
        let got = call(core, "get_attachment", &format!(r#"{{"id":"{id}"}}"#));
        if let Some(b64) = got["ok"]["bytes_base64"].as_str() {
            // decode length only
            println!("get_attachment: OK, {} base64 chars ({} bytes)",
                     b64.len(), b64.len() * 3 / 4);
            println!("ATTACH-OK");
        } else {
            println!("get_attachment FAILED: {}", got);
        }
    }
    unsafe { proton_core_free(core) };
}
