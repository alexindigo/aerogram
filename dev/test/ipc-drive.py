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

SOCK = "/home/user/.cache/Aerogram/aerogram.ipc"
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

    conv1 = "imap:test@localhost/INBOX"
    conv2 = "imap:test2@localhost/INBOX"

    call(s, "fetchMessages", [conv1], req_id=2)
    p, buf = wait_for(s, buf, "messagesChanged",
                      lambda p: p.get("conversationId") == conv1)
    assert p is not None, "no messagesChanged for test INBOX"
    print("messagesChanged(test INBOX): OK")

    call(s, "fetchMessageBody", [conv1, "welcome-001@example.com"], req_id=3)
    p, buf = wait_for(s, buf, "messageBodyReady",
                      lambda p: "welcome to your new mail client" in p.get("body", "").lower())
    assert p is not None, "body not returned/matched"
    print("messageBodyReady: OK (body matched)")

    call(s, "fetchMessages", [conv2], req_id=4)
    p, buf = wait_for(s, buf, "messagesChanged",
                      lambda p: p.get("conversationId") == conv2)
    assert p is not None, "no messagesChanged for test2 INBOX"
    print("messagesChanged(test2 INBOX): OK")

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
