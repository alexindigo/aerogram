//! Resolve label ids → names (both types).
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
    let dir = std::env::args().nth(1).expect("usage: names_probe <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    println!("restore: {}", call(core, "restore_session", "{}"));
    for t in ["folder", "label"] {
        let r = call(core, "list_labels", &format!(r#"{{"label_type":"{t}"}}"#));
        println!("--- {t}s:");
        for l in r["ok"].as_array().unwrap_or(&vec![]) {
            println!("  id={} name={} total={} unread={}",
                     l["id"].as_str().unwrap_or("?"), l["name"].as_str().unwrap_or("?"),
                     l["total"], l["unread"]);
        }
    }
    unsafe { proton_core_free(core) };
}
