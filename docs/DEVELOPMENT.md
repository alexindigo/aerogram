# Development

## Build

```bash
cmake -B build -S .
cmake --build build
```

## Run

```bash
./build/aerogram
```

Or headless (no display required — useful for smoke testing that the
QML and C++ layers load without errors):

```bash
timeout 3 ./build/aerogram --platform offscreen
```

Exit code `124` from `timeout` means the app launched successfully and
was killed by the timeout (a GUI app in its event loop). Any other
non-zero exit indicates an error.

## Runtime locations

- Delta Chat account storage: `~/.config/aerogram/delta/`
- IMAP prototype storage: `~/.local/share/Aerogram/imap/<user>@<host>/`
  (per-account `index.db` + sharded `storage/<hh>/<hh>/<sha>.eml`)
- IPC socket: `~/.cache/Aerogram/aerogram.ipc`

Note: this system routes Qt logs to journald by default. Prefix runs
with `QT_FORCE_STDERR_LOGGING=1` to see qInfo/qWarning on the console.

## IPC smoke test

Requires the app to be running:

```bash
python3 -c "
import socket, json, os
s = socket.socket(socket.AF_UNIX)
s.connect(os.path.expanduser('~/.cache/Aerogram/aerogram.ipc'))
s.sendall(b'{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}\n')
print(s.recv(4096))
"
```

Expected response: `{"id":1,"jsonrpc":"2.0","result":"pong"}`

## Clean rebuild

```bash
rm -rf build
cmake -B build -S .
cmake --build build
```

## Reset Delta Chat state

To wipe all accounts and start fresh:

```bash
rm -rf ~/.config/aerogram/delta/
```

## IMAP backend prototype

The IMAP backend uses libcurl transport + a minimal in-tree MIME
parser + hash-sharded `.eml` storage + SQLite (WAL + FTS5). See
`~/Documents/Aerogram/plans/imap-backend-prototype/plan.md`.

Start the local Dovecot test server (Docker, port 1143, user/pass
`test`/`test`, folders INBOX + Dev with seeded mail):

```bash
dev/test/start-test-imap.sh
```

Run Aerogram against it:

```bash
./build/aerogram --backend=imap --imap-host=localhost \
    --imap-port=1143 --imap-user=test --imap-pass=test
```

Add `--imap-tls` for IMAPS servers (e.g. port 993). For multiple
accounts, use a JSON file instead (passwords stay off the command
line):

```json
[
  {"type": "imap", "host": "localhost", "port": 1143, "user": "test",  "pass": "test",  "tls": false},
  {"type": "imap", "host": "imap.example.com", "port": 993, "user": "alice", "pass": "...", "tls": true}
]
```

```bash
./build/aerogram --accounts=dev/test/accounts.json
```

The container ships two users (`test`/`test`, `test2`/`test2`), so
`dev/test/accounts.json` exercises multi-account locally.

### Adding accounts from the UI

The **+** button (top of the sidebar) opens the add-account dialog
(host, port, user, password/app-password, TLS). Submitted accounts
start syncing immediately and persist to
`~/.config/Aerogram/accounts.json` (0600, owner-only) so they auto-load
on every later launch — no CLI flags, nothing in shell history.

Accounts from the CLI (`--accounts` / `--imap-*`) and the persisted
file are merged with dedup by `imap:user@host`.

Folder list appears in the chats (💬) view with per-account labels;
selecting a conversation fills the email view (list left, reading pane
right). Clicking a message shows its body in the pane; attachments
have Save buttons (Save As dialog, decode-on-save — no app-side copy).

Headless end-to-end check (requires the test server running):

```bash
QT_FORCE_STDERR_LOGGING=1 QT_QPA_PLATFORM=offscreen ./build/aerogram \
    --accounts=dev/test/accounts.json &
python3 dev/test/ipc-drive.py
```

Expected: ping, both accounts' messagesChanged, messageBodyReady,
attachmentSaved + content check, `ALL OK`.
