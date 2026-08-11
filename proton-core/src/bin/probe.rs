//! Manual probe for the proton core: restore the persisted session in
//! the given data dir and dump label/message counts. Read-only.
//!
//!   cargo run --bin probe -- <data_dir>

use aerogram_proton_core::*; // (unused — the lib's API is the C ABI; see below)

fn main() {
    let dir = std::env::args().nth(1).expect("usage: probe <data_dir>");
    println!("probing {}", dir);

    // The lib exposes only the C ABI; drive it like C++ would.
    let core = unsafe {
        proton_core_new(std::ffi::CString::new(dir.clone()).unwrap().as_ptr())
    };
    assert!(!core.is_null(), "core failed to start");

    let call = |method: &str, params: &str| -> serde_json::Value {
        let raw = unsafe {
            proton_call(
                core,
                std::ffi::CString::new(method).unwrap().as_ptr(),
                std::ffi::CString::new(params).unwrap().as_ptr(),
            )
        };
        let s = unsafe { std::ffi::CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned();
        unsafe { proton_free_string(raw) };
        serde_json::from_str(&s).expect("json")
    };

    println!("restore_session: {}", call("restore_session", "{}"));
    let labels = call("list_labels", "{}");
    println!("labels: {}", labels);

    if let Some(arr) = labels.get("ok").and_then(|v| v.as_array()) {
        for l in arr {
            let id = l["id"].as_u64().unwrap();
            let msgs = call("list_messages", &format!(r#"{{"label_id":{id},"limit":10}}"#));
            let count = msgs
                .get("ok")
                .and_then(|v| v.as_array())
                .map(|a| a.len())
                .unwrap_or(0);
            println!("  label {} ({}): {} messages{}", id, l["name"], count,
                     if count == 0 { format!(" — raw: {}", &msgs.to_string()[..msgs.to_string().len().min(200)]) } else { String::new() });
        }
    }

    // BODY-CHECK: fetch the first message of each non-empty label and
    // report mime type + whether HTML tags survive (never print content).
    if let Some(arr) = labels.get("ok").and_then(|v| v.as_array()) {
        for l in arr {
            let id = l["id"].as_u64().unwrap();
            let msgs = call("list_messages", &format!(r#"{{"label_id":{id},"limit":1}}"#));
            if let Some(first) = msgs.get("ok").and_then(|v| v.as_array()).and_then(|a| a.first()) {
                if let Some(mid) = first["id"].as_u64() {
                    let body = call("message_body", &format!(r#"{{"id":{mid}}}"#));
                    if let Some(ok) = body.get("ok") {
                        let text = ok["text"].as_str().unwrap_or("");
                        println!("  body check label {}: mime={} len={} html_tags_left={}",
                                 id, ok["mime_type"], text.len(),
                                 text.contains("<div") || text.contains("<img"));
                    } else {
                        println!("  body check label {}: {}", id, body);
                    }
                }
            }
        }
    }

    unsafe { proton_core_free(core) };
}
