# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarizes EDEN_NX2_SAMPLE output: per thread, how busy it is and its top functions."""
import re
import sys

WAITS = re.compile(r"(Wait|Sleep|Delay|Alert|Yield|SwitchToThread|SignalAndWait|KeWait|NtRemoveIoCompletion)",
                   re.I)


def parse(path):
    threads, cur, section = {}, None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r'=== thread "([^"]+)": (\d+) samples', line)
        if m:
            cur = {"total": int(m.group(2)), "self": [], "incl": []}
            threads[m.group(1)] = cur
            continue
        if line.startswith("-- self (where"):
            section = "self"
            continue
        if line.startswith("-- "):
            section = "incl" if line.startswith("-- inclusive") else "other"
            continue
        m = re.match(r"\s*(\d+)\s+([\d.]+)%\s+(.*)", line)
        if m and cur is not None and section in ("self", "incl"):
            cur[section].append((int(m.group(1)), m.group(3).strip()))
    return threads


def main(path, top, only):
    for name, t in parse(path).items():
        if only and not any(name.startswith(o) for o in only):
            continue
        waiting = sum(c for c, f in t["self"] if WAITS.search(f))
        busy = t["total"] - waiting
        print(f"== {name}: {t['total']} samples, busy {100 * busy / max(t['total'], 1):.1f}%")
        shown = 0
        for c, f in t["self"]:
            if WAITS.search(f):
                continue
            print(f"   {100 * c / max(busy, 1):5.1f}% of busy  {f[:110]}")
            shown += 1
            if shown >= top:
                break


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 12, sys.argv[3:])
