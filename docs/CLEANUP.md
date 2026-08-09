# Cleanup — Undiscussed Patterns

This document captures ambient patterns and design tensions discovered
during the code audit. Each entry has a **description**, **status**,
and a **proposed direction**. These are not yet agreed decisions — they
are parked here for per-pattern review with the user.

When a pattern is resolved, it moves out of this file:
- Into `docs/ARCHITECTURE.md` if it becomes part of the core design.
- Into a commit / refactor if it's actionable code work.
- Deleted if we decide it's a non-issue.

---

## Pattern B — QML context property injection

### Description

`src/main.cpp` publishes the `AccountController` instance as a QML
context property:

```cpp
engine.rootContext()->setContextProperty("accountController", &controller);
```

Every QML file transparently sees `accountController` in its scope
without explicit imports. Example:

```qml
// EmailInboxView.qml — reaches into a globally injected object
ListView { model: accountController.messageListModel }
```

### Tension with Pattern 1

`docs/ARCHITECTURE.md` currently says:

> Components have zero reference knowledge outside their file
> boundaries.

But every view happily references `accountController` — which is
outside its file. Taken literally, this is a violation.

### Proposed direction

Reframe Pattern 1's rule to distinguish two kinds of "outside":

- **Sibling components** — must remain unknown to each other.
  Components must not import or reference other components' internals.
- **The controller layer** — is intentionally globally accessible.
  Views read from it; they don't reach sideways.

Add an explicit "Context property injection is a permitted exception"
paragraph to `docs/ARCHITECTURE.md` Pattern 1, clarifying that the
controller is the single global anchor and everything else is
file-scoped.

Alternative: switch to `qmlRegisterSingletonType` for stronger typing
and IDE support. Slightly more setup, cleaner semantics.

---

## Pattern C — Data-transfer structs in Types.h

### Description

`src/core/Types.h` defines plain structs (`Message`, `ChatMessage`)
without `Q_GADGET`, without metatype registration, not exposed to QML.
They are pure C++ transport between backend and controller/models.

QML only sees the fields via `QAbstractListModel::data()` and role
names — never touches the struct itself.

### Proposed direction

Document this in `docs/ARCHITECTURE.md` as an intentional choice:

> DTOs (`Types.h`) are C++-only. QML consumes them through the model's
> role names, not through direct struct access. Do not add `Q_GADGET`
> or metatype registration unless a specific use case (e.g. passing a
> struct through a signal to QML) requires it.

Prevents future contributors from adding QML plumbing on speculation.

---

## Pattern D — Header-only Q_OBJECT classes

### Description

`DeltaChatBackend`, `MockBackend`, `IpcServer` are all header-only:
class definition, method bodies, `Q_OBJECT` — everything in the `.h`.

Works because `CMAKE_AUTOMOC` picks up any header listed in
`qt_add_executable`. Compiles fine.

### Trade-offs

- **Pro:** one file per class, fewer files to keep in sync.
- **Con:** headers get large; recompilation is coarse (touching a
  private method rebuilds every translation unit that includes the
  header).

### Proposed direction

Guideline, not enforcement:

> Keep classes header-only until they exceed ~150 lines or start
> including heavy dependencies. At that point, split to
> `.h` + `.cpp`. Existing header-only classes stay as-is until they
> warrant a split.

Add to `docs/ARCHITECTURE.md` under a new "Style Guidelines" section
(or a separate `docs/STYLE.md`).

---

## Pattern E — Stringly-typed configStatus

### Description

`AccountController::configStatus` is a `QString` with values like:

- `"Not configured"`
- `"Connecting..."`
- `"Connected"`
- `"Setting up account from QR..."`
- `"Receiving backup from first device..."`
- `"Setup failed"`
- `"Error: <backend message>"`

Both QML (Settings page) and IPC clients receive this string.

### Problem

External clients can't distinguish states programmatically without
string matching. Adding new states requires coordinating with all
consumers.

### Proposed direction

Split into two properties:

```cpp
enum class ConfigState {
    NotConfigured,
    Connecting,
    Connected,
    ReceivingBackup,
    Error
};
Q_PROPERTY(ConfigState configState READ configState NOTIFY configStateChanged)
Q_PROPERTY(QString configStatusText READ configStatusText NOTIFY configStatusChanged)
```

`configState` is machine-readable, `configStatusText` is
human-readable. IPC clients can react to state changes; QML can
still display the friendly string.

### Not urgent

Deferred until an IPC client actually needs to react to state
changes programmatically.

---

## Pattern G — Full-reset model updates

### Description

Both `MessageListModel::setMessages()` and
`ChatListModel::setChatMessages()` replace the entire dataset via
`beginResetModel` / `endResetModel`. QML `ListView` re-renders from
scratch on every update.

For a chat list of a few dozen entries: fine. For a message list that
grows to thousands: flicker, lost scroll position, wasted CPU.

### Proposed direction

Migrate to incremental updates (`insertRows`, `removeRows`,
`dataChanged`) when message counts grow past ~100, or when scroll
retention becomes a felt problem.

Not urgent. Flag in `docs/ARCHITECTURE.md` under "Known Limitations"
so nobody assumes the current pattern is intentional for scale.

---

## Cross-cutting rule discussed but not yet documented

During the Pattern A discussion, we agreed on a general principle:

> **State transitions of interest must be typed controller signals.**
> Do not observe state via logs, string parsing, or polling. If IPC
> clients or QML would react to a change, that change is a signal.

This came out of resolving the silent `startIo` / `stopIo` callbacks.
It composes Pattern 1 (semantic signals), Pattern 2 (unidirectional
flow), and Pattern 3 (IPC event broadcast) into a single rule.

**Proposed direction:** add to `docs/ARCHITECTURE.md` under Pattern 3
in a future revision. Parked here pending explicit approval.

---

## Placeholder handlers in `main.qml` — resolved separately

Not a pattern per se, but noted in the audit:

- `onChatThreadRequested` → `console.log(...)`
- `onGroupInfoRequested` → `console.log(...)`
- `onComposeMessageRequested` → sends `"hello"` to first chat

These are being **deleted** as part of the Phase 1 dead-signal
cleanup. Recorded here for completeness.

---

## Pattern H — QML-level service injection + component activation

### Source

`~/Projects/aerial-lock/` — Wayland session-lock client with Aerial
video backgrounds. Uses Quickshell + QML.

### Three sub-patterns observed

**A. Service injection.** Services are instantiated at the shell
level and passed down as properties. Children never import services
globally — they receive what they need through their public API
surface.

```qml
// shell.qml — creates the service, owns its lifecycle
Services.ConfigStore { id: cfg }

// component — receives config via property, does not import the service
Lock { config: cfg.data }
```

Contrast with Aerogram's current approach: `accountController` is a
global C++ context property, visually reachable from any QML file.
The intent is similar (single source of truth), but the coupling
model differs: aerial-lock makes the dependency chain explicit in
the property graph; Aerogram makes it ambient in the QML scope.

**B. Lazy component activation with Loader.** Components only exist
when their activation conditions are met.

```qml
Loader {
    active: cfg.ready        // component is not created until config is loaded
    sourceComponent: Component { LockModule.Lock { ... } }
}
```

Contrast with Aerogram's current approach: all views live in a
static `StackLayout` created at startup regardless of whether an
account is connected.

**C. QML-level services (not just C++ controllers).**
aerial-lock's `ConfigStore` is a 192-line QML service with zero C++.
It handles config loading, validation, schema checking, i18n —
all in QML/JS. Aerogram currently has no QML service layer; every
non-trivial piece is C++.

### How this could reshape Aerogram

```
main.qml
  ├── Services.Configuration { ... }    ← QML service: accounts, status
  ├── Services.BackendBridge { ... }    ← thin QML wrapper around C++
  ├── Sidebar { accounts: config.accounts }
  └── StackLayout
        ├── Loader { active: status === "Connected"; ChatView }
        └── SettingsView { ... }
```

### Proposed direction

Document as future design direction. Not actionable until we have
concrete pain from the flat `AccountController` / global
context-property pattern (e.g. too many components coupling to
internals, initialization-order bugs, difficulty testing individual
views in isolation).

The global context property is a recognized and documented exception
to Pattern 1's file-isolation rule (see CLEANUP Pattern B).

Priority: later. This is architectural north-star material, not a
flaw.

### References

- `~/Projects/aerial-lock/shell.qml` — root level, service injection,
  Loader pattern
- `~/Projects/aerial-lock/Services/ConfigStore.qml` — 192-line pure
  QML service with zero C++
- `~/Projects/aerogram/ui/main.qml` — current flat static-layout
  approach (for contrast)
- `~/Documents/Aerial/DESIGN.md` — aerial-lock design document with
  component tree and C++/QML layer split
