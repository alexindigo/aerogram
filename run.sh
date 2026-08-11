#!/usr/bin/env bash
# Build (no-op if nothing changed) then run Aerogram, capturing console
# logs to /tmp/aerogram-live.log. Usage: ./run.sh [args for aerogram]
# Building first matters: QML is compiled INTO the binary — without a
# rebuild you are running stale QML and stale C++.
set -u

if ! cmake --build build; then
    echo "=== BUILD FAILED — not starting the app ===" >&2
    exit 1
fi

export QT_FORCE_STDERR_LOGGING=1
{
    echo "=== run $(date -Is) ==="
    ./build/aerogram "$@"
} 2>&1 | tee -a /tmp/aerogram-live.log
