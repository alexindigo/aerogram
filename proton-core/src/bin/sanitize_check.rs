//! Verify the sanitize pipeline on the first HTML message in the store.
//! Prints only safety metadata, never content.
//!   cargo run --bin sanitize_check -- <data_dir>

use aerogram_proton_core::*;

fn main() {
    let dir = std::env::args().nth(1).expect("usage: sanitize_check <data_dir>");
    let core = unsafe { proton_core_new(std::ffi::CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());

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

    println!("restore: {}", call("restore_session", "{}"));
    let labels = call("list_labels", "{}");
    'outer: for l in labels["ok"].as_array().unwrap() {
        let id = l["id"].as_u64().unwrap();
        let msgs = call("list_messages", &format!(r#"{{"label_id":{id},"limit":15}}"#));
        for m in msgs["ok"].as_array().unwrap() {
            let Some(mid) = m["id"].as_u64() else { continue };
            let b = call("message_body", &format!(r#"{{"id":{mid}}}"#));
            let ok = &b["ok"];
            if ok["mime_type"] != "TextHtml" {
                continue;
            }
            let html = ok["html"].as_str().unwrap_or("");
            let text = ok["text"].as_str().unwrap_or("");
            println!(
                "HTML msg {mid}: html_len={} text_len={} blocked_remote={} script={} remote_img={} form={} style_attr={}",
                html.len(),
                text.len(),
                ok["blocked_remote"],
                html.contains("<script"),
                html.contains("src=\"http"),
                html.contains("<form"),
                html.contains("style=")
            );
            break 'outer;
        }
    }

    unsafe { proton_core_free(core) };
}
