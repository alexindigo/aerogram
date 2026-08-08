#!/usr/bin/env bash
#
# migrate-opencode-session.sh
#
# Rewrite opencode DB rows after a project directory rename so this
# session (and any other session tied to the old path) is visible in
# the TUI and can be continued without stale tool-call references.
#
# Implements the "opencode-session-migration" skill:
#   ~/.config/opencode/skills/opencode-session-migration/SKILL.md
#
# What it rewrites in ~/.local/share/opencode/opencode.db:
#   project.worktree, project.name        where worktree = OLD_PATH
#   session.directory                     where directory = OLD_PATH
#   session.path                          where path = OLD_REL
#   message.data:  "cwd":"OLD_PATH{/,"}         -> NEW_PATH
#   part.data:     "filePath":"OLD_PATH{/,"}    -> NEW_PATH
#   part.data:     "workdir":"OLD_PATH{/,"}     -> NEW_PATH
#
# What it deliberately leaves alone (history principle):
#   Tool call outputs, reasoning text, assistant text bodies that
#   mention the old path. Loose files under log/, storage/,
#   tool-output/, and snapshot/ directories.
#
# Usage (default: atmogram -> aerogram):
#   Quit opencode first, then:
#     ./scripts/migrate-opencode-session.sh
#
# Or with explicit args for other renames:
#   ./scripts/migrate-opencode-session.sh /old/path /new/path
#
# Idempotent. Re-running with the same args after success is a no-op.

set -euo pipefail

OLD_PATH="${1:-/home/user/Projects/atmogram}"
NEW_PATH="${2:-/home/user/Projects/aerogram}"
DB="$HOME/.local/share/opencode/opencode.db"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/migration-sql.sh
. "$SCRIPT_DIR/lib/migration-sql.sh"

# ---------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------

for p in "$OLD_PATH" "$NEW_PATH"; do
    case "$p" in
        *"'"*|*$'\n'*)
            echo "ERROR: unsafe character in path: $p" >&2
            exit 1
            ;;
    esac
done

OLD_REL="${OLD_PATH#/}"
NEW_REL="${NEW_PATH#/}"
NEW_NAME=$(basename -- "$NEW_PATH")

[ -f "$DB" ]       || { echo "ERROR: no opencode DB at $DB" >&2; exit 1; }
[ -d "$NEW_PATH" ] || { echo "ERROR: NEW_PATH does not exist: $NEW_PATH" >&2; exit 1; }

# Refuse if opencode is still running. WAL mode would technically
# allow the writes, but the running client caches session/project data
# in memory and won't see the changes until restart. Better to shut
# down cleanly first.
if command -v lsof >/dev/null 2>&1 && lsof -- "$DB" >/dev/null 2>&1; then
    echo "ERROR: something has $DB open. Quit opencode and re-run." >&2
    lsof -- "$DB" >&2 || true
    exit 1
fi
if pgrep -x -u "$USER" opencode >/dev/null 2>&1; then
    echo "ERROR: opencode process is running (as $USER). Quit it and re-run." >&2
    exit 1
fi

# ---------------------------------------------------------------------
# Snapshot
# ---------------------------------------------------------------------

TS=$(date +%Y%m%d-%H%M%S)
SNAP="$DB.pre-migrate-$TS"
cp -v -- "$DB" "$SNAP"

# ---------------------------------------------------------------------
# Show what will be affected
# ---------------------------------------------------------------------

echo
echo "Migration plan:"
printf '  from: %s\n' "$OLD_PATH"
printf '  to:   %s\n' "$NEW_PATH"
echo

echo "--- projects at OLD_PATH ---"
sqlite3 "$DB" \
    ".mode column" ".headers on" \
    "SELECT id, worktree, name FROM project WHERE worktree = '$OLD_PATH';"

echo
echo "--- sessions at OLD_PATH ---"
sqlite3 "$DB" \
    ".mode column" ".headers on" \
    "SELECT id, directory, path, title FROM session WHERE directory = '$OLD_PATH';"

echo
echo "--- JSON blob rewrite counts ---"
sqlite3 "$DB" \
    "SELECT
       (SELECT COUNT(*) FROM message
          WHERE data LIKE '%\"cwd\":\"$OLD_PATH/%'
             OR data LIKE '%\"cwd\":\"$OLD_PATH\"%') AS msg_cwd,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"filePath\":\"$OLD_PATH/%'
             OR data LIKE '%\"filePath\":\"$OLD_PATH\"%') AS part_filepath,
       (SELECT COUNT(*) FROM part
          WHERE data LIKE '%\"workdir\":\"$OLD_PATH/%'
             OR data LIKE '%\"workdir\":\"$OLD_PATH\"%') AS part_workdir;"
echo

read -rp "Proceed? [y/N] " ANS
case "$ANS" in
    y|Y|yes|YES) ;;
    *) echo "Aborted. Snapshot kept at $SNAP"; exit 0 ;;
esac

# ---------------------------------------------------------------------
# Apply — all in one transaction
# ---------------------------------------------------------------------

emit_migration_sql | sqlite3 "$DB"

# ---------------------------------------------------------------------
# Verification — all counts should be 0
# ---------------------------------------------------------------------

echo
echo "--- verification (all should be 0) ---"
emit_verification_sql | sqlite3 "$DB" -header -column

echo
echo "Post-migration state:"
sqlite3 "$DB" \
    ".mode column" ".headers on" \
    "SELECT worktree, name FROM project WHERE worktree = '$NEW_PATH';
     SELECT id, directory, title FROM session WHERE directory = '$NEW_PATH';"

echo
echo "Done. Snapshot at:"
echo "  $SNAP"
echo
echo "Restart opencode from the new path:"
echo "  cd $NEW_PATH && opencode"
echo
echo "Rollback (if something looks wrong):"
echo "  cp $SNAP $DB"
