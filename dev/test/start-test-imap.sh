#!/usr/bin/env bash
# Start a local Dovecot IMAP test server for the ImapBackend prototype.
#
#   imap://localhost:1143  user=test  pass=test
#
# Folders: INBOX (9 seen + 3 unseen), Dev (3 seen).
set -euo pipefail
cd "$(dirname "$0")"

docker build -q -t aerogram-dovecot .
docker rm -f aerogram-dovecot >/dev/null 2>&1 || true
docker run -d --name aerogram-dovecot -p 1143:143 aerogram-dovecot >/dev/null

# Wait for the listener to come up.
for _ in $(seq 1 30); do
    if curl -s --connect-timeout 2 -u test:test \
            "imap://localhost:1143/INBOX" -X "SEARCH ALL" 2>/dev/null | grep -q "SEARCH"; then
        echo "IMAP test server up: imap://localhost:1143 user=test pass=test"
        exit 0
    fi
    sleep 1
done

echo "ERROR: dovecot did not come up" >&2
docker logs aerogram-dovecot >&2 || true
exit 1
