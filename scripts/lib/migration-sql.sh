#!/usr/bin/env bash
#
# lib/migration-sql.sh
#
# Emits the SQL transaction that rewrites opencode DB rows from
# OLD_PATH to NEW_PATH. Sourced by both:
#   - migrate-opencode-session.sh (quit-first workflow)
#   - recover-session-live.sh     (WAL live-write workflow)
#
# Callers must define OLD_PATH, NEW_PATH, OLD_REL, NEW_REL, NEW_NAME
# before calling emit_migration_sql.
#
# Rewritten columns:
#   project.worktree, project.name  where worktree = OLD_PATH
#   session.directory               where directory = OLD_PATH
#   session.path                    where path = OLD_REL
#   message.data:  "cwd":"OLD_PATH{/,"}      -> NEW_PATH
#   part.data:     "filePath":"OLD_PATH{/,"} -> NEW_PATH
#   part.data:     "workdir":"OLD_PATH{/,"}  -> NEW_PATH
#
# Deliberately NOT rewritten (history principle):
#   Tool call outputs, reasoning text, assistant text bodies.

emit_migration_sql() {
    cat <<SQL
BEGIN;

-- 1. project row
UPDATE project
   SET worktree = '$NEW_PATH', name = '$NEW_NAME'
 WHERE worktree = '$OLD_PATH';

-- 2. session.directory (absolute)
UPDATE session
   SET directory = '$NEW_PATH'
 WHERE directory = '$OLD_PATH';

-- 3. session.path (absolute minus leading /; may be no-op)
UPDATE session
   SET path = '$NEW_REL'
 WHERE path = '$OLD_REL';

-- 4. message.data "cwd" (both boundary forms)
UPDATE message
   SET data = REPLACE(data, '"cwd":"$OLD_PATH/', '"cwd":"$NEW_PATH/')
 WHERE data LIKE '%"cwd":"$OLD_PATH/%';
UPDATE message
   SET data = REPLACE(data, '"cwd":"$OLD_PATH"', '"cwd":"$NEW_PATH"')
 WHERE data LIKE '%"cwd":"$OLD_PATH"%';

-- 5. part.data "filePath" (Read/Write/Edit tool inputs)
UPDATE part
   SET data = REPLACE(data, '"filePath":"$OLD_PATH/', '"filePath":"$NEW_PATH/')
 WHERE data LIKE '%"filePath":"$OLD_PATH/%';
UPDATE part
   SET data = REPLACE(data, '"filePath":"$OLD_PATH"', '"filePath":"$NEW_PATH"')
 WHERE data LIKE '%"filePath":"$OLD_PATH"%';

-- 6. part.data "workdir" (Bash tool inputs)
UPDATE part
   SET data = REPLACE(data, '"workdir":"$OLD_PATH/', '"workdir":"$NEW_PATH/')
 WHERE data LIKE '%"workdir":"$OLD_PATH/%';
UPDATE part
   SET data = REPLACE(data, '"workdir":"$OLD_PATH"', '"workdir":"$NEW_PATH"')
 WHERE data LIKE '%"workdir":"$OLD_PATH"%';

COMMIT;
SQL
}

emit_verification_sql() {
    # NOTE: uses GLOB (case-sensitive) instead of LIKE (case-insensitive
    # for ASCII in SQLite). This matters because the JSON keys we
    # rewrite are camelCase ("filePath", "workdir", "cwd") — but tool
    # OUTPUTS may use lowercase ("filepath"), which the history
    # principle deliberately leaves alone. A LIKE-based check would
    # false-alarm on those preserved output records.
    cat <<SQL
SELECT
   (SELECT COUNT(*) FROM project  WHERE worktree = '$OLD_PATH')       AS project_stale,
   (SELECT COUNT(*) FROM session  WHERE directory = '$OLD_PATH')      AS session_stale,
   (SELECT COUNT(*) FROM session  WHERE path = '$OLD_REL')            AS session_path_stale,
   (SELECT COUNT(*) FROM message  WHERE data GLOB '*"cwd":"$OLD_PATH/*'
                                     OR data GLOB '*"cwd":"$OLD_PATH"*')      AS msg_cwd_stale,
   (SELECT COUNT(*) FROM part     WHERE data GLOB '*"filePath":"$OLD_PATH/*'
                                     OR data GLOB '*"filePath":"$OLD_PATH"*') AS part_filepath_stale,
   (SELECT COUNT(*) FROM part     WHERE data GLOB '*"workdir":"$OLD_PATH/*'
                                     OR data GLOB '*"workdir":"$OLD_PATH"*')  AS part_workdir_stale;
SQL
}
