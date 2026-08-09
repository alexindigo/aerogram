#!/usr/bin/env python3
"""Drive the Aerogram IPC socket to verify the IMAP prototype chain.

Usage: dev/test/ipc-drive.py
Assumes aerogram is running with --accounts=dev/test/accounts.json
(multi-account) and the Dovecot test container is up.

Steps are sequential: each fetch moves the active conversation, so we
await its broadcast before issuing the next call.
"""
import json
import os
import socket
import sys
import time

SOCK = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")),
                    "Aerogram/aerogram.ipc")
ATTACH_OUT = "/tmp/aerogram-attachment-test.txt"


def call(sock, method, params=None, req_id=1):
    req = {"jsonrpc": "2.0", "method": method, "params": params or [], "id": req_id}
    sock.sendall((json.dumps(req) + "\n").encode())


def recv_line(sock, buf):
    while b"\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            return None, buf
        buf += chunk
    line, buf = buf.split(b"\n", 1)
    return json.loads(line), buf


def wait_for(sock, buf, method, pred, timeout=30):
    """Read notifications until one matches pred; return (params, buf) or (None, buf)."""
    sock.settimeout(timeout)
    while True:
        try:
            msg, buf = recv_line(sock, buf)
        except (TimeoutError, json.JSONDecodeError):
            return None, buf
        if msg is None:
            return None, buf
        if msg.get("method") == method and pred(msg.get("params", {})):
            return msg["params"], buf


def main():
    if os.path.exists(ATTACH_OUT):
        os.unlink(ATTACH_OUT)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    buf = b""

    call(s, "ping", req_id=1)
    resp, buf = recv_line(s, buf)
    assert resp and resp.get("result") == "pong", resp
    print("ping: OK")

    # First run: createVault with master password + Secret Key phrase.
    call(s, "createVault", ["proto-test-pass",
                            "my cat is often grumpy in the mornings"], req_id=10)
    # Each account's sync lands a conversationsChanged; wait for both.
    for i in range(2):
        p, buf = wait_for(s, buf, "conversationsChanged", lambda p: True)
        assert p is not None, f"sync {i+1} did not complete after createVault"
    print("createVault + sync (2 accounts): OK")

    conv1 = "test@localhost#imap/INBOX"
    conv2 = "test2@localhost#imap/INBOX"

    # Poll: an early fetchMessages moves the active conversation before
    # sync lands (suppressing auto-select), so re-fetch until populated.
    # Sleep between attempts — 0-count broadcasts answer fast, and a
    # cold-start sync needs ~15s; without the sleep, 20 polls burn
    # through in a few seconds and the assertion fires mid-sync.
    populated = None
    for attempt in range(30):
        call(s, "fetchMessages", [conv1], req_id=100 + attempt)
        p, buf = wait_for(s, buf, "messagesChanged",
                          lambda p: p.get("conversationId") == conv1, timeout=15)
        assert p is not None, "no messagesChanged for test INBOX"
        if p.get("count", 0) > 0:
            populated = p
            break
        time.sleep(1.5)
    assert populated is not None, "test INBOX never populated"
    print(f"messagesChanged(test INBOX): OK ({populated.get('count')} messages)")

    call(s, "fetchMessageBody", [conv1, "welcome-001@example.com"], req_id=3)
    p, buf = wait_for(s, buf, "messageBodyReady",
                      lambda p: "welcome to your new mail client" in p.get("body", "").lower())
    assert p is not None, "body not returned/matched"
    print("messageBodyReady: OK (body matched)")

    populated2 = None
    for attempt in range(30):
        call(s, "fetchMessages", [conv2], req_id=200 + attempt)
        p, buf = wait_for(s, buf, "messagesChanged",
                          lambda p: p.get("conversationId") == conv2, timeout=15)
        assert p is not None, "no messagesChanged for test2 INBOX"
        if p.get("count", 0) > 0:
            populated2 = p
            break
        time.sleep(1.5)
    assert populated2 is not None, "test2 INBOX never populated"
    print(f"messagesChanged(test2 INBOX): OK ({populated2.get('count')} messages)")

    call(s, "saveAttachment", ["test2-inbox-002@example.com", 0, ATTACH_OUT], req_id=5)
    p, buf = wait_for(s, buf, "attachmentSaved",
                      lambda p: p.get("ok") and p.get("path") == ATTACH_OUT)
    assert p is not None, "attachment save failed"
    print("attachmentSaved: OK")

    s.close()

    with open(ATTACH_OUT, "rb") as f:
        content = f.read().decode()
    assert "These are the meeting notes." in content, content[:100]
    print("attachment content: OK")

    print("ALL OK")


if __name__ == "__main__":
    main()
