#!/usr/bin/env bash
# Run Aerogram with console logs captured to /tmp/aerogram-live.log
# so they can be inspected later. Usage: ./run.sh [same args as ./build/aerogram]
set -u
export QT_FORCE_STDERR_LOGGING=1
{
    echo "=== run $(date -Is) ==="
    ./build/aerogram "$@"
} 2>&1 | tee -a /tmp/aerogram-live.log
