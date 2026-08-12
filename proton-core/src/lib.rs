//! aerogram-proton-core: Aerogram's Proton Mail backend core.
//!
//! LEAN CORE: Proton's crates as libraries, their sync/storage engine
//! dropped. We use:
//!   - proton_core_common::Context  — SRP login + session persistence
//!     (small accounts/sessions DB, NO message stash)
//!   - proton_mail_api              — raw endpoints (get_messages /
//!     get_message / get_attachment / get_labels / events)
//!   - proton_crypto_inbox          — body + attachment decryption
//! …and WE own sync (a poll of the events feed) and storage (the app's
//! EmailStore). One copy of mail on disk — ours.
//!
//! The C ABI is unchanged: proton_core_new / proton_call /
//! proton_core_free / proton_free_string, with JSON in/out.
//!
//! Ids are REMOTE ids throughout (no local stash u64s): labels and
//! messages are keyed by their Proton remote ids.

use std::ffi::{c_char, CStr, CString};
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

use secrecy::{ExposeSecret, SecretString};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::runtime::Runtime;
use tokio::sync::Mutex;

use proton_account_api::login::LoginFlow;
use proton_account_api::shared::challenge::ChallengeInfo;
use proton_core_api::services::proton::LabelId;
use proton_core_common::datatypes::{ApiConfig, AppDetails};
use proton_core_common::device::DynDeviceInfoProvider;
use proton_core_common::event_loop::EventPollMode;
use proton_core_common::migration_snooper::NoopMigrationSnooper;
use proton_core_common::os::{KeyChain, KeyChainEntryKind, KeyChainError};
use proton_core_common::post_login_check::DefaultPostLoginValidator;
use proton_core_common::services::DeviceInfoService;
use proton_core_common::Origin;
use proton_crypto_inbox::message::DecryptableMessage;
use proton_crypto_inbox::proton_crypto;
use proton_issue_reporter_service::NoopIssueReporter;
use proton_log_service::LogService;
use proton_mail_api::services::proton::common::MessageId;
use proton_mail_api::services::proton::requests::GetMessagesOptions;
use proton_mail_api::services::proton::ProtonMail;
use proton_core_api::services::proton::ProtonCore as ProtonCoreApi;

// ---------------------------------------------------------------------
// Tokio runtime: one multi-thread runtime per process, created lazily.
// ---------------------------------------------------------------------
fn runtime() -> &'static Runtime {
    static RT: OnceLock<Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .enable_all()
            .build()
            .expect("tokio runtime")
    })
}

// ---------------------------------------------------------------------
// File keychain (session DB encryption key must persist across restarts)
// ---------------------------------------------------------------------
struct FileKeyChain {
    dir: PathBuf,
}

impl FileKeyChain {
    fn new(dir: PathBuf) -> Self {
        std::fs::create_dir_all(&dir).ok();
        Self { dir }
    }
    fn path(&self, kind: KeyChainEntryKind) -> PathBuf {
        let name = match kind {
            KeyChainEntryKind::EncryptionKey => "encryption",
            KeyChainEntryKind::DeviceKey => "device",
            KeyChainEntryKind::PinHash => "pin_hash",
        };
        self.dir.join(format!("{name}.key"))
    }
}

impl KeyChain for FileKeyChain {
    fn store_entry(&self, kind: KeyChainEntryKind, key: SecretString) -> Result<(), KeyChainError> {
        let path = self.path(kind);
        std::fs::write(&path, key.expose_secret().as_bytes())
            .map_err(|e| KeyChainError::new(Box::new(e)))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).ok();
        }
        Ok(())
    }
    fn delete_entry(&self, kind: KeyChainEntryKind) -> Result<(), KeyChainError> {
        let path = self.path(kind);
        if path.exists() {
            std::fs::remove_file(&path).map_err(|e| KeyChainError::new(Box::new(e)))?;
        }
        Ok(())
    }
    fn load_entry(&self, kind: KeyChainEntryKind) -> Result<Option<SecretString>, KeyChainError> {
        let path = self.path(kind);
        if !path.exists() {
            return Ok(None);
        }
        let s = std::fs::read_to_string(&path).map_err(|e| KeyChainError::new(Box::new(e)))?;
        Ok(Some(SecretString::from(s)))
    }
}

// ---------------------------------------------------------------------
// Core state
// ---------------------------------------------------------------------
type CoreContext = proton_core_common::Context;
type UserCtx = proton_core_common::UserContext;
type ApiSession = proton_core_api::session::Session;
type CoreSession = proton_core_common::db::account::CoreSession;

#[derive(Default)]
struct CoreState {
    login_flow: Option<LoginFlow>,
    user_ctx: Option<Arc<UserCtx>>,
    api: Option<ApiSession>,
    last_event_id: Option<String>,
}

pub struct ProtonCore {
    ctx: Arc<CoreContext>,
    state: Mutex<CoreState>,
}

async fn create_context(data_dir: &Path) -> Result<Arc<CoreContext>, String> {
    let session_db = data_dir.join("session");
    let user_db = data_dir.join("user");
    let core_cache = data_dir.join("core_cache");
    for d in [&session_db, &user_db, &core_cache] {
        std::fs::create_dir_all(d).map_err(|e| format!("mkdir {}: {e}", d.display()))?;
    }

    let log_config = proton_log_service::Config::builder()
        .name("aerogram-proton".into())
        .directory(data_dir.join("logs"))
        .build();

    let api_config = ApiConfig {
        app_details: AppDetails {
            platform: "linux".into(),
            product: "mail".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        },
        user_agent: None,
        env_id: proton_core_api::session::EnvId::new_prod(),
        proxy: None,
        resolver: None,
    };

    CoreContext::new(
        Origin::App,
        runtime().handle().clone(),
        session_db,
        user_db,
        Arc::new(FileKeyChain::new(data_dir.join("keychain"))),
        vec![],  // no per-user DB initializers — no stash
        api_config,
        None,    // hv challenge notifier
        None::<DynDeviceInfoProvider>,  // device info provider
        core_cache,
        LogService::new(log_config),
        EventPollMode::Manual,  // WE poll events; no foreign event loop
        Default::default(),
        Arc::new(NoopIssueReporter),
        proton_core_common::services::global_feature_flags::FeatureFlagsBackgroundTask::Disabled,
    )
    .await
    .map_err(|e| format!("core Context::new: {e:?}"))
}

// ---------------------------------------------------------------------
// FFI plumbing
// ---------------------------------------------------------------------
fn cstr_arg<'a>(ptr: *const c_char, what: &str) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err(format!("{what} is null"));
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|e| format!("{what} is not UTF-8: {e}"))
}

fn into_c(value: Value) -> *mut c_char {
    let s = value.to_string();
    CString::new(s)
        .unwrap_or_else(|_| CString::new(r#"{"err":"nul byte in payload"}"#).unwrap())
        .into_raw()
}

fn ok(value: Value) -> *mut c_char {
    into_c(json!({ "ok": value }))
}

fn err(message: impl Into<String>) -> *mut c_char {
    into_c(json!({ "err": message.into() }))
}

#[no_mangle]
pub unsafe extern "C" fn proton_core_new(data_dir: *const c_char) -> *mut ProtonCore {
    let dir = match cstr_arg(data_dir, "data_dir") {
        Ok(d) => PathBuf::from(d),
        Err(e) => {
            eprintln!("proton_core_new: {e}");
            return std::ptr::null_mut();
        }
    };
    match runtime().block_on(create_context(&dir)) {
        Ok(ctx) => Box::into_raw(Box::new(ProtonCore {
            ctx,
            state: Mutex::new(CoreState::default()),
        })),
        Err(e) => {
            eprintln!("proton_core_new: {e}");
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn proton_core_free(core: *mut ProtonCore) {
    if !core.is_null() {
        drop(Box::from_raw(core));
    }
}

#[no_mangle]
pub unsafe extern "C" fn proton_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[no_mangle]
pub unsafe extern "C" fn proton_call(
    core: *mut ProtonCore,
    method: *const c_char,
    params: *const c_char,
) -> *mut c_char {
    let core = match core.as_ref() {
        Some(c) => c,
        None => return err("core is null"),
    };
    let method = match cstr_arg(method, "method") {
        Ok(m) => m,
        Err(e) => return err(e),
    };
    let params_raw = match cstr_arg(params, "params") {
        Ok(p) => p,
        Err(e) => return err(e),
    };
    let params: Value = match serde_json::from_str(params_raw) {
        Ok(p) => p,
        Err(e) => return err(format!("params JSON: {e}")),
    };
    match runtime().block_on(dispatch(core, method, params)) {
        Ok(v) => ok(v),
        Err(e) => err(e),
    }
}

// ---------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------
async fn dispatch(core: &ProtonCore, method: &str, params: Value) -> Result<Value, String> {
    match method {
        "ping" => Ok(Value::String("pong".into())),
        "login" => login(core, params).await,
        "submit_totp" => submit_totp(core, params).await,
        "submit_mailbox_password" => submit_mailbox_password(core, params).await,
        "restore_session" => restore_session(core).await,
        "account_info" => account_info(core).await,
        "list_labels" => list_labels(core).await,
        "list_messages" => list_messages(core, params).await,
        "message_body" => message_body(core, params).await,
        "get_attachment" => get_attachment(core, params).await,
        "wait_event" => wait_event(core, params).await,
        _ => Err(format!("unknown method: {method}")),
    }
}

// ---------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------
#[derive(Deserialize)]
struct LoginParams {
    user: String,
    password: String,
}

fn flow_state(flow: &LoginFlow) -> &'static str {
    if flow.is_awaiting_2fa() {
        "totp"
    } else if flow.is_awaiting_mailbox_password() {
        "mailbox_password"
    } else {
        "ok"
    }
}

/// Login flow finished: promote to a user context + api session.
async fn finish_login(core: &ProtonCore) -> Result<Value, String> {
    let flow = core.state.lock().await.login_flow.take();
    let mut flow = flow.ok_or("no login flow in progress")?;
    // The completed flow persisted the session; re-read it and build
    // the user context the lean way (core Context has no
    // user_context_from_login_flow).
    let session = core
        .ctx
        .get_authenticated_sessions()
        .await
        .map_err(|e| format!("get_authenticated_sessions: {e:?}"))?
        .next()
        .ok_or("no session after login")?;
    let user_ctx = core
        .ctx
        .user_context_from_session(&session)
        .await
        .map_err(|e| format!("user_context_from_session: {e:?}"))?;
    let api = core
        .ctx
        .new_api_session(Some(&session))
        .await
        .map_err(|e| format!("new_api_session: {e:?}"))?;
    let addr = account_email(core, &user_ctx).await;
    {
        let mut st = core.state.lock().await;
        st.user_ctx = Some(user_ctx);
        st.api = Some(api);
    }
    Ok(json!({ "state": "ok", "addr": addr }))
}

async fn login(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let p: LoginParams =
        serde_json::from_value(params).map_err(|e| format!("login params: {e}"))?;

    // Assemble the login flow manually (the pieces MailContext::new_login_flow
    // wired), minus the dropped mail layer's snooper/validator.
    let _ = core
        .ctx
        .get_encryption_key()
        .map_err(|e| format!("encryption key: {e:?}"))?;
    let session = core
        .ctx
        .new_api_session(None)
        .await
        .map_err(|e| format!("new_api_session: {e:?}"))?;
    let device_info = core
        .ctx
        .get_service::<DeviceInfoService>()
        .get_device_info()
        .await;
    let challenge_info = ChallengeInfo {
        product_name: core.ctx.get_client_id(),
        device_info,
        recovery_behavior: None,
        username_behavior: None,
    };
    let mut flow = LoginFlow::new(
        session,
        challenge_info,
        Box::new(NoopMigrationSnooper),
        Box::new(DefaultPostLoginValidator::new(None, Arc::clone(&core.ctx))),
    );
    flow.login_with_credentials(p.user, p.password, None)
        .await
        .map_err(|e| format!("login: {e:?}"))?;

    let st = flow_state(&flow);
    core.state.lock().await.login_flow = Some(flow);
    if st == "ok" {
        finish_login(core).await
    } else {
        Ok(json!({ "state": st }))
    }
}

async fn submit_totp(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let code = params
        .get("code")
        .and_then(Value::as_str)
        .ok_or("missing code")?
        .to_owned();
    {
        let mut state = core.state.lock().await;
        let flow = state.login_flow.as_mut().ok_or("no login flow in progress")?;
        flow.submit_totp(code)
            .await
            .map_err(|e| format!("submit_totp: {e:?}"))?;
    }
    let st = {
        let state = core.state.lock().await;
        let flow = state.login_flow.as_ref().ok_or("login flow lost")?;
        flow_state(flow)
    };
    if st == "ok" {
        finish_login(core).await
    } else {
        Ok(json!({ "state": st }))
    }
}

async fn submit_mailbox_password(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let password = params
        .get("password")
        .and_then(Value::as_str)
        .ok_or("missing password")?
        .to_owned();
    {
        let mut state = core.state.lock().await;
        let flow = state.login_flow.as_mut().ok_or("no login flow in progress")?;
        flow.submit_mailbox_password(password)
            .await
            .map_err(|e| format!("submit_mailbox_password: {e:?}"))?;
    }
    let st = {
        let state = core.state.lock().await;
        let flow = state.login_flow.as_ref().ok_or("login flow lost")?;
        flow_state(flow)
    };
    if st == "ok" {
        finish_login(core).await
    } else {
        Ok(json!({ "state": st }))
    }
}

/// Resume a persisted session (no password) after app restart.
async fn restore_session(core: &ProtonCore) -> Result<Value, String> {
    let session = core
        .ctx
        .get_authenticated_sessions()
        .await
        .map_err(|e| format!("get_authenticated_sessions: {e:?}"))?
        .next()
        .ok_or("no stored session")
        .map_err(str::to_string);
    let session = match session {
        Ok(s) => s,
        Err(_) => return Ok(json!({ "state": "none" })),
    };
    let user_ctx = core
        .ctx
        .user_context_from_session(&session)
        .await
        .map_err(|e| format!("user_context_from_session: {e:?}"))?;
    let api = core
        .ctx
        .new_api_session(Some(&session))
        .await
        .map_err(|e| format!("new_api_session: {e:?}"))?;
    let addr = account_email(core, &user_ctx).await;
    {
        let mut st = core.state.lock().await;
        st.user_ctx = Some(user_ctx);
        st.api = Some(api);
    }
    Ok(json!({ "state": "ok", "addr": addr }))
}

// ---------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------
async fn api(core: &ProtonCore) -> Result<ApiSession, String> {
    core.state
        .lock()
        .await
        .api
        .clone()
        .ok_or_else(|| "not logged in".to_string())
}

async fn user_ctx(core: &ProtonCore) -> Result<Arc<UserCtx>, String> {
    core.state
        .lock()
        .await
        .user_ctx
        .clone()
        .ok_or_else(|| "not logged in".to_string())
}

async fn account_info(core: &ProtonCore) -> Result<Value, String> {
    let ctx = user_ctx(core).await?;
    let email = account_email(core, &ctx).await;
    Ok(json!({ "email": email }))
}

/// The account's address via the account record (core Context has no
/// UserContext::user accessor — the address lives on CoreAccount).
async fn account_email(core: &ProtonCore, uctx: &Arc<UserCtx>) -> String {
    core.ctx
        .get_account(uctx.user_id().clone())
        .await
        .ok()
        .flatten()
        .map(|a| a.name_or_addr)
        .unwrap_or_default()
}

// ---------------------------------------------------------------------
// Labels / messages
// ---------------------------------------------------------------------

/// System labels with their REMOTE ids (Proton label ids are stable
/// strings: "0"=INBOX etc.). Custom folders/labels come from the API.
async fn list_labels(core: &ProtonCore) -> Result<Value, String> {
    let api = api(core).await?;

    // System labels are a fixed Proton set.
    let mut out = vec![
        json!({"id": "0", "name": "INBOX"}),
        json!({"id": "8", "name": "Drafts"}),
        json!({"id": "7", "name": "Sent"}),
        json!({"id": "6", "name": "Archive"}),
        json!({"id": "4", "name": "Spam"}),
        json!({"id": "3", "name": "Trash"}),
        json!({"id": "5", "name": "All Mail"}),
    ];

    // Custom folders/labels from the API (LabelType::Folder=2 / Label=1 —
    // resolved at compile time; the call is what matters).
    let labels = api
        .get_labels(proton_core_api::services::proton::LabelType::Folder)
        .await
        .map_err(|e| format!("get_labels: {e:?}"))?;
    for l in labels.labels {
        out.push(json!({
            "id": l.id.to_string(),
            "name": l.name,
        }));
    }
    Ok(Value::Array(out))
}

async fn list_messages(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let label_id = params
        .get("label_id")
        .and_then(Value::as_str)
        .ok_or("missing label_id")?
        .to_owned();
    let limit = params
        .get("limit")
        .and_then(Value::as_u64)
        .unwrap_or(50);

    let api = api(core).await?;
    let resp = api
        .get_messages(GetMessagesOptions {
            label_id: Some(vec![LabelId::from(label_id)]),
            page: 0,
            page_size: limit,
            ..Default::default()
        })
        .await
        .map_err(|e| format!("get_messages: {e:?}"))?;

    let total = resp.total;
    let mut out = Vec::new();
    for m in resp.messages {
        out.push(json!({
            "id": m.id.to_string(),
            "remote_id": m.id.to_string(),
            "subject": m.subject,
            "sender_name": m.sender.name.as_clear_text_str(),
            "sender_addr": m.sender.address.as_clear_text_str(),
            "time": m.time,
            "unread": m.unread,
            "attachments": m.num_attachments,
        }));
    }
    Ok(json!({ "messages": out, "total": total }))
}

// ---------------------------------------------------------------------
// Body / attachments (decrypt via crypto-inbox; no stash)
// ---------------------------------------------------------------------

/// Our wrapper for the upstream decrypt machinery (their trait, our
/// type — see docs/development/extraction-patterns.md).
struct LeanMessage {
    id: String,
    body: Vec<u8>,
    is_mime: bool,
}
impl proton_crypto_inbox::message::GettablePGPMessage for LeanMessage {
    fn pgp_message(&self) -> &[u8] {
        &self.body
    }
}
impl DecryptableMessage for LeanMessage {
    fn message_id(&self) -> Option<&str> {
        Some(&self.id)
    }
    fn message_is_mime(&self) -> bool {
        self.is_mime
    }
}

async fn message_body(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let id = params
        .get("id")
        .and_then(Value::as_str)
        .ok_or("missing id")?
        .to_owned();
    let api = api(core).await?;
    let uctx = user_ctx(core).await?;

    let full = api
        .get_message(MessageId::from(id.clone()))
        .await
        .map_err(|e| format!("get_message: {e:?}"))?;
    let m = &full.message;

    // Decrypt the body with the message's OWN address keyring.
    let pgp = proton_crypto::new_pgp_provider();
    let tether = uctx
        .stash()
        .connection()
        .await
        .map_err(|e| format!("stash connection: {e:?}"))?;
    let keys = uctx
        .unlocked_address_keys(&pgp, &tether, &api, &m.metadata.address_id)
        .await
        .map_err(|e| format!("unlock address keys: {e:?}"))?;

    let is_mime = format!("{:?}", m.body.mime_type) == "MultipartMixed";
    let lean = LeanMessage {
        id: m.metadata.id.to_string(),
        body: m.body.body.clone().into_bytes(),
        is_mime,
    };
    let raw = lean
        .decrypt(&pgp, &keys)
        .map_err(|e| format!("decrypt: {e:?}"))?;
    let dec = raw
        .processed_body()
        .map_err(|e| format!("processed_body: {e:?}"))?;
    let body_text = dec.into_string();

    let is_html = format!("{:?}", m.body.mime_type) == "TextHtml";
    let (text, html, blocked_remote) = if is_html {
        let (h, p, b) = aerogram_html_sanitize::sanitize_for_display(&body_text);
        (p, h, b)
    } else {
        (body_text.clone(), String::new(), false)
    };

    // Attachments metadata (local id not needed — remote ids are used).
    let attachments: Vec<Value> = m
        .body
        .attachments
        .iter()
        .map(|a| {
            json!({
                "id": a.id.to_string(),
                "name": a.name,
                "mime": a.mime_type,
                "size": a.size,
            })
        })
        .collect();

    Ok(json!({
        "text": text,
        "html": html,
        "blocked_remote": blocked_remote,
        "mime_type": format!("{:?}", m.body.mime_type),
        "header": m.body.header,
        "attachments": attachments,
    }))
}

async fn get_attachment(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let id = params
        .get("id")
        .and_then(Value::as_str)
        .ok_or("missing attachment id")?
        .to_owned();
    let api = api(core).await?;
    let uctx = user_ctx(core).await?;

    // Lean core: attachment ids are REMOTE ids (no stash-local ids).
    let remote_id =
        proton_mail_api::services::proton::common::AttachmentId::from(id.clone());

    // Metadata (key packets + signatures) then the encrypted bytes.
    let meta = api
        .get_attachment_metadata(remote_id.clone())
        .await
        .map_err(|e| format!("get_attachment_metadata: {e:?}"))?;
    let bytes = api
        .get_attachment(remote_id)
        .await
        .map_err(|e| format!("get_attachment: {e:?}"))?;

    // Decrypt with the attachment's message address keyring.
    let pgp = proton_crypto::new_pgp_provider();
    let tether = uctx
        .stash()
        .connection()
        .await
        .map_err(|e| format!("stash connection: {e:?}"))?;
    let keys = uctx
        .unlocked_address_keys(&pgp, &tether, &api, &meta.attachment.address_id)
        .await
        .map_err(|e| format!("unlock address keys: {e:?}"))?;

    // The API's attachment fields ARE the crypto-inbox types (re-exported),
    // so our wrapper holds them directly (their trait, our type).
    use proton_crypto_inbox::attachment::{
        AttachmentEncryptedSignature, AttachmentSignature, DecryptableAttachment, KeyPackets,
    };
    struct LeanAttachment {
        key_packets: KeyPackets,
        signature: Option<AttachmentSignature>,
        enc_signature: Option<AttachmentEncryptedSignature>,
    }
    impl DecryptableAttachment for LeanAttachment {
        fn attachment_key_packets(&self) -> &KeyPackets {
            &self.key_packets
        }
        fn attachment_signature(&self) -> Option<&AttachmentSignature> {
            self.signature.as_ref()
        }
        fn attachment_encrypted_signature(&self) -> Option<&AttachmentEncryptedSignature> {
            self.enc_signature.as_ref()
        }
    }
    let lean = LeanAttachment {
        key_packets: meta.attachment.key_packets,
        signature: meta.attachment.signature,
        enc_signature: meta.attachment.enc_signature,
    };
    let decrypted = lean
        .decrypt(&pgp, &keys, &keys, &bytes)
        .map_err(|e| format!("attachment decrypt: {e:?}"))?;

    Ok(json!({
        "bytes_base64": base64_encode(decrypted.as_bytes()),
        "name": meta.attachment.name,
    }))
}

fn base64_encode(bytes: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity((bytes.len() + 2) / 3 * 4);
    for chunk in bytes.chunks(3) {
        let mut n = (chunk[0] as u32) << 16;
        if chunk.len() > 1 { n |= (chunk[1] as u32) << 8; }
        if chunk.len() > 2 { n |= chunk[2] as u32; }
        out.push(T[(n >> 18) as usize & 63] as char);
        out.push(T[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    out
}

// ---------------------------------------------------------------------
// Events (we poll the events feed ourselves — no foreign event loop)
// ---------------------------------------------------------------------
async fn wait_event(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let timeout_ms = params
        .get("timeout_ms")
        .and_then(Value::as_u64)
        .unwrap_or(2000);

    let api = api(core).await?;
    let deadline = std::time::Instant::now()
        + std::time::Duration::from_millis(timeout_ms.max(500));

    loop {
        let latest = api
            .get_events_latest()
            .await
            .map_err(|e| format!("get_events_latest: {e:?}"))?;

        let mut st = core.state.lock().await;
        match &st.last_event_id {
            None => {
                // First contact: record the marker, no events to report.
                st.last_event_id = Some(latest.event_id.as_str().to_owned());
                return Ok(Value::Array(vec![]));
            }
            Some(last) if *last != latest.event_id.as_str() => {
                st.last_event_id = Some(latest.event_id.as_str().to_owned());
                return Ok(Value::Array(vec![Value::String("messages".into())]));
            }
            _ => {}
        }
        drop(st);

        if std::time::Instant::now() >= deadline {
            return Ok(Value::Array(vec![]));
        }
        tokio::time::sleep(std::time::Duration::from_millis(250)).await;
    }
}
