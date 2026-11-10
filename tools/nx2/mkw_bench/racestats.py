# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame statistics from a run's log, from the nx2:vi2 600-present windows: the race after its
countdown, and the boot up to the title screen (the loading screen), with what held its frames."""
import bisect
import re
import sys

# Newer builds add the presents that asked for 2+ vblanks, the word 12 changes and the game speed.
PAT = re.compile(r"last 600 presents: frame (\d+) us \(max (\d+), (\d+) over 100 ms\), blocked on the "
                 r"GPU fence (\d+) us(?:.*?; (\d+) asked for 2\+ vblanks, word 12 changed (\d+) times, "
                 r"game speed (\d+)%)?")
PIPELINES = re.compile(r"Runtime pipelines in the last 10 s: (\d+) built, ([\d.]+) ms on the GPU thread"
                       r"(?:; (\d+) from the disk cache \(([\d.]+) ms\), (\d+) task/mesh, (\d+) draws "
                       r"skipped while building)?(?:; over ([\d.]+) s)?")
READS = re.compile(r"CPU read of GPU-written memory .*\((\d+) so far\)")
TIME = re.compile(r"^\[\s*([\d.]+)\]")
TOTAL = re.compile(r"Total Pipeline Count: (\d+)")
LAZY = re.compile(r"(\d+) cached graphics pipelines are built when first needed")
USED_LIST = re.compile(r"Used pipeline list: (.*)$")
PRESENT = re.compile(r"nx2:vi2 229 present #(\d+) token")
WAITS = re.compile(r"Draws waiting for a pipeline build in the last ([\d.]+) s: (\d+) graphics / "
                   r"(\d+) compute, ([\d.]+) ms total")
TRAPS = re.compile(r"CPU reads of GPU-written memory in the last ([\d.]+) s: (\d+) waits, ([\d.]+) ms "
                   r"total \(([\d.]+) ms in the flush itself\)")
BLOCKED = re.compile(r"Guest CPU writes blocked on the GPU thread's cache locks in the last ([\d.]+) "
                     r"s: (\d+), ([\d.]+) ms total")
HELD = re.compile(r"nx2:vi2 present #\d+ held (\d+) ms")
ZEROS = re.compile(r"nx2:gpu (\d+) MiB of zeros over unwritten (sparse|GPU-only) blocks left out")
BACKED = re.compile(r"nx2:gpu \d+ sparse blocks backed \((\d+) MiB\)")
# The frame-rate mode the loader applied (Mario Kart World: ... on, off or not applied).
MKW_MODE = re.compile(r"Mario Kart World: ((?:dynamic frame rate|30 fps lock|frame-rate mode) "
                      r"(?:on|off|not applied))")
LONG_OP = "Long GPU-thread operation"
FRAME_GEN = "Frame generation: building"

BOOT_WINDOWS = 2  # presents 1-1200: the title screen is up by 1500 (make_keys.py)
TITLE_FRAME = 1500  # make_keys.py
LOAD_US = 3000000  # the race load holds one frame for more than 3 s
AFTER_LOAD = 3  # the course intro and the countdown fill the next two windows
FALLBACK = 9  # with the load in window 6, windows 9+ start at present 4801, after the countdown
TITLE = re.compile(rf"Key script: press key \d+ at [\d.]+ s \(frame {TITLE_FRAME}\)")

# Per-window sums. A summary line goes to the window holding the middle of the time it covers: the
# first event 10 s or more after the last line logs it, so it can cover much more, and it says how
# long (the *_s sums). Older builds' runtime pipelines lines do not, and count as 10 s.
SUMS = ("built", "built_ms", "disk", "disk_ms", "mesh", "skipped", "waits", "wait_ms", "traps",
        "trap_ms", "flush_ms", "blocked", "blocked_ms", "held", "held_ms", "long_ops", "zeros",
        "backed", "built_s", "waits_s", "traps_s", "blocked_s")
SUMMARY_SHIFT = 5.0


class Run:
    def __init__(self):
        self.rows = []     # (frame, max, over 100 ms, fence, built, built_ms, reads) per window
        self.timing = []   # (asked for 2+ vblanks, word 12 changes, game speed) per window, or None
        self.windows = []  # the SUMS of each window
        self.seen = set()  # the SUMS any line of this run reports (older builds lack some)
        self.total = self.lazy = self.used_list = None
        self.frame_gen = False
        self.mkw_mode = None
        self.presents = {}  # present number -> log time
        self.title = None


def parse(path):
    """The windows in log order, each with the runtime pipelines and CPU reads logged up to it
    (the boot line), and the SUMS of each."""
    run = Run()
    built, built_ms, reads = 0, 0.0, 0
    now = 0.0
    ends = []    # log time of each window's line
    events = []  # (time the amounts belong to, {sum: amount})
    totals = []  # (time, {"zeros"/"backed": running total so far})
    zeros = {"sparse": 0, "GPU-only": 0}

    def add(shift, **amounts):
        events.append((now - shift, amounts))
        run.seen.update(amounts)

    for line in open(path, encoding="utf-8", errors="replace"):
        t = TIME.search(line)
        if t:
            now = float(t.group(1))
        m = PAT.search(line)
        if m:
            run.rows.append(tuple(map(int, m.groups()[:4])) + (built, built_ms, reads))
            run.timing.append(tuple(map(int, m.groups()[4:])) if m.group(7) is not None else None)
            ends.append(now)
            continue
        m = PIPELINES.search(line)
        if m:
            built += int(m.group(1))
            built_ms += float(m.group(2))
            amounts = {"built": int(m.group(1)), "built_ms": float(m.group(2))}
            shift = SUMMARY_SHIFT
            if m.group(7) is not None:
                amounts["built_s"] = float(m.group(7))
                shift = amounts["built_s"] / 2
            add(shift, **amounts)
            if m.group(3) is not None:
                add(shift, disk=int(m.group(3)), disk_ms=float(m.group(4)),
                    mesh=int(m.group(5)), skipped=int(m.group(6)))
            continue
        m = READS.search(line)
        if m:
            reads = int(m.group(1))
            continue
        m = TRAPS.search(line)
        if m:
            span = float(m.group(1))
            add(span / 2, traps=int(m.group(2)), trap_ms=float(m.group(3)),
                flush_ms=float(m.group(4)), traps_s=span)
            continue
        m = WAITS.search(line)
        if m:
            span = float(m.group(1))
            add(span / 2, waits=int(m.group(2)) + int(m.group(3)), wait_ms=float(m.group(4)),
                waits_s=span)
            continue
        m = BLOCKED.search(line)
        if m:
            span = float(m.group(1))
            add(span / 2, blocked=int(m.group(2)), blocked_ms=float(m.group(3)), blocked_s=span)
            continue
        m = HELD.search(line)
        if m:
            add(0.0, held=1, held_ms=int(m.group(1)))
            continue
        if LONG_OP in line:
            add(0.0, long_ops=1)
            continue
        m = ZEROS.search(line)
        if m:
            zeros[m.group(2)] = int(m.group(1))
            totals.append((now, "zeros", sum(zeros.values())))
            run.seen.add("zeros")
            continue
        m = BACKED.search(line)
        if m:
            totals.append((now, "backed", int(m.group(1))))
            run.seen.add("backed")
            continue
        m = PRESENT.search(line)
        if m:
            run.presents.setdefault(int(m.group(1)), now)
            continue
        if run.title is None and TITLE.search(line):
            run.title = now
            continue
        m = MKW_MODE.search(line)
        if m:
            run.mkw_mode = m.group(1)
            continue
        m = TOTAL.search(line)
        if m:
            run.total = int(m.group(1))
            continue
        m = LAZY.search(line)
        if m:
            run.lazy = int(m.group(1))
            continue
        m = USED_LIST.search(line)
        if m:
            run.used_list = m.group(1).strip()
            continue
        if FRAME_GEN in line:
            run.frame_gen = True
    run.windows = [dict.fromkeys(SUMS, 0) for _ in ends]
    for when, amounts in events:
        index = bisect.bisect_left(ends, when)
        if index < len(ends):  # not past the last whole window
            for key, amount in amounts.items():
                run.windows[index][key] += amount
    # Running totals (logged every 256 MiB): each window gets the growth up to its end.
    last = {"zeros": 0, "backed": 0}
    position = 0
    for index, end in enumerate(ends):
        before = dict(last)
        while position < len(totals) and totals[position][0] <= end:
            last[totals[position][1]] = totals[position][2]
            position += 1
        for key in last:
            run.windows[index][key] = last[key] - before[key]
    return run


def race_start(rows):
    """The first in-race window (1-based) and the race load's window, or window 9 and None. A menu
    can also hold a frame for over 3 s, so the load is the longest such frame before window 9."""
    loads = [i for i in range(BOOT_WINDOWS, min(len(rows), FALLBACK - 1)) if rows[i][1] > LOAD_US]
    if not loads:
        return FALLBACK, None
    i = max(loads, key=lambda i: rows[i][1])
    return i + 1 + AFTER_LOAD, i + 1


def stats(rows):
    frame = sum(r[0] for r in rows) / len(rows)
    return {
        "windows": len(rows),
        "frame_ms": frame / 1000,
        "fps": 1e6 / frame,
        "max_ms": max(r[1] for r in rows) / 1000,
        "over100": sum(r[2] for r in rows),
        "fence_ms": sum(r[3] for r in rows) / len(rows) / 1000,
        "per_window": [round(r[0] / 1000, 1) for r in rows],
    }


def game_speed(run, first, last=None):
    """Game speed in windows first..last (1-based, inclusive; to the end without last), each window
    weighted by its time, the presents that asked for 2+ vblanks and the word 12 changes there; "-"
    for a build that does not log it."""
    rows, timing = run.rows[first - 1:last], run.timing[first - 1:last]
    if not rows or any(t is None for t in timing):
        return "-"
    speed = sum(r[0] * t[2] for r, t in zip(rows, timing)) / sum(r[0] for r in rows)
    return (f"{speed:.0f}% ({sum(t[0] for t in timing)} asked for 2+ vblanks, "
            f"{sum(t[1] for t in timing)} word 12 changes)")


def since_first_present(run, t):
    first = run.presents.get(1)
    return f"{t - first:.1f} s" if first is not None and t is not None else "-"


def span_line(run, label, first, last):
    """Frame times and what held them in windows first..last (1-based, inclusive)."""
    rows = run.rows[first - 1:last]
    if not rows:
        return None
    s = stats(rows)
    sums = {key: sum(w[key] for w in run.windows[first - 1:last]) for key in SUMS}

    def field(text, *keys):
        return text.format(*(sums[k] for k in keys)) if all(k in run.seen for k in keys) else "-"

    def per_10s(ms_key, span_key):
        """The ms per 10 s of the time the window's summary lines cover, when they say it."""
        if span_key not in run.seen or not sums[span_key]:
            return ""
        return f"; {sums[ms_key] * 10 / sums[span_key]:.0f} ms/10 s over {sums[span_key]:.0f} s"

    build_waits = field("{} ({:.0f} ms" + per_10s("wait_ms", "waits_s") + ")", "waits", "wait_ms")
    trap_waits = field("{} ({:.0f} ms, {:.0f} ms flushing" + per_10s("trap_ms", "traps_s") + ")",
                       "traps", "trap_ms", "flush_ms")
    blocked = field("{} ({:.0f} ms" + per_10s("blocked_ms", "blocked_s") + ")", "blocked", "blocked_ms")
    return (f"{label:<24} {s['frame_ms']:5.1f} ms  max {s['max_ms']:6.0f} ms  {s['over100']:3d} over 100 ms"
            f" | pipelines {sums['built']} ({sums['built_ms']:.0f} ms{per_10s('built_ms', 'built_s')}), "
            f"{field('{} from disk ({:.0f} ms), {} mesh, {} draws skipped', 'disk', 'disk_ms', 'mesh', 'skipped')}"
            f" | build waits {build_waits} | trap waits {trap_waits} | blocked writes {blocked}"
            f" | held {field('{} ({} ms)', 'held', 'held_ms')}"
            f" | long ops {field('{}', 'long_ops')}"
            f" | zeros left out {field('{} MiB', 'zeros')}, sparse blocks backed {field('{} MiB', 'backed')}"
            f" | game speed {game_speed(run, first, last)}")


if __name__ == "__main__":
    for path in sys.argv[1:]:
        name = path.replace("\\", "/").split("/")[-1]
        run = parse(path)
        rows = run.rows
        first, load = race_start(rows)
        race = rows[first - 1:]
        if race:
            s = stats(race)
            start = f"load in window {load}" if load else "no load over 3 s"
            print(f"{name:<24} {s['windows']:2d} windows  {s['frame_ms']:5.1f} ms  {s['fps']:4.1f} fps  "
                  f"max {s['max_ms']:6.0f} ms  {s['over100']:3d} over 100 ms  fence {s['fence_ms']:.2f} ms  "
                  f"from window {first} ({start})  {s['per_window']}")
            print(f"{'  game speed':<24} {game_speed(run, first)}")
        else:
            print(f"{path}: no race windows")
        boot = rows[:BOOT_WINDOWS]
        if boot:
            s = stats(boot)
            built, built_ms, reads = boot[-1][4:]
            print(f"{'  boot':<24} {s['windows']:2d} windows  {s['frame_ms']:5.1f} ms  {s['fps']:4.1f} fps  "
                  f"max {s['max_ms']:6.0f} ms  {s['over100']:3d} over 100 ms  {built} runtime pipelines "
                  f"({built_ms:.0f} ms on the GPU thread), {reads} CPU reads of GPU-written memory")
        total = run.total if run.total is not None else "-"
        lazy = run.lazy if run.lazy is not None else "-"
        print(f"{'  preload':<24} {total} pipelines built at boot, {lazy} cached graphics pipelines "
              f"when first needed, used list {run.used_list or '-'}, "
              f"frame generation {'on' if run.frame_gen else 'off'}, MKW {run.mkw_mode or '-'}")
        title_log = f" (log {run.title:.1f} s)" if run.title is not None else ""
        print(f"{'  progress':<24} from present #1: #600 at {since_first_present(run, run.presents.get(600))}, "
              f"#1200 at {since_first_present(run, run.presents.get(1200))}, title (frame {TITLE_FRAME}) "
              f"at {since_first_present(run, run.title)}{title_log}")
        menus_end = (load if load else FALLBACK - AFTER_LOAD) - 1
        for line in (span_line(run, "  window 1", 1, 1), span_line(run, "  window 2", 2, 2),
                     span_line(run, f"  menus (3-{menus_end})", BOOT_WINDOWS + 1, menus_end)):
            if line:
                print(line)
