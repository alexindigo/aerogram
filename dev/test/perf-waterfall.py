#!/usr/bin/env python3
"""Harvest PERF markers from an aerogram log into per-message waterfalls.

Usage:
    perf-waterfall.py <logfile> [msg-substring ...]

Groups every PERF line by its msg= id, sorts by abs epoch ms, and prints
each message's timeline as +delta from its ui-request/open marker.
Lines without abs (dur-only stages) are placed by file order and given
the abs of the previous marker for the same message.
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(r"PERF (\S+) (.*)")
KV = re.compile(r"(\w+)=([^\s\]]+)")

def main():
    path = sys.argv[1]
    filters = sys.argv[2:]
    events = defaultdict(list)  # msg -> [(abs_ms_or_None, order, stage, rest)]
    order = 0
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            stage, rest = m.group(1), m.group(2)
            kv = dict(KV.findall(rest))
            msg = kv.get("msg") or kv.get("conv") or "-"
            if filters and not any(flt in msg for flt in filters):
                continue
            abs_ms = kv.get("abs")
            events[msg].append((int(abs_ms) if abs_ms else None, order, stage, rest))
            order += 1

    for msg, evs in sorted(events.items(), key=lambda kv: min(o for _, o, _, _ in kv[1])):
        # fill missing abs from previous marker of same message
        last_abs = None
        rows = []
        for abs_ms, _, stage, rest in sorted(evs, key=lambda e: e[1]):
            if abs_ms is not None:
                last_abs = abs_ms
            rows.append((last_abs, stage, rest))
        base = next((a for a, _, _ in rows if a is not None), None)
        short = msg if len(msg) <= 28 else msg[:25] + "..."
        print(f"\n=== {short} ===")
        for abs_ms, stage, rest in rows:
            delta = f"+{abs_ms - base:>6}ms" if (abs_ms is not None and base is not None) else "   ?   "
            # keep the rest short: drop msg= and abs= tokens
            detail = " ".join(t for t in rest.split()
                              if not t.startswith("msg=") and not t.startswith("abs="))
            print(f"  {delta}  {stage:<14} {detail}")

if __name__ == "__main__":
    main()
