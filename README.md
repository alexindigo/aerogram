# Aerogram

A unified email and Delta Chat client built with Qt6 / KDE Kirigami.

## Build

```bash
cmake -B build -S .
cmake --build build
./build/aerogram
```

Dependencies: Qt 6.5+ (Core, Quick, QuickControls2, Sql-less — see
below, Concurrent), KF6 Kirigami, libcurl, SQLCipher, libsodium.

## Architecture

```
src/
├── main.cpp                      # Entry point; registers backend factories
├── controllers/
│   └── AccountController         # Single source of truth; owns Account
│                                 #   entities (backend held via interface)
├── models/
│   ├── ConversationListModel     # Conversation list (chat|folder kinds)
│   ├── MessageListModel          # Message list
│   └── AccountListModel          # Sidebar account rail
└── core/
    ├── Account.h                 # First-class account entity
    ├── Types.h                   # Conversation / Message / AttachmentMeta
    ├── crypto/                   # MasterKeyManager (Argon2id vault),
    │                             #   AccountStore (SQLCipher accounts.db)
    ├── imap/                     # ImapBackend: CurlTransport, MimeParser,
    │                             #   MessageStore (.enc shards), MetadataIndex
    ├── ipc/                      # IpcServer (reflective JSON-RPC dispatch)
    └── plugin/                   # BackendPlugin base, Capabilities,
                                  #   BackendRegistry, MockBackend,
                                  #   DeltaChatBackend

ui/
├── main.qml                      # Orchestrator: RowLayout + StackLayout
├── components/
│   ├── Sidebar.qml               # Account rail (chips by family)
│   ├── AddAccountDialog.qml      # Add-account dialog
│   ├── LockOverlay.qml           # Vault lock/welcome overlay
│   ├── AeroIcon.qml              # On-disk icon pack wrapper
│   └── IdentityBlock.qml         # Sender identity block (address-derived)
└── views/
    ├── EmailInboxView.qml        # Two-pane inbox (list + reading pane)
    ├── ChatView.qml              # Conversation list
    └── SettingsView.qml          # Settings + danger zone
```

Backends plug in through `BackendPlugin` + capability interfaces and
register with `BackendRegistry`; adding one is a single
`registerType()` call. Components communicate via semantic signals
wired declaratively in `main.qml`. See `docs/ARCHITECTURE.md`.
