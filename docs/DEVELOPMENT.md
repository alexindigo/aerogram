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

Plain launch with no accounts anywhere boots into the **first-run
vault form** (master password + Secret Key phrase). Add accounts with
the **+** button (bottom of the sidebar). Daily launches with an
existing vault show the password-only unlock.

Backends are explicit: `--backend=deltachat|mock|imap`,
`--accounts=<file>`, or persisted accounts from the config file. No
backend starts by default.

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
  (per-account **SQLCipher-encrypted** `index.db` + sharded
  **encrypted** `storage/<hh>/<hh>/<sha>.enc`)
- Vault: `~/.local/share/Aerogram/vault/` (`wrap-salt.bin`,
  `secret-key.enc`, `keycheck.enc`, `accounts.db` — the encrypted
  accounts table; there is no plaintext accounts file)
- Icon pack: `~/.local/share/Aerogram/icons/default/` (Tabler SVGs,
  extracted from the bundle on first run; user-replaceable by design —
  future: user packs as sibling dirs + a switcher)
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

Accounts from the CLI (`--accounts` / `--imap-*`) are runtime-only and
never persisted; accounts added via the **+** dialog persist into the
encrypted vault DB (`vault/accounts.db`) and auto-load post-unlock.
`removeAccount` drops both the runtime backend and the persisted row
(the on-disk store is kept; re-adding reuses it). A legacy plaintext
`accounts.json` is imported into the vault DB once at first unlock and
renamed to `accounts.json.migrated`.

### Encryption at rest + lock screen

IMAP storage is encrypted: the DB is SQLCipher (AES-256) and each
message file is ChaCha20-Poly1305 (libsodium secretbox). The data key
is derived via Argon2id from your **master password + Secret Key
phrase** (a memorable text you choose at first run, e.g. "my cat is
often grumpy in the mornings"). The phrase is the portability
artifact: password + phrase alone open any recovered `.enc` file, no
vault artifact needed. The store is a cache — losing both secrets
means re-init + resync from the servers.

Vault files (`~/.local/share/Aerogram/vault/`, all 0600):

| File | Contents |
|------|----------|
| `wrap-salt.bin` | random 16-byte wrap salt, plaintext (not secret) |
| `secret-key.enc` | phrase encrypted with password-derived wrap key |
| `keycheck.enc` | known-plaintext check encrypted with the data key |

When any `imap:` account exists, Aerogram boots to a lock overlay:

- **First run** (no vault): set master password + Secret Key phrase
  (confirm both). The phrase is stored encrypted; daily unlocks need
  only the password.
- **Daily**: enter the master password.
- **Recovery** (`secret-key.enc` lost/corrupt, or archive copied to a
  fresh machine): click "Vault damaged? Recover with Secret Key",
  enter password + phrase; the daily-unlock box is rewritten.

Wrong password or phrase leaves the app locked with no IO. Rotation
(`rotateVault` slot, mode `wipe-resync`) rewrites the vault, wipes the
local store, and resyncs.

Headless testing can drive the vault over IPC (slots are reflective):

```bash
# app running with --accounts=dev/test/accounts.json, then:
python3 dev/test/ipc-drive.py   # includes the createVault step
```

To reset the vault (loses access to all encrypted stores):

```bash
rm -rf ~/.local/share/Aerogram/vault ~/.local/share/Aerogram/imap
```

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
