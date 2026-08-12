# Extracting from a framework: the trait-on-our-type pattern

When we take a piece of a large upstream stack (Proton's Rust mail
crates) without its orchestration layer, the recurring problem is that
capabilities are delivered as *traits implemented in the layer we're
dropping*.

Concrete case: `EncryptedMessageBody.decrypt(...)` works via the
`DecryptableMessage` trait — but the *impl* of that trait for
`EncryptedMessageBody` lives in `mail-common`, which the lean core
drops. Drop the layer, lose the impl.

**The pattern: implement the upstream trait for our own minimal
wrapper type.** The trait (and its algorithms) live in a crate we keep;
the impl is ours:

```rust
use proton_crypto_inbox::message::{DecryptableMessage, GettablePGPMessage};

struct LeanMessage { id: String, body: Vec<u8>, is_mime: bool }

impl GettablePGPMessage for LeanMessage {
    fn pgp_message(&self) -> &[u8] { &self.body }
}
impl DecryptableMessage for LeanMessage {
    fn message_id(&self) -> Option<&str> { Some(&self.id) }
    fn message_is_mime(&self) -> bool { self.is_mime }
}

// upstream's battle-tested decrypt now runs on OUR type:
let raw = lean.decrypt(&pgp, &keys)?;
```

Rules of thumb:

- The trait must be defined in a crate you *keep* (crypto-inbox), not
  the one you're dropping (mail-common). Check where the trait is
  *defined*, not just where it's used.
- Everything crossing crate boundaries (trait objects, generic bounds)
  must come from a single crate instance — see the
  `rust-duplicate-crate-instances` skill for the failure mode.
- Keep the wrapper minimal: carry only what the trait's methods read
  (id, body, mime-ness). The wrapper is an adapter, not a re-model.
