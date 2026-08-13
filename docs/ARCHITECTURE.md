# Aerogram Architecture

Aerogram is a unified chat and email client. This document defines the
architectural patterns that shape the codebase. Read it fully before
making changes.

Some sections describe the **target state**. Where the current code
has not yet caught up, this is called out inline with a
**(TARGET STATE)** marker. New work should move toward the target
state, not away from it.

---

## The Three Interlocking Patterns

Aerogram's architecture rests on three patterns:

1.  **Semantic callback handlers**
2.  **Unidirectional data flow**
3.  **Behind-the-scenes IPC wiring**

They are not independent — each reinforces the others.

---

## Pattern 1: Semantic Callback Handlers

Every component publishes explicit, domain-named signals. No magic
strings, no generic event routers, no downstream string parsing.

### Rules

- Each signal has a specific semantic meaning
  (`accountSelected`, `messageDetailsRequested`, `chatSelected`) — not
  generic (`itemClicked`, `action`).
- If data flows with the event, the signal declares the parameter
  type explicitly.
- Components have zero reference knowledge outside their file
  boundaries. They broadcast outward via signals and receive state via
  bound properties. That is the entire communication surface.
- The parent (`main.qml`) wires each signal declaratively to a
  single-purpose handler. No conditional dispatch based on string
  values.

### Example — a component publishing signals

```qml
// Sidebar.qml
signal accountSelected(string accountId)
signal settingsRequested()
signal resetApplicationRequested()
```

### Example — declarative wiring in the orchestrator

```qml
Sidebar {
    onAccountSelected: (id) => accountController.selectAccount(id)
    onSettingsRequested: accountController.showSettings()
    onResetApplicationRequested: accountController.resetApp()
}
```

Each handler is a straight line from signal to controller slot. No
intermediate state, no parsing.

---

## Pattern 2: Unidirectional Data Flow

Components do not own semantic state. State lives in one place
(`AccountController`), and visual components are pure projections of
it. Components emit intent upward; they never mutate shared state
directly.

### Flow for a user action

```
component signal
    → main.qml handler
    → controller slot
    → controller state updates
    → NOTIFY fires
    → component re-reads via bound property
    → UI reflects new state
```

### What counts as component-local state (allowed)

Transient UI details with no semantic meaning outside the component:

- Text field contents while the user is typing.
- Scroll position within a list.
- Open/closed state of a menu or dropdown.

### What must NOT be component-local state (forbidden)

Anything a second observer would care about:

- The currently active account or view.
- Which chat is selected.
- Whether a sync is in progress.
- The list of accounts / chats / messages.

These must be read from the controller through a bound property.

### Example — correct (component reads from controller)

```qml
Sidebar {
    id: sidebar

    // Sidebar's visual state is a projection of controller state.
    Button {
        highlighted: accountController.activeAccountId === "delta:1"
        onClicked: sidebar.accountSelected("delta:1")
    }
}
```

Clicking the button emits a signal upward. It does not touch any
local state. The highlight updates only after the controller commits
the new active account and the binding re-evaluates.

### Example — incorrect (component owns state)

```qml
Sidebar {
    id: sidebar
    property string currentSection: "email"   // ✗ local state

    Button {
        highlighted: sidebar.currentSection === "email"   // ✗ reads own state
        onClicked: {
            sidebar.currentSection = "email"              // ✗ mutates own state
            sidebar.inboxRequested()
        }
    }
}
```

Now IPC or any other input source can change the controller without
the sidebar noticing. Selection state desyncs. This is the exact
failure mode Pattern 2 prevents.

### Rules

- Every controller property visible to QML must have a `NOTIFY`
  signal. No polling, no one-shot reads.
- Components read; components emit; components do not mutate.
- The controller is the single source of truth for all semantic
  state.

### Panel independence

Panels are independent components. The controller owns a **layout
model** (`panelLayout`): a list of `{id, type, x, y, width, height,
visible}` entries where type is `panel` or `separator`. `main.qml` is
a Repeater of absolutely-positioned Loaders bound to that model — the
window pushes its size via `setWindowSize` and the controller
recomputes geometry. No panel references another — a panel may be its
own window later without changing its code. Responsive layout (e.g.
narrow windows stack conversations above messages 35/65) is a
controller branch on window size, not QML. Composition containers that
assume co-location (a StackLayout of fixed pages, a view nesting
another view) are forbidden: they presume what the controller owns.

---

## Pattern 3: Behind-the-Scenes IPC Wiring

IPC (Unix domain socket, JSON-RPC 2.0) attaches at the
`AccountController` layer using reflection. UI components have zero
knowledge that IPC exists.

### Incoming — external code invokes a controller slot

```
JSON-RPC request arrives
    → IPC layer looks up the method by name (reflection)
    → invokes the same slot a user click would reach
    → same downstream state changes, same NOTIFYs
```

Adding a new public slot to `AccountController` immediately makes it
available over IPC. The IPC layer (`IpcServer`) dispatches incoming
calls reflectively via `QMetaObject::invoke` — no per-method wiring.

### Outgoing — controller signals broadcast to IPC clients

```
Controller signal fires
    → IPC layer serializes parameters as JSON
    → broadcasts as JSON-RPC 2.0 notification (no "id")
    → all connected clients receive the event
```

Outgoing broadcast exists: `IpcServer::subscribeToControllerSignals`
has per-signal `connect()` lines that serialize each controller signal
into a JSON-RPC notification. **(TARGET STATE)** Fully reflective
signal-to-event broadcast (no per-signal wiring) remains planned.

### Rules

- Do not bypass the controller. IPC attaches at that layer only.
- Do not hand-write per-method dispatch in the IPC layer.
- Do not expose backend internals directly to IPC.

---

## Composition — why the three patterns need each other

```
Semantic signals
    ↓
    give components clear, typed surfaces to emit intent.
    ↓
Unidirectional flow
    ↓
    ensures those signals are the *only* source of intent, and state
    changes propagate through a single channel.
    ↓
IPC wiring
    ↓
    can attach generically at the controller layer, because that
    layer is the sole convergence point for all inputs and the sole
    source of all outputs.
```

Remove any one pattern and the others break down:

- Without semantic signals, the controller's public surface is
  polluted with generic dispatchers.
- Without unidirectional flow, IPC changes desync from UI state.
- Without behind-the-scenes IPC, the controller layer accumulates
  ad-hoc integration points.

---

## Layered View

```
  ┌─────────────────────┐   ┌──────────────────────┐
  │  QML components     │   │  External IPC client │
  │  (semantic signals) │   │  (JSON-RPC 2.0)      │
  └─────────┬───────────┘   └──────────┬───────────┘
            │                           │
            │ declarative               │ reflection
            │ wiring in main.qml        │ (target state)
            │                           │
            ▼                           ▼
        ┌──────────────────────────────────────┐
        │  AccountController                    │
        │  (slots + signals + Q_PROPERTY)       │
        │                                       │
        │  Single source of truth. All inputs   │
        │  converge here. All outputs radiate   │
        │  from here.                           │
        └────────────────┬─────────────────────┘
                         │
                         ▼
              ┌──────────────────────┐
              │  BackendPlugin(s)    │
              │  DeltaChatBackend    │
              │  MockBackend         │
              │  (future: Imap, etc) │
              └──────────────────────┘
```

---

## Store as Firewall

Content flows one way and never backend→UI:

```
Backend (sync/push → writes store, emits storageChanged)
   ⇥  [ Email Store: .eml shards + SQLCipher FTS index ]  ⇥
Content Pipeline (KMime parse → shared sanitize)  →  Controller (read-through)  →  Panels
```

### Rules

- **Backends never serve UI content.** Their job is to fill the store
  (sync loops, push events, on-demand fetches) and emit
  `storageChanged`. Lists and bodies read from the store, not from a
  live backend call.
- **The store holds raw truth.** Sanitization happens at read time
  (defense in depth — a renderer change never re-exposes stored
  poison).
- **One content pipeline** (`src/core/content/`): KMime parses the
  `.eml`; `HtmlSanitizer` (the shared Rust core) sanitizes HTML for
  Qt's rich-text subset. No backend-specific parsing or sanitizing.
- **The store is a shared platform service** (`src/core/store/`), not
  an IMAP implementation detail. Backends must not call each other —
  they call the store and the pipeline.
- **storageChanged means "the store changed; re-read."** Payload push
  signals (`messageArrived` et al.) are a documented live-thread
  preview of what was just stored — never a source of truth.

---

## Universal Message Content Contract

Message body presentations are **universal controller state**, not
backend output. Any source (email on disk, a future chat protocol, an
ephemeral in-memory message, an archive import) fills the same fields;
views only project them.

### Body representations

Per open message, these full documents:

| Field | Meaning |
|-------|---------|
| `raw` | verbatim source (`.eml` for email; may be empty elsewhere) |
| `textOnly` | plain text for reading |
| `readerHtml` | calm, structure-aware HTML subset (default email view) |
| `sanitizedHtml` | full Qt-safe sanitize for the HTML view |

For email-on-disk the **EmailStore** facade implements them
(`readBodyViews`: one shard read + one pipeline parse yields all of
them, plus `headers` and envelope projections). Other sources fill the
same fields without EmailStore. **Unsanitized HTML never enters state.**

### Open-message state

The controller owns **one open-conversation context**; currently the
message panel hosts a single message (N=1). The shape is a list of
message records (`messages[]`) so multi-message threads are the same
contract later. Account/conversation/selection change **resets** the
whole state — no cross-account leftovers.

### Renderer rules

- The pane is dumb: a field is present → paint it; it changes →
  re-paint the **full** value. There is no chunk/stream protocol in
  QML.
- Progressive feel ("butter") is a **controller state-update policy**,
  never a store API family.
- Presentation chrome (the Raw | Text | Reader | HTML pill, columns,
  bubbles) is **view-local**; the controller doesn't know the pill
  exists. Panels are per-type and eventually pluginable.

### Headers

Universal per-message bag on the open state:

```text
headers: dict[string, array<dict[string, string>>]
```

- **List** = repetition of instances (To×N, Received×N).
- **Inner dict** = facets of one instance; multi-key when facets belong
  together (mailbox `{display, addr}`), single-key for simple values
  (`{value}` / `{raw}`).
- Scalars like Subject: one list element with combined facets
  (`{raw, text}`).
- Email keys are RFC5322 names; chat keys are namespaced
  (`matrix.sender`, `xmpp.type`, …).
- Deeper structure goes in a string facet (JSON allowed).
- Start sparse; enrichment (contacts, avatars) hydrates facets of the
  same instance later.
- Optional envelope projections (`fromDisplay`, `when`) keep list and
  bubble chrome from parsing the bag.

### What this forbids

- Backend-provided body text/HTML driving the pane (store-only cut;
  `messageBodyStored` + `readBodyViews` is the email path).
- A browser engine (WebEngine/WebView) for mail HTML — Reader +
  sanitizedHtml on Qt rich-text is the ceiling.
- Re-introducing a size-sliced "blocks" list view as the product HTML
  path (that experiment was abandoned).

---

## Pattern A: Async RPC coordination via QFuture

Backends often need to compose multiple asynchronous operations —
send a JSON-RPC request, wait for the response, send another one,
handle errors along the way. Aerogram uses `QFuture` / `QPromise` as
the promise primitive for this.

### The primitive

Every backend that speaks a request/response protocol should provide
a private helper that turns a single call into a `QFuture`:

```cpp
QFuture<QJsonValue> call(const QString &method, const QJsonArray &params);
```

Internal contract:

- Allocate a fresh request id.
- Create a shared `QPromise<QJsonValue>`; store it under the id.
- Write the request to the transport.
- Return the promise's future to the caller.

When the response arrives, look up the promise by id and either
`addResult(...); finish()` on success or `setException(...); finish()`
on failure. The exception type is `RpcError` (a `std::runtime_error`
subclass), so `.onFailed([](const std::exception &e){...})` catches it
uniformly.

### Composing sequential operations

Chain with `.then()`. When the callback itself returns a future,
follow with `.unwrap()` to flatten `QFuture<QFuture<T>>` back to
`QFuture<T>`:

```cpp
call("set_config_from_qr", {m_accountId, qrContent})
    .then([this](QJsonValue) {
        return call("configure", {m_accountId});
    }).unwrap()
    .then([this](QJsonValue) {
        return call("start_io", {m_accountId});
    }).unwrap()
    .then([this](QJsonValue) {
        emit configured(true);
        emit ioStarted(true, QString());
    })
    .onFailed([this](const std::exception &e) {
        emit configured(false);
        emit errorOccurred(QString::fromUtf8(e.what()));
    });
```

Any exception thrown anywhere in the chain — including from the
internal `setException` on RPC error — is caught by the trailing
`.onFailed`. There is no per-step error handler; error paths funnel
into one place.

### Composing parallel operations (fan-out)

Use `QtFuture::whenAll` on a container of futures. Aggregate results
in the follow-up `.then`:

```cpp
QList<QFuture<QJsonValue>> details;
for (int chatId : chatIds)
    details.append(call("get_full_chat_by_id", {m_accountId, chatId}));

return QtFuture::whenAll(details.begin(), details.end())
    .then([this](QList<QFuture<QJsonValue>> results) {
        // build aggregate result, emit signal
    });
```

### Chain-end type mechanics (Qt 6.5 gotcha)

`.onFailed(handler)` requires `handler` to return the same type as the
future it terminates. If the chain ends in `QFuture<QJsonValue>` but
the failure handler wants to just do side effects, insert a
`.then([](QJsonValue){})` to convert to `QFuture<void>` first. Chains
that already end in `void` (because the final `.then` returns void)
work with a void-returning `.onFailed` handler directly.

### Rules for backends

- **Every outbound request goes through `call()`.** No hand-rolled
  request-id / pending-map bookkeeping outside the primitive.
- **Every chain must end in `.onFailed()`.** Silent failures are
  bugs, even in "shouldn't happen" cases.
- **Success paths emit typed backend signals.** Failure paths emit
  `errorOccurred` and any relevant negative signal (e.g.
  `configured(false)`, `ioStarted(false, msg)`).
- **State transitions of interest** — start/stop IO, connection
  status, configuration progress — **become typed backend signals**,
  bubbled up through the controller. External observers (QML, IPC)
  react via `NOTIFY` and JSON-RPC events.

### Why futures over raw callbacks

- Sequential composition reads linearly, no nested closures.
- One exception path replaces per-step error handlers.
- Cancellation is a language-level primitive (`QFuture::cancel`).
- Timeout wrappers can be layered as ordinary future transformations.
- Fan-out/whenAll replaces manual counters and shared-pointer state
  bags.

The earlier callback-based implementation is preserved in git history
for reference. Pull it out when hunting for equivalent behavior;
otherwise, this section is the current contract.

---

## Data Models

Large, ordered data sets (chat list, message list, account list) are
exposed to QML as `QAbstractListModel` subclasses. They are owned by
`AccountController` and populated by controller code in response to
backend signals.

### Flow for a data fetch

```
Backend receives data
    → emits typed signal (e.g. conversationsReady(QVector<Conversation>))
    → AccountController slot handles it
    → AccountController calls setConversations() on the model
    → Model emits beginResetModel / endResetModel
    → QML ListView re-renders from the model
```

### Rules

- Models are populated only by the controller. Backends do not touch
  models directly.
- QML consumes models as read-only through `Q_PROPERTY`. Mutations go
  through controller slots.
- Role names in `roleNames()` are the model's contract with QML. They
  are semantic (`senderName`, `messageText`, `timestamp`), not
  positional.

---

## BackendPlugin Architecture

Aerogram talks to multiple chat and email services through a shared
vocabulary: `Account → Conversation → Message`. A `Conversation` has
a `kind` discriminator (`"chat"` for chat backends, `"folder"` for
email v1, `"thread"` later), so one interface serves both domains.

### Lifecycle base + capability interfaces

`BackendPlugin` is a small QObject base owning only lifecycle
(`initialize`, `shutdown`, `startIo`, `stopIo`) and **all signals**.
Content operations live in pure-C++ capability interfaces in
`Capabilities.h`:

- `IConversationProvider` — fetchConversations()
- `IMessageProvider` — fetchMessages(), fetchMessageBody()
- `IMessageSender` — sendMessage()
- `IQrSetup` — Delta Chat QR setup flows
- `ICredentialsSetup` — host/user/password setup (IMAP, Proton)

Backends multiple-inherit: `BackendPlugin` first (QObject base), then
whichever capabilities they support. The controller probes with
`dynamic_cast<IConversationProvider*>(plugin)` and emits
`errorOccurred` when a capability is absent. Signals live on the base
so the controller connects once, against `BackendPlugin*`, regardless
of which capabilities exist.

Why signals-on-base rather than QObject capability interfaces: it
avoids moc/Q_INTERFACES diamond-inheritance machinery, and the
controller's connect() list stays fixed as backends vary.

### The controller aggregates across backends

**(TARGET STATE)** Currently the controller holds a single backend
pointer, chosen at startup via `--backend=deltachat|imap|mock`.
Multi-backend aggregation (compound account IDs like `"delta:1"`) is
planned.

### Rules

- Backends do not know about each other.
- Backends do not know about the controller's other clients (QML,
  IPC).
- Backends emit typed signals only. No callbacks into the controller.
- Adding a backend means implementing the lifecycle base plus the
  capability interfaces it supports. No other layer changes.
- Blocking I/O (network, DB, files) never runs on the UI thread.
  Backends use `QtConcurrent` workers and deliver results via queued
  signal emission.

### Push events

Backends with a live change channel (Delta Chat's event batch
long-poll, the Proton SDK's local-store watchers) emit the **push
signal family** on the base:

- `messageArrived(conversationId, message)` — payload; the controller
  appends to the open conversation (dedup by id) and models grow
  targeted row mutations instead of resets.
- `conversationUpserted(conversation)` — payload; sidebar badges and
  previews update in place.
- `messageRemoved(conversationId, messageId)` — tombstone.
- `storageChanged()` — coarse fallback for bursts/ambiguous changes;
  the controller debounces (300ms per account) into a targeted refetch.

Payload-carrying signals are preferred where the backend knows the
data cheaply; `storageChanged` is always correct. Poll-based backends
(IMAP) may instead implement the `ISyncable` capability, which the
controller pokes on account activation. Push events ride the same
unidirectional flow as everything else — no QML changes.

---

## Non-Goals

Design choices that are deliberately excluded, and why.

- **No two-way bindings between components and controller.**
  Components read; controller writes. Two-way bindings reintroduce
  desync risks and make it harder to reason about state changes.

- **No generic event bus.**
  Signals are always specific and typed. Adding a shared bus creates
  a category of code that inspects and re-dispatches events —
  precisely the string-parsing anti-pattern Pattern 1 forbids.

- **No direct component-to-component signaling.**
  Components must not import each other. All coordination happens
  through the controller.

- **No polling for state.**
  If a property matters, it has a `NOTIFY` signal. Timers are for
  time-based work (e.g. periodic sync), not for observing state.

- **No side effects during property reads.**
  Getters return current state. Fetching, refreshing, and mutating
  happen through slots.

---

## Walkthrough — a correct user flow end to end

**(TARGET STATE)** — the sidebar refactor and controller property
hoisting are planned. This walkthrough describes the intended behavior
once those changes land.

The user clicks the account icon for `alice@delta` in the sidebar.

1.  The `Sidebar` component fires a semantic signal:
    ```qml
    signal accountSelected(string accountId)
    // ...
    onClicked: sidebar.accountSelected("delta:1")
    ```

2.  The orchestrator (`main.qml`) handles it declaratively:
    ```qml
    Sidebar {
        onAccountSelected: (id) => accountController.selectAccount(id)
    }
    ```

3.  `AccountController::selectAccount("delta:1")` updates the active
    account, emits `NOTIFY`, and asks the correct backend to refresh:
    ```cpp
    void AccountController::selectAccount(const QString &id) {
        m_activeAccountId = id;
        emit activeAccountIdChanged();
        m_deltaBackend->selectAccount(1);
        m_deltaBackend->fetchChatList();
    }
    ```

4.  The sidebar's highlight binding re-evaluates because
    `activeAccountId` changed:
    ```qml
    highlighted: accountController.activeAccountId === "delta:1"
    ```
    The icon now shows as selected.

5.  When the backend returns chats, the controller updates the model:
    ```cpp
    void AccountController::onChatListReady(const QVector<ChatMessage> &chats) {
        m_chatModel->setChatMessages(chats);
    }
    ```
    The `ListView` re-renders.

6.  Meanwhile, if an external IPC client had called
    `selectAccount("delta:1")` instead of the user clicking, steps 3–5
    would be identical. The sidebar highlight and chat list would
    update the same way. There is no separate code path.

Every input converges on the controller. Every output radiates from
the controller through `NOTIFY`. Components stay simple.

---

## Rules Summary

1.  New UI signals must be semantic and typed.
2.  Components must not own semantic state. Read from controller
    `Q_PROPERTY`; emit signals up.
3.  New controller slots become IPC methods automatically (target
    state). New controller signals become IPC events automatically
    (target state). Do not hand-wire IPC dispatch.
4.  Do not bypass the controller.
5.  Every controller state exposed to QML must have `NOTIFY`.
6.  Models are populated only by the controller.
7.  Backends implement the plugin interface and know nothing about
    other layers.

Enforcement checklist lives in `AGENTS.md`.
