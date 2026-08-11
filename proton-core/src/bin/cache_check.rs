//! Prove body caching: same message_body twice, time both.
//!   cargo run --bin cache_check -- <data_dir>

use aerogram_proton_core::*;
use std::time::Instant;

fn main() {
    let dir = std::env::args().nth(1).expect("usage: cache_check <data_dir>");
    let core = unsafe { proton_core_new(std::ffi::CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());

    let call = |method: &str, params: &str| -> (serde_json::Value, std::time::Duration) {
        let t0 = Instant::now();
        let raw = unsafe {
            proton_call(core, std::ffi::CString::new(method).unwrap().as_ptr(),
                        std::ffi::CString::new(params).unwrap().as_ptr())
        };
        let s = unsafe { std::ffi::CStr::from_ptr(raw) }.to_string_lossy().into_owned();
        unsafe { proton_free_string(raw) };
        (serde_json::from_str(&s).expect("json"), t0.elapsed())
    };

    call("restore_session", "{}");
    let (labels, _) = call("list_labels", "{}");
    for l in labels["ok"].as_array().unwrap() {
        let id = l["id"].as_u64().unwrap();
        let (msgs, _) = call("list_messages", &format!(r#"{{"label_id":{id},"limit":5}}"#));
        for m in msgs["ok"]["messages"].as_array().unwrap() {
            let Some(mid) = m["id"].as_u64() else { continue };
            let (b1, t1) = call("message_body", &format!(r#"{{"id":{mid}}}"#));
            let (_b2, t2) = call("message_body", &format!(r#"{{"id":{mid}}}"#));
            if b1["ok"].is_null() { continue; }
            println!("msg {mid}: 1st={:?}  2nd={:?}  (cached if 2nd << 1st)", t1, t2);
            unsafe { proton_core_free(core) };
            return;
        }
    }
    unsafe { proton_core_free(core) };
}
