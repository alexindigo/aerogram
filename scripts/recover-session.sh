#!/usr/bin/env bash
#
# recover-session.sh
#
# One-shot recovery wrapper for the atmogram -> aerogram session
# migration described in docs/SESSION-RECOVERY.md.
#
# Run this from a plain shell AFTER quitting every opencode TUI you
# have open (including the one in ~/Projects/aerogram). Domovoy's
# opencode serve (different user) can stay running.
#
# What it does:
#   1. Verifies no opencode process for $USER is running.
#   2. Verifies nothing holds ~/.local/share/opencode/opencode.db.
#   3. Runs scripts/migrate-opencode-session.sh (prompts once for y/N).
#   4. Verifies stale-reference counts are zero.
#   5. Confirms the salvaged session + project now point at the new path.
#   6. Reports snapshot config files that still mention the old path.
#   7. Prints relaunch instructions.
#
# Rollback (only if step 7 misbehaves):
#   ls -lt ~/.local/share/opencode/opencode.db.pre-migrate-* | head
#   cp ~/.local/share/opencode/opencode.db.pre-migrate-<ts> \
#      ~/.local/share/opencode/opencode.db

set -euo pipefail

OLD_PATH="/home/user/Projects/atmogram"
NEW_PATH="/home/user/Projects/aerogram"
DB="$HOME/.local/share/opencode/opencode.db"
SESSION_ID="ses_0ab6a634dffeXeoHKUxv995hQm"
PROJECT_ID="e2b4fbab0694323efe14664093598a13d6c9557b"
REPO_ROOT="$NEW_PATH"

hr() { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
ok() { printf '\033[1;32mOK\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------
hr "Step 1: preconditions"
# ---------------------------------------------------------------------

[ -f "$DB" ] || die "opencode DB not found at $DB"
[ -d "$NEW_PATH" ] || die "new project path missing: $NEW_PATH"
[ -x "$REPO_ROOT/scripts/migrate-opencode-session.sh" ] \
    || die "migration script missing or not executable"

if pgrep -x -u "$USER" opencode >/dev/null 2>&1; then
    echo "opencode processes still running for $USER:" >&2
    pgrep -af -u "$USER" opencode >&2
    die "quit them all (including any TUI you're sitting at) and re-run"
fi
ok "no opencode process for $USER"

if command -v lsof >/dev/null 2>&1; then
    if lsof -- "$DB" >/dev/null 2>&1; then
        echo "something still has the DB open:" >&2
        lsof -- "$DB" >&2 || true
        die "release the DB and re-run"
    fi
    ok "DB is not held"
else
    echo "(lsof not installed; skipping DB-holder check)"
fi

# ---------------------------------------------------------------------
hr "Step 2: pre-migration stale counts (expected: 1 1 318 92 40, approx)"
# ---------------------------------------------------------------------
sqlite3 -readonly "$DB" \
    ".mode column" ".headers on" \
    "SELECT
       (SELECT COUNT(*) FROM project  WHERE worktree = '$OLD_PATH')  AS project_stale,
       (SELECT COUNT(*) FROM session  WHERE directory = '$OLD_PATH') AS session_stale,
       (SELECT COUNT(*) FROM message
          WHERE data LIKE '%\"cwd\":\"$OLD_PATH/%'
             OR data LIKE '%\"cwd\":\"$OLD_PATH\"%')                 AS msg_cwd_stale,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"filePath\":\"$OLD_PATH/%'
             OR data LIKE '%\"filePath\":\"$OLD_PATH\"%')            AS part_filepath_stale,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"workdir\":\"$OLD_PATH/%'
             OR data LIKE '%\"workdir\":\"$OLD_PATH\"%')             AS part_workdir_stale;"

# ---------------------------------------------------------------------
hr "Step 3: running migration script (will prompt once)"
# ---------------------------------------------------------------------
cd "$REPO_ROOT"
./scripts/migrate-opencode-session.sh "$OLD_PATH" "$NEW_PATH"

# ---------------------------------------------------------------------
hr "Step 4: post-migration stale counts (all must be 0)"
# ---------------------------------------------------------------------
POST=$(sqlite3 "$DB" \
    "SELECT
       (SELECT COUNT(*) FROM project  WHERE worktree = '$OLD_PATH')
     + (SELECT COUNT(*) FROM session  WHERE directory = '$OLD_PATH')
     + (SELECT COUNT(*) FROM message
          WHERE data LIKE '%\"cwd\":\"$OLD_PATH/%'
             OR data LIKE '%\"cwd\":\"$OLD_PATH\"%')
     + (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"filePath\":\"$OLD_PATH/%'
             OR data LIKE '%\"filePath\":\"$OLD_PATH\"%')
     + (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"workdir\":\"$OLD_PATH/%'
             OR data LIKE '%\"workdir\":\"$OLD_PATH\"%');")

if [ "$POST" != "0" ]; then
    die "stale references remain (sum=$POST). See per-column breakdown:
$(sqlite3 "$DB" '.mode column' '.headers on' \
    "SELECT
       (SELECT COUNT(*) FROM project  WHERE worktree = '$OLD_PATH')  AS project_stale,
       (SELECT COUNT(*) FROM session  WHERE directory = '$OLD_PATH') AS session_stale,
       (SELECT COUNT(*) FROM message
          WHERE data LIKE '%\"cwd\":\"$OLD_PATH/%'
             OR data LIKE '%\"cwd\":\"$OLD_PATH\"%')                 AS msg_cwd_stale,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"filePath\":\"$OLD_PATH/%'
             OR data LIKE '%\"filePath\":\"$OLD_PATH\"%')            AS part_filepath_stale,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"workdir\":\"$OLD_PATH/%'
             OR data LIKE '%\"workdir\":\"$OLD_PATH\"%')             AS part_workdir_stale;")"
fi
ok "all stale reference counts are 0"

# ---------------------------------------------------------------------
hr "Step 5: confirm target session + project now point at new path"
# ---------------------------------------------------------------------
sqlite3 "$DB" \
    ".mode column" ".headers on" \
    "SELECT id, directory, title FROM session WHERE id = '$SESSION_ID';
     SELECT id, worktree, name FROM project WHERE id = '$PROJECT_ID';"

SESSION_DIR=$(sqlite3 "$DB" "SELECT directory FROM session WHERE id = '$SESSION_ID';")
PROJECT_WT=$(sqlite3 "$DB" "SELECT worktree FROM project WHERE id = '$PROJECT_ID';")
[ "$SESSION_DIR" = "$NEW_PATH" ] || die "session.directory did not update: $SESSION_DIR"
[ "$PROJECT_WT"  = "$NEW_PATH" ] || die "project.worktree did not update: $PROJECT_WT"
ok "session + project both point at $NEW_PATH"

# ---------------------------------------------------------------------
hr "Step 6: snapshot config files still mentioning old path (rare)"
# ---------------------------------------------------------------------
SNAP_HITS=$(grep -rl "$OLD_PATH" "$HOME/.local/share/opencode/snapshot/" 2>/dev/null || true)
if [ -n "$SNAP_HITS" ]; then
    echo "$SNAP_HITS"
    echo
    echo "Hand-edit '[core] worktree = ...' in each of the above to $NEW_PATH."
    echo "Non-blocking: snapshots rarely cause user-visible breakage."
else
    ok "no snapshot configs reference the old path"
fi

# ---------------------------------------------------------------------
hr "Done"
# ---------------------------------------------------------------------
cat <<EOF
Relaunch:

    cd $NEW_PATH
    opencode

Open session '$SESSION_ID' from the TUI and send a prompt. It should
no longer hang.

Rollback (only if something's wrong):

    ls -lt $HOME/.local/share/opencode/opencode.db.pre-migrate-* | head
    cp $HOME/.local/share/opencode/opencode.db.pre-migrate-<ts> $DB
EOF
