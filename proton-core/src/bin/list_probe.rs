//! What does the listing ACTUALLY return? top-N per label, raw epochs.
use aerogram_proton_core::*;
use std::ffi::{CStr, CString};

fn call(core: *mut ProtonCore, method: &str, params: &str) -> serde_json::Value {
    let raw = unsafe { proton_call(core, CString::new(method).unwrap().as_ptr(),
                                   CString::new(params).unwrap().as_ptr()) };
    let s = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
    unsafe { proton_free_string(raw) };
    serde_json::from_str(&s).expect("json")
}

fn main() {
    let dir = std::env::args().nth(1).expect("usage: list_probe <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    println!("restore: {}", call(core, "restore_session", "{}"));
    for label in ["0", "5"] {
        let r = call(core, "list_messages", &format!(r#"{{"label_id":"{label}","limit":8}}"#));
        println!("--- label {label}: total={}", r["ok"]["total"]);
        for m in r["ok"]["messages"].as_array().unwrap_or(&vec![]) {
            let t = m["time"].as_i64().unwrap_or(0);
            let subj: String = m["subject"].as_str().unwrap_or("?").chars().take(40).collect();
            let labels: String = m["labels"].as_array().map(|a| a.iter()
                .filter_map(|l| l.as_str()).collect::<Vec<_>>().join(","))
                .unwrap_or_else(|| "?".into());
            println!("  {t} | labels={labels} | {subj}");
        }
    }
    unsafe { proton_core_free(core) };
}
