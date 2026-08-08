#!/usr/bin/env bash
#
# recover-session-live.sh
#
# Live-migration variant of recover-session.sh. Runs against the
# opencode DB WHILE other opencode instances (owned by $USER or any
# other user) are still running.
#
# Safe because:
#   - SQLite WAL mode supports one writer + concurrent readers.
#   - The UPDATE statements only touch rows matching OLD_PATH; other
#     projects/sessions are not affected.
#   - Other running opencodes cache their own project/session data;
#     they won't observe these changes until restart, but they also
#     don't reference the rows we're touching.
#
# After running, restart ONLY the opencode instance in the aerogram
# project (this current TUI) so the salvaged session appears with
# corrected paths. Other TUIs can keep running.
#
# Rollback:
#   ls -lt ~/.local/share/opencode/opencode.db.pre-migrate-live-* | head
#   # Do NOT overwrite the live DB while opencode is running against it.
#   # Instead: quit ALL opencodes for $USER, then:
#   #   cp ~/.local/share/opencode/opencode.db.pre-migrate-live-<ts> \
#   #      ~/.local/share/opencode/opencode.db

set -euo pipefail

OLD_PATH="${1:-/home/user/Projects/atmogram}"
NEW_PATH="${2:-/home/user/Projects/aerogram}"
DB="$HOME/.local/share/opencode/opencode.db"
SESSION_ID="ses_0ab6a634dffeXeoHKUxv995hQm"
PROJECT_ID="e2b4fbab0694323efe14664093598a13d6c9557b"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/migration-sql.sh
. "$SCRIPT_DIR/lib/migration-sql.sh"

hr() { printf '\n\033[1;34m== %s ==\033[0m\n' "$*"; }
ok() { printf '\033[1;32mOK\033[0m %s\n' "$*"; }
die() { printf '\033[1;31mERROR\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------
# Sanity
# ---------------------------------------------------------------------
for p in "$OLD_PATH" "$NEW_PATH"; do
    case "$p" in
        *"'"*|*$'\n'*) die "unsafe character in path: $p" ;;
    esac
done

OLD_REL="${OLD_PATH#/}"
NEW_REL="${NEW_PATH#/}"
NEW_NAME=$(basename -- "$NEW_PATH")
export OLD_PATH NEW_PATH OLD_REL NEW_REL NEW_NAME

[ -f "$DB" ]       || die "opencode DB not found at $DB"
[ -d "$NEW_PATH" ] || die "NEW_PATH does not exist: $NEW_PATH"

# ---------------------------------------------------------------------
hr "Live migration: $OLD_PATH -> $NEW_PATH"
# ---------------------------------------------------------------------
echo "This runs WHILE other opencode instances are up. WAL mode makes"
echo "concurrent writes safe. Only your ($USER's) aerogram opencode"
echo "will need a restart afterward to observe the change on the"
echo "salvaged session."
echo

# Confirm WAL is on. If not, we still proceed but warn.
WAL_MODE=$(sqlite3 "$DB" 'PRAGMA journal_mode;' 2>/dev/null || echo "unknown")
if [ "$WAL_MODE" = "wal" ]; then
    ok "DB is in WAL mode ($WAL_MODE)"
else
    echo "WARNING: DB is not in WAL mode (journal_mode=$WAL_MODE)."
    echo "         Live migration is riskier; consider quitting all"
    echo "         opencode instances and using recover-session.sh"
    echo "         instead. Continuing anyway if you say yes."
fi

# ---------------------------------------------------------------------
hr "Pre-migration stale counts"
# ---------------------------------------------------------------------
emit_verification_sql | sqlite3 "$DB" -header -column

# ---------------------------------------------------------------------
hr "Target session + project current state"
# ---------------------------------------------------------------------
sqlite3 "$DB" -header -column \
    "SELECT id, directory, title FROM session WHERE id = '$SESSION_ID';
     SELECT id, worktree, name FROM project WHERE id = '$PROJECT_ID';"

echo
read -rp "Proceed with live migration? [y/N] " ANS
case "$ANS" in
    y|Y|yes|YES) ;;
    *) echo "Aborted. Nothing written."; exit 0 ;;
esac

# ---------------------------------------------------------------------
# WAL-safe snapshot: use sqlite3 .backup, which is safe against a live
# DB with concurrent readers/writers. Do NOT `cp` the raw file, which
# can capture an inconsistent WAL/main-file pair.
# ---------------------------------------------------------------------
TS=$(date +%Y%m%d-%H%M%S)
SNAP="$DB.pre-migrate-live-$TS"
hr "Snapshotting DB via sqlite3 backup (WAL-safe)"
sqlite3 "$DB" ".backup '$SNAP'"
ok "snapshot at $SNAP"

# ---------------------------------------------------------------------
hr "Applying migration transaction"
# ---------------------------------------------------------------------
emit_migration_sql | sqlite3 "$DB"
ok "transaction committed"

# ---------------------------------------------------------------------
hr "Post-migration stale counts (all must be 0)"
# ---------------------------------------------------------------------
emit_verification_sql | sqlite3 "$DB" -header -column

# Numeric sum check.
POST=$(emit_verification_sql \
    | sed 's/SELECT/SELECT (/;s/ AS [a-z_]*//g;s/;$/);/' \
    | sqlite3 "$DB" 2>/dev/null || true)
# Fallback: just recompute by summing counts.
POST_SUM=$(sqlite3 "$DB" "
    SELECT
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

[ "$POST_SUM" = "0" ] || die "stale references remain (total=$POST_SUM)"
ok "all stale reference counts are 0"

# ---------------------------------------------------------------------
hr "Target session + project after migration"
# ---------------------------------------------------------------------
sqlite3 "$DB" -header -column \
    "SELECT id, directory, title FROM session WHERE id = '$SESSION_ID';
     SELECT id, worktree, name FROM project WHERE id = '$PROJECT_ID';"

SESSION_DIR=$(sqlite3 "$DB" "SELECT directory FROM session WHERE id = '$SESSION_ID';")
PROJECT_WT=$(sqlite3 "$DB" "SELECT worktree FROM project WHERE id = '$PROJECT_ID';")
[ "$SESSION_DIR" = "$NEW_PATH" ] || die "session.directory did not update: $SESSION_DIR"
[ "$PROJECT_WT"  = "$NEW_PATH" ] || die "project.worktree did not update: $PROJECT_WT"
ok "session + project both point at $NEW_PATH"

# ---------------------------------------------------------------------
hr "Snapshot config files still mentioning old path (rare)"
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
Restart ONLY the opencode instance in this project so it picks up the
DB changes for the salvaged session:

    # from pts/12 (the aerogram opencode):
    Ctrl+C to quit, then:
    cd $NEW_PATH
    opencode

Open session '$SESSION_ID' from the TUI and send a prompt. It should
no longer hang.

Other opencode instances (other ptses, other projects) do NOT need to
restart — their data was untouched.

Rollback (only if something's wrong, and only after quitting ALL your
opencodes so nothing is writing to the DB):

    cp $SNAP $DB
EOF
