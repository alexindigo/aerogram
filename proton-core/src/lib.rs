//! aerogram-proton-core: Aerogram's Proton Mail backend core.
//!
//! Thin C ABI over Proton's Rust mail stack (proton-mail-common et al,
//! pinned from ProtonMail/rust-mail). The Qt side links the staticlib
//! and drives everything through four C functions; all payloads are
//! JSON strings:
//!
//!   proton_core_new(data_dir) -> *mut ProtonCore
//!   proton_call(core, method, params_json) -> *mut c_char  (JSON)
//!   proton_core_free(core)
//!   proton_free_string(ptr)
//!
//! Method results are `{"ok": <value>}` or `{"err": "<message>"}`.
//! The generic dispatch keeps the ABI at four functions forever —
//! adding a method never changes the link surface.

use std::ffi::{c_char, CStr, CString};
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

use secrecy::{ExposeSecret, SecretString};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::runtime::Runtime;
use tokio::sync::Mutex;

use proton_account_api::login::LoginFlow;
use proton_core_api::session::EnvId;
use proton_core_common::datatypes::{ApiConfig, AppDetails, LocalLabelId, SystemLabel};
use proton_core_common::db::account::SessionEncryptionKey;
use proton_core_common::event_loop::EventPollMode;
use proton_core_common::os::{KeyChain, KeyChainEntryKind, KeyChainError, KeyChainExt as _};
use proton_core_common::Origin;
use proton_issue_reporter_service::NoopIssueReporter;
use proton_log_service::LogService;
use proton_mail_common::datatypes::LocalMessageId;
use proton_core_common::models::ModelExtension as _;
use proton_mail_common::models::Message as MailMessage;
use proton_mail_common::MailContext;

// ---------------------------------------------------------------------
// Tokio runtime: one multi-thread runtime per process, created lazily.
// The C++ side calls us from QtConcurrent workers; each FFI call
// block_on()s its async work on this runtime.
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
// File-backed keychain. The session DB encryption key must survive
// process restarts or stored sessions become unreadable and every
// launch would demand the password again. One file per entry kind,
// 0600, under <data_dir>/keychain/.
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
        // No Debug on the enum — map explicitly.
        let name = match kind {
            KeyChainEntryKind::EncryptionKey => "encryption",
            KeyChainEntryKind::DeviceKey => "device",
            KeyChainEntryKind::PinHash => "pin_hash",
            _ => "other",
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
#[derive(Default)]
struct CoreState {
    login_flow: Option<LoginFlow>,
    user_ctx: Option<Arc<proton_mail_common::MailUserContext>>,
}

pub struct ProtonCore {
    ctx: Arc<MailContext>,
    state: Mutex<CoreState>,
    /// Per-label budget for forced re-sync when a label comes back
    /// empty right after its first sync (burned-initialized race with
    /// the SDK's initial event sync). label_id → attempts used.
    empty_sync_retries: Mutex<std::collections::HashMap<u64, u32>>,
}

async fn create_context(data_dir: &Path) -> Result<Arc<MailContext>, String> {
    let session_db = data_dir.join("session");
    let user_db = data_dir.join("user");
    let core_cache = data_dir.join("core_cache");
    let mail_cache = data_dir.join("mail_cache");
    for d in [&session_db, &user_db, &core_cache, &mail_cache] {
        std::fs::create_dir_all(d).map_err(|e| format!("mkdir {}: {e}", d.display()))?;
    }

    let log_config = proton_log_service::Config::builder()
        .name("aerogram-proton".into())
        .directory(data_dir.join("logs"))
        .build();

    // The session DB encryption key must exist before the context
    // boots (login flow fails with KeyChainHasNoKey otherwise) and must
    // be the SAME key across restarts, or stored sessions become
    // unreadable. Generate once, persist in the file keychain.
    let keychain = FileKeyChain::new(data_dir.join("keychain"));
    if keychain
        .load::<SessionEncryptionKey>()
        .map_err(|e| format!("keychain load: {e}"))?
        .is_none()
    {
        keychain
            .store(SessionEncryptionKey::random())
            .map_err(|e| format!("keychain store: {e}"))?;
    }

    // Client identity: muon validates platform/product against closed
    // enums, so a custom "aerogram" token is not expressible here — the
    // composed header is "linux-mail@<version>". A named third-party
    // identity ("Other_aerogram") is an upstream question
    // (go-proton-api#227); TODO: revisit once Proton offers one.
    let api_config = ApiConfig {
        app_details: AppDetails {
            platform: "linux".into(),
            product: "mail".into(),
            version: env!("CARGO_PKG_VERSION").into(),
        },
        user_agent: None,
        env_id: EnvId::new_prod(),
        proxy: None,
        resolver: None,
    };

    MailContext::new(
        Origin::App,
        runtime().handle().clone(),
        session_db,
        user_db,
        core_cache,
        mail_cache,
        50 * 1024 * 1024,
        Arc::new(keychain),
        api_config,
        None, // hv challenge notifier
        None, // device info provider
        LogService::new(log_config),
        EventPollMode::Automatic(std::time::Duration::from_secs(60)),
        Default::default(),
        Arc::new(NoopIssueReporter),
    )
    .await
    .map_err(|e| format!("MailContext::new: {e:?}"))
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

/// Create the core for an account data dir.
///
/// # Safety
/// `data_dir` must be a valid NUL-terminated UTF-8 C string. The
/// returned pointer is owned by the caller and must be freed with
/// proton_core_free.
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
            empty_sync_retries: Mutex::new(std::collections::HashMap::new()),
        })),
        Err(e) => {
            eprintln!("proton_core_new: {e}");
            std::ptr::null_mut()
        }
    }
}

/// # Safety
/// Frees a core created by proton_core_new. Not safe to call twice.
#[no_mangle]
pub unsafe extern "C" fn proton_core_free(core: *mut ProtonCore) {
    if !core.is_null() {
        drop(Box::from_raw(core));
    }
}

/// # Safety
/// Frees a string returned by proton_call. Not safe to call twice.
#[no_mangle]
pub unsafe extern "C" fn proton_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

/// Generic method dispatch. `params` is a JSON object; the result is a
/// JSON string `{"ok": …}` / `{"err": …}` owned by the caller.
///
/// # Safety
/// `core` must come from proton_core_new; `method` and `params` must
/// be valid NUL-terminated UTF-8 C strings.
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
// Method implementations
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
        _ => Err(format!("unknown method: {method}")),
    }
}

#[derive(Deserialize)]
struct LoginParams {
    user: String,
    password: String,
}

/// Login state machine: ok | totp | mailbox_password.
fn flow_state(flow: &LoginFlow) -> &'static str {
    if flow.is_awaiting_2fa() {
        "totp"
    } else if flow.is_awaiting_mailbox_password() {
        "mailbox_password"
    } else {
        "ok"
    }
}

/// Login flow finished: promote to a user context and remember it.
/// Call with the state lock NOT held.
async fn finish_login(core: &ProtonCore) -> Result<Value, String> {
    let flow = core.state.lock().await.login_flow.take();
    let mut flow = flow.ok_or("no login flow in progress")?;
    let user_ctx = core
        .ctx
        .user_context_from_login_flow(&mut flow)
        .await
        .map_err(|e| format!("user_context_from_login_flow: {e:?}"))?;
    let addr = user_ctx
        .user()
        .await
        .map(|u| u.email)
        .unwrap_or_default();
    core.state.lock().await.user_ctx = Some(user_ctx);
    Ok(json!({ "state": "ok", "addr": addr }))
}

async fn login(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let p: LoginParams =
        serde_json::from_value(params).map_err(|e| format!("login params: {e}"))?;

    let mut flow = core
        .ctx
        .new_login_flow()
        .await
        .map_err(|e| format!("new_login_flow: {e:?}"))?;
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
    let needs_more = {
        let state = core.state.lock().await;
        let flow = state.login_flow.as_ref().ok_or("login flow lost")?;
        flow_state(flow)
    };
    if needs_more == "ok" {
        finish_login(core).await
    } else {
        Ok(json!({ "state": needs_more }))
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
    let ctxs = core
        .ctx
        .get_all_logged_in_user_ctx()
        .await
        .map_err(|e| format!("get_all_logged_in_user_ctx: {e:?}"))?;
    let Some(user_ctx) = ctxs.into_iter().next() else {
        return Ok(json!({ "state": "none" }));
    };
    let addr = user_ctx.user().await.map(|u| u.email).unwrap_or_default();
    core.state.lock().await.user_ctx = Some(user_ctx);
    Ok(json!({ "state": "ok", "addr": addr }))
}

async fn user_ctx(
    core: &ProtonCore,
) -> Result<Arc<proton_mail_common::MailUserContext>, String> {
    core.state
        .lock()
        .await
        .user_ctx
        .clone()
        .ok_or_else(|| "not logged in".to_string())
}

async fn account_info(core: &ProtonCore) -> Result<Value, String> {
    let ctx = user_ctx(core).await?;
    let user = ctx.user().await.map_err(|e| format!("user: {e:?}"))?;
    Ok(json!({
        "email": user.email,
        "name": user.name,
        "display_name": user.display_name,
    }))
}

/// Conversations list = Proton labels. Round 1: the system label set,
/// resolved to local ids.
async fn list_labels(core: &ProtonCore) -> Result<Value, String> {
    let ctx = user_ctx(core).await?;
    let stash = ctx.user_stash();
    let tether = stash
        .connection()
        .await
        .map_err(|e| format!("db connection: {e:?}"))?;

    const SYSTEM: &[(SystemLabel, &str)] = &[
        (SystemLabel::Inbox, "INBOX"),
        (SystemLabel::AllDrafts, "Drafts"),
        (SystemLabel::AllSent, "Sent"),
        (SystemLabel::Archive, "Archive"),
        (SystemLabel::Spam, "Spam"),
        (SystemLabel::Trash, "Trash"),
        (SystemLabel::AllMail, "All Mail"),
    ];

    let mut out = Vec::new();
    for (label, name) in SYSTEM {
        match label.local_id(&tether).await {
            Ok(Some(local_id)) => out.push(json!({
                "id": local_id.as_u64(),
                "name": name,
            })),
            Ok(None) => {} // not present locally yet — fine
            Err(e) => eprintln!("label {name} local_id: {e:?}"),
        }
    }
    Ok(Value::Array(out))
}

async fn list_messages(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let label_id = params
        .get("label_id")
        .and_then(Value::as_u64)
        .ok_or("missing label_id")?;
    let limit = params
        .get("limit")
        .and_then(Value::as_u64)
        .unwrap_or(50) as usize;

    let ctx = user_ctx(core).await?;
    let local_label = LocalLabelId::from(label_id);

    // First-page sync if this label has never been fetched (the SDK
    // skips re-sync once the label is initialized).
    {
        let stash = ctx.user_stash();
        let mut tether = stash
            .connection()
            .await
            .map_err(|e| format!("db connection: {e:?}"))?;
        let mbox = proton_mail_common::Mailbox::new(&tether, local_label)
            .await
            .map_err(|e| format!("mailbox: {e:?}"))?;
        mbox.sync(&mut tether, ctx.session(), limit)
            .await
            .map_err(|e| format!("mailbox sync: {e:?}"))?;

        let mut messages = MailMessage::in_label(local_label, &tether)
            .await
            .map_err(|e| format!("in_label: {e:?}"))?;

        // Burned-initialized guard: a first sync that raced the SDK's
        // initial event sync marks the label initialized while EMPTY and
        // it never refills. Detect empty-after-sync and force one direct
        // page fetch (capped per label per session — genuinely empty
        // folders must not re-fetch forever).
        if messages.is_empty() {
            let mut retries = core.empty_sync_retries.lock().await;
            let count = retries.entry(label_id).or_insert(0);
            if *count < 3 {
                *count += 1;
                let remote = proton_core_common::models::Label::find_by_id(local_label, &tether)
                    .await
                    .ok()
                    .flatten()
                    .and_then(|l| l.remote_id);
                if let Some(remote_id) = remote {
                    eprintln!("proton: label {label_id} empty after sync; forcing page fetch");
                    MailMessage::sync_first_message_page(
                        remote_id,
                        limit.max(50),
                        ctx.session(),
                        &mut tether,
                    )
                    .await
                    .map_err(|e| format!("forced page sync: {e:?}"))?;
                    messages = MailMessage::in_label(local_label, &tether)
                        .await
                        .map_err(|e| format!("in_label: {e:?}"))?;
                }
            }
        }

        let mut out = Vec::new();
        for m in messages.into_iter().take(limit) {
            out.push(json!({
                "id": m.local_id.map(|id| id.as_u64()),
                "remote_id": m.remote_id.as_ref().map(|id| id.to_string()),
                "subject": m.subject,
                "sender_name": m.sender.name.as_clear_text_str(),
                "sender_addr": m.sender.address.as_clear_text_str(),
                "time": m.time.as_u64(),
                "unread": m.unread,
                "attachments": m.num_attachments,
            }));
        }
        return Ok(Value::Array(out));
    }
}

async fn message_body(core: &ProtonCore, params: Value) -> Result<Value, String> {
    let id = params
        .get("id")
        .and_then(Value::as_u64)
        .ok_or("missing id")?;
    let ctx = user_ctx(core).await?;
    let local_id = LocalMessageId::from(id);

    let body = MailMessage::message_body(&ctx, local_id)
        .await
        .map_err(|e| format!("message_body: {e:?}"))?;

    Ok(json!({
        "text": body.body,
        "mime_type": format!("{:?}", body.mime_type),
    }))
}
