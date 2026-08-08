# Opencode session recovery

This document explains a specific opencode session that is at risk of
being orphaned by a project directory rename, and how a fresh opencode
session — invoked separately — can recover it.

If you're a fresh opencode session that the user pointed here for
context: **read this file top to bottom, then check the current DB
state before acting.** The situation may have already been resolved.

---

## What happened

The project directory was renamed on disk mid-session:

```
/home/user/Projects/atmogram  →  /home/user/Projects/aerogram
```

Opencode stores absolute paths in multiple columns of its SQLite DB
(`~/.local/share/opencode/opencode.db`). None of them were rewritten
by the filesystem `mv`, so the session that was created before the
rename now points at a directory that no longer exists.

The relevant identifiers for this specific case:

| Kind | Value |
|------|-------|
| Old path | `/home/user/Projects/atmogram` |
| New path | `/home/user/Projects/aerogram` |
| Project ID | `e2b4fbab0694323efe14664093598a13d6c9557b` |
| Session ID | `ses_0ab6a634dffeXeoHKUxv995hQm` |
| Session title | `New session - 2026-07-12T04:28:38.962Z` |

The session's message history is intact in the DB. Only the paths are
stale.

---

## The symptoms

Any of these indicate the migration hasn't been done (or was reverted):

- The session doesn't appear in the TUI's session list when opencode
  is launched from `~/Projects/aerogram`.
- Opening the session (by ID) shows the conversation history, but
  sending any prompt hangs, silently aborts, or errors on file-picker
  init.
- Logs show `Failed to init file picker: Invalid path
  /home/user/Projects/atmogram`.

---

## The fix — one command

A migration script is committed to this repo:

```
scripts/migrate-opencode-session.sh
```

Backed by the skill at
`~/.config/opencode/skills/opencode-session-migration/SKILL.md`,
which documents the mechanics.

**Steps**:

1. Quit the running opencode instance completely. From a shell that
   is not inside opencode:

    ```bash
    pgrep -af opencode
    ```

    Should be empty. If not, quit those processes (Ctrl+C from the
    TUI, or `kill <pid>` for a stuck one). Confirm again.

2. From the project root, run the script:

    ```bash
    cd ~/Projects/aerogram
    ./scripts/migrate-opencode-session.sh
    ```

    Defaults are `atmogram` → `aerogram`. For other renames, pass
    explicit arguments:

    ```bash
    ./scripts/migrate-opencode-session.sh /old/path /new/path
    ```

3. The script does the following:
    - Refuses to run if opencode is still holding the DB.
    - Snapshots the DB to
      `~/.local/share/opencode/opencode.db.pre-migrate-<timestamp>`.
    - Shows what will be affected and asks for confirmation.
    - Runs the SQL updates in a single transaction.
    - Verifies (all stale-row counts should be 0).
    - Prints the post-migration state and restart instructions.

4. Restart opencode from the new path:

    ```bash
    cd ~/Projects/aerogram
    opencode
    ```

5. The session should appear in the list under the "aerogram"
   project, with full history.

---

## If the migration failed or the session is still broken

### Rollback

Every script run writes a snapshot before making changes. Restore
from the most recent one:

```bash
ls -lt ~/.local/share/opencode/opencode.db.pre-migrate-* | head
cp ~/.local/share/opencode/opencode.db.pre-migrate-<timestamp> \
   ~/.local/share/opencode/opencode.db
```

Restart opencode. You're back to pre-migration state.

### Find the session directly

Even if the TUI doesn't show it, the row is in the DB:

```bash
sqlite3 -readonly ~/.local/share/opencode/opencode.db \
  "SELECT id, directory, path, title FROM session
    WHERE id = 'ses_0ab6a634dffeXeoHKUxv995hQm';"
```

Or by list:

```bash
opencode session list
```

### Check for stale references that survived

If the migration ran but tool calls still misbehave, the JSON blob
rewrites may have missed some. Diagnose:

```bash
sqlite3 -readonly ~/.local/share/opencode/opencode.db \
  "SELECT
     (SELECT COUNT(*) FROM project  WHERE worktree = '/home/user/Projects/atmogram')  AS project_stale,
     (SELECT COUNT(*) FROM session  WHERE directory = '/home/user/Projects/atmogram') AS session_stale,
     (SELECT COUNT(*) FROM message
        WHERE data LIKE '%\"cwd\":\"/home/user/Projects/atmogram%')                   AS msg_cwd_stale,
     (SELECT COUNT(*) FROM part
        WHERE data LIKE '%\"filePath\":\"/home/user/Projects/atmogram%')              AS part_filepath_stale,
     (SELECT COUNT(*) FROM part
        WHERE data LIKE '%\"workdir\":\"/home/user/Projects/atmogram%')               AS part_workdir_stale;"
```

Every count should be 0. If not, re-run the script — the WHERE
clauses are idempotent.

### Snapshot config files (rarely needed)

Opencode also keeps git-based snapshots under
`~/.local/share/opencode/snapshot/`. Their internal `config` files
may reference the old path via `worktree = /home/user/Projects/atmogram`.
Rarely causes user-visible breakage, but if snapshots fail after the
DB migration:

```bash
grep -rl "/home/user/Projects/atmogram" ~/.local/share/opencode/snapshot/ 2>/dev/null
```

For each match, edit the `[core] worktree = ...` line to the new
path. The skill's history-principle applies here too — do not
rewrite git objects, only functional config.

---

## What we deliberately do not rewrite

Per the "history principle" of the skill:

- Tool call outputs and reasoning text inside `part.data`. These
  record what tools returned and what the assistant was thinking at
  the time. The cwd *was* the old path when those events happened.
- Loose files in `log/`, `storage/`, `tool-output/`.
- Assistant text bodies (`"text":"..."` fields in messages) that
  mention the old path in conversation.

Continued conversation will naturally use the new path via the
functional-field rewrites we do apply.

---

## References

- **Skill**: `~/.config/opencode/skills/opencode-session-migration/SKILL.md`
  Full mechanics, column-by-column rewrite table, boundary-safety
  notes, edge cases.

- **Script**: `scripts/migrate-opencode-session.sh` in this repo.
  Executable form of the skill's SQL, with sanity checks, snapshot,
  and verification.

- **Related opencode issues** (all confirm this is a known problem
  with no native fix at the time this session was in flight):
  - anomalyco/opencode#23248 — Sessions become orphaned when project
    directory is renamed.
  - anomalyco/opencode#33909 — Moving a project directory breaks all
    existing sessions.
  - anomalyco/opencode#29703 — Feature request: allow changing
    project folder path without losing session history.
  - anomalyco/opencode#25625 — Feature request: allow renaming or
    moving a project folder while persisting history.

---

## What the salvaged session was doing

Context in case the fresh session needs to pick up where we left off:

We were mid-planning of a D-Bus IPC exposure for Aerogram, having
just:

- Finalized `docs/ARCHITECTURE.md` (three interlocking patterns:
  semantic callback handlers, unidirectional data flow, behind-the-
  scenes IPC wiring, plus Pattern A on QFuture-based async).
- Documented `docs/CLEANUP.md` with undiscussed patterns B–G.
- Refactored the opencode-session-migration skill to cover both home
  migration and project rename.

Open decisions when the migration was queued:

- **DB-3a** — Bus name prefix (`dev.aerogram.*`, `com.alexindigo.Aerogram`,
  `chat.aerogram.*`, or other).
- **DB-3b** — Per-backend paths recommended; awaiting confirmation.
- **DB-5 / DB-6** — Method/property/signal surface review.
- **Q6** — Backend status shape (fixed schema + `details` recommended).
- **T-2** — Test language (Python + `dbus-next` recommended).
- **T-3** — Trace record/replay (defer to later recommended).

If you're a fresh session picking this up: read `docs/ARCHITECTURE.md`
and `docs/CLEANUP.md` first, then ask the user which of the above
open questions to resolve next.
