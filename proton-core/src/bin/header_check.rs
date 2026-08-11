use aerogram_proton_core::*;
fn main() {
    let dir = std::env::args().nth(1).expect("usage: header_check <dir>");
    let core = unsafe { proton_core_new(std::ffi::CString::new(dir).unwrap().as_ptr()) };
    assert!(!core.is_null());
    let call = |m: &str, p: &str| -> serde_json::Value {
        let raw = unsafe { proton_call(core, std::ffi::CString::new(m).unwrap().as_ptr(),
                                       std::ffi::CString::new(p).unwrap().as_ptr()) };
        let s = unsafe { std::ffi::CStr::from_ptr(raw) }.to_string_lossy().into_owned();
        unsafe { proton_free_string(raw) };
        serde_json::from_str(&s).unwrap()
    };
    call("restore_session", "{}");
    let labels = call("list_labels", "{}");
    let id = labels["ok"][0]["id"].as_u64().unwrap();
    let msgs = call("list_messages", &format!(r#"{{"label_id":{id},"limit":1}}"#));
    let mid = msgs["ok"][0]["id"].as_u64().unwrap();
    let b = call("message_body", &format!(r#"{{"id":{mid}}}"#));
    let hdr = b["ok"]["header"].as_str().unwrap_or("");
    println!("header present: {} ({} bytes); has From={} Subject={} Date={}",
             !hdr.is_empty(), hdr.len(),
             hdr.contains("From"), hdr.to_lowercase().contains("subject"), hdr.contains("Date"));
    unsafe { proton_core_free(core) };
}
