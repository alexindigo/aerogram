//! Lean-core spike: prove we can login-restore + fetch + decrypt a
//! message using ONLY the core Context + mail-api + crypto-inbox —
//! NO MailContext (no stash, no duplicate body store).
//!
//!   cargo run --bin lean_spike -- <data_dir>

use std::sync::Arc;

use proton_core_common::datatypes::{ApiConfig, AppDetails};
use proton_core_common::event_loop::EventPollMode;
use proton_core_common::os::{KeyChain, KeyChainEntryKind, KeyChainError};
use proton_core_common::Origin;
use proton_issue_reporter_service::NoopIssueReporter;
use proton_log_service::LogService;
use secrecy::{ExposeSecret, SecretString};

// --- minimal file keychain (same scheme as the app's) ---
struct FileKeyChain(std::path::PathBuf);
impl KeyChain for FileKeyChain {
    fn store_entry(&self, kind: KeyChainEntryKind, key: SecretString) -> Result<(), KeyChainError> {
        let name = match kind {
            KeyChainEntryKind::EncryptionKey => "encryption",
            KeyChainEntryKind::DeviceKey => "device",
            KeyChainEntryKind::PinHash => "pin_hash",
        };
        std::fs::create_dir_all(&self.0).ok();
        std::fs::write(self.0.join(format!("{name}.key")), key.expose_secret())
            .map_err(|e| KeyChainError::new(Box::new(e)))
    }
    fn delete_entry(&self, kind: KeyChainEntryKind) -> Result<(), KeyChainError> {
        let name = match kind {
            KeyChainEntryKind::EncryptionKey => "encryption",
            KeyChainEntryKind::DeviceKey => "device",
            KeyChainEntryKind::PinHash => "pin_hash",
        };
        let _ = std::fs::remove_file(self.0.join(format!("{name}.key")));
        Ok(())
    }
    fn load_entry(&self, kind: KeyChainEntryKind) -> Result<Option<SecretString>, KeyChainError> {
        let name = match kind {
            KeyChainEntryKind::EncryptionKey => "encryption",
            KeyChainEntryKind::DeviceKey => "device",
            KeyChainEntryKind::PinHash => "pin_hash",
        };
        let p = self.0.join(format!("{name}.key"));
        if !p.exists() {
            return Ok(None);
        }
        Ok(Some(SecretString::from(
            std::fs::read_to_string(p).map_err(|e| KeyChainError::new(Box::new(e)))?,
        )))
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let dir = std::env::args().nth(1).expect("usage: lean_spike <data_dir>");
    let base = std::path::PathBuf::from(dir);

    let api_config = ApiConfig {
        app_details: AppDetails {
            platform: "linux".into(),
            product: "mail".into(),
            version: "0.1.0".into(),
        },
        ..Default::default()
    };
    let log_config = proton_log_service::Config::builder()
        .name("lean-spike".into())
        .directory(base.join("logs"))
        .build();

    let handle = tokio::runtime::Handle::current();
    let ctx = proton_core_common::Context::new(
        Origin::App,
        handle,
        base.join("session"),   // account_db (sessions) — small
        base.join("user"),      // user db (settings/addresses)
        Arc::new(FileKeyChain(base.join("keychain"))),
        vec![],                 // no user-db initializers (no mail schema)
        api_config,
        None,
        None,
        base.join("core_cache"),
        LogService::new(log_config),
        EventPollMode::Manual,
        Default::default(),
        Arc::new(NoopIssueReporter),
        proton_core_common::services::global_feature_flags::FeatureFlagsBackgroundTask::Disabled,
    )
    .await?;

    // restore session
    let mut sessions = ctx.get_authenticated_sessions().await?;
    let session = sessions.next().ok_or("no stored session")?;
    println!("session restored: {}", session.remote_id);

    let user_ctx = ctx.user_context_from_session(&session).await?;
    let api = ctx.new_api_session(Some(&session)).await?;

    // list INBOX (label "0") — 1 message
    use proton_mail_api::services::proton::ProtonMail;
use proton_mail_api::services::proton::requests::GetMessagesOptions;
use proton_core_api::services::proton::LabelId;
    let label_inbox = LabelId::from("0");
    let opts = GetMessagesOptions {
        label_id: Some(vec![label_inbox]),
        page: 0,
        page_size: 1,
        ..Default::default()
    };
    let list = api.get_messages(opts).await?;
    let first = list.messages.first().ok_or("inbox empty")?.clone();
    let msg_id = first.id.clone();
    println!("got message id: {}", msg_id);

    let full = api.get_message(msg_id.clone()).await?;
    let m = &full.message;
    println!("subject: {}", m.metadata.subject);

    // decrypt the body with the address keyring
    let pgp = proton_crypto_inbox::proton_crypto::new_pgp_provider();
    let tether = user_ctx.stash().connection().await?;
    let addr = user_ctx
        .address_service()
        .find_valid_sender_address()
        .await?
        .ok_or("no sender address")?;
    let keys = user_ctx
        .unlocked_address_keys(&pgp, &tether, &api, &m.metadata.address_id)
        .await?;

    // Extraction pattern: implement THEIR DecryptableMessage trait for
    // OUR tiny wrapper (the impl for their EncryptedMessageBody lives in
    // mail-common, which we've dropped).
    use proton_crypto_inbox::message::{DecryptableMessage, GettablePGPMessage};
    struct SpikeMsg { id: String, body: Vec<u8>, is_mime: bool }
    impl GettablePGPMessage for SpikeMsg {
        fn pgp_message(&self) -> &[u8] { &self.body }
    }
    impl DecryptableMessage for SpikeMsg {
        fn message_id(&self) -> Option<&str> { Some(&self.id) }
        fn message_is_mime(&self) -> bool { self.is_mime }
    }
    let is_mime = format!("{:?}", m.body.mime_type) == "MultipartMixed";
    let spike = SpikeMsg {
        id: msg_id.to_string(),
        body: m.body.body.clone().into_bytes(),
        is_mime,
    };
    let raw = spike.decrypt(&pgp, &keys)?;
    let dec = raw.processed_body()?;
    let text = dec.into_string();
    println!("=== DECRYPTED BODY (first 200 chars) ===");
    println!("{}", &text[..text.len().min(200)]);
    println!("SPIKE-OK");
    Ok(())
}
