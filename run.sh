#!/usr/bin/env bash
# Build (no-op if nothing changed) then run Aerogram, capturing console
# logs to /tmp/aerogram-live.log. Usage: ./run.sh [args for aerogram]
# Building first matters: QML is compiled INTO the binary — without a
# rebuild you are running stale QML and stale C++.
set -u

# Anchor to the script's own directory: `./run.sh` from any other cwd
# would otherwise build+run whatever ./build is THERE (e.g. the master
# worktree) — this exact mix-up already produced a bogus test session.
cd "$(dirname "$(readlink -f "$0")")"

if ! cmake --build build; then
    echo "=== BUILD FAILED — not starting the app ===" >&2
    exit 1
fi

export QT_FORCE_STDERR_LOGGING=1
{
    echo "=== run $(date -Is) ==="
    echo "=== worktree: $(basename "$(git rev-parse --show-toplevel 2>/dev/null || pwd)") ==="
    ./build/aerogram "$@"
} 2>&1 | tee -a /tmp/aerogram-live.log
