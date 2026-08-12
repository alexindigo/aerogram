//! list_labels probe: verify unread counters ride along.
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
    let dir = std::env::args().nth(1).expect("usage: labels_probe <data_dir>");
    let core = unsafe { proton_core_new(CString::new(dir).unwrap().as_ptr()) };
    println!("restore: {}", call(core, "restore_session", "{}"));
    println!("labels: {}", call(core, "list_labels", "{}"));
    unsafe { proton_core_free(core) };
}
