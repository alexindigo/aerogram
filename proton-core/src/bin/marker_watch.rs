//! Watch the events-latest marker for drift.
use aerogram_proton_core::*;
use std::ffi::{CStr, CString};

fn call(core: *mut ProtonCore, method: &str, params: &str) -> String {
    let raw = unsafe { proton_call(core, CString::new(method).unwrap().as_ptr(),
                                   CString::new(params).unwrap().as_ptr()) };
    let s = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
    unsafe { proton_free_string(raw) };
    s
}

fn main() {
    let dir = std::env::args().nth(1).expect("usage: marker_watch <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    println!("restore: {}", call(core, "restore_session", "{}"));
    for i in 0..6 {
        // wait_event returns [] on no-change; we print raw each 15s
        let out = call(core, "wait_event", r#"{"timeout_ms": 15000}"#);
        println!("#{i}: {out}");
    }
    unsafe { proton_core_free(core) };
}
