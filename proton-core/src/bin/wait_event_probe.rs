//! Probe the lean core's wait_event: restore session, then call
//! wait_event several times, printing raw results/errors.
//!   cargo run --bin wait_event_probe -- <data_dir>

use aerogram_proton_core::*;
use std::ffi::{CStr, CString};

fn call(core: *mut ProtonCore, method: &str, params: &str) -> String {
    let raw = unsafe {
        proton_call(core, CString::new(method).unwrap().as_ptr(),
                    CString::new(params).unwrap().as_ptr())
    };
    let s = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
    unsafe { proton_free_string(raw) };
    s
}

fn main() {
    let dir = std::env::args().nth(1).expect("usage: wait_event_probe <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());
    println!("restore: {}", call(core, "restore_session", "{}"));
    for i in 0..3 {
        let out = call(core, "wait_event", r#"{"timeout_ms": 2000}"#);
        let trimmed: String = out.chars().take(300).collect();
        println!("wait_event #{i}: {trimmed}");
    }
    unsafe { proton_core_free(core) };
}
