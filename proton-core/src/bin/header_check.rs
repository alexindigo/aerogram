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
    let mid = msgs["ok"]["messages"][0]["id"].as_u64().unwrap();
    let b = call("message_body", &format!(r#"{{"id":{mid}}}"#));
    let hdr = b["ok"]["header"].as_str().unwrap_or("");
    println!("header: {} bytes", hdr.len());
    // Print header NAMES + any content-type/mime-version values only
    // (never content): is it the real message headers?
    for line in hdr.lines() {
        if line.starts_with(|c: char| c.is_ascii_alphabetic()) {
            let name = line.split(':').next().unwrap_or("");
            let value = if name.eq_ignore_ascii_case("content-type")
                          || name.eq_ignore_ascii_case("mime-version") {
                line.splitn(2, ':').nth(1).unwrap_or("").trim()
            } else {
                "…"
            };
            println!("  {}: {}", name, value);
        }
    }
    unsafe { proton_core_free(core) };
}
