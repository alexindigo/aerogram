# Atmogram

A unified email and Delta Chat client built with Qt6 / KDE Kirigami.

## Build

```bash
cmake -B build -S .
cmake --build build
./build/atmogram
```

Dependencies: Qt 6.5+ (Core, Quick, QuickControls2) and KF6 Kirigami.

## Architecture

```
src/
├── main.cpp                     # Application entry point
├── controllers/
│   └── AccountController         # QObject exposing models + backend slots
├── models/
│   ├── MessageListModel          # QAbstractListModel with mock email data
│   └── ChatListModel             # QAbstractListModel with mock chat data
└── core/                         # Future backend stubs (IMAP, Proton, Delta Chat)

ui/
├── main.qml                      # Orchestrator: RowLayout + StackLayout
├── components/
│   └── Sidebar.qml               # Navigation toolbar with explicit signals
└── views/
    ├── EmailInboxView.qml        # Inbox view with semantic signals
    └── ChatView.qml              # Chat view with semantic signals
```

Components communicate via semantic callback handlers wired declaratively in `main.qml`.
