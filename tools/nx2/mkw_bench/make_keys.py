# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copies a CLI config with player 1 on the keyboard and writes the race key script.

make_keys.py <config.ini> <keys.ini> <script.txt> [race|soak] [quit frame]

Keys are SDL scancodes (EDEN_NX2_KEY_SCRIPT presses them by presented frame): A=4 B=5 X=27 Y=28
L=15 R=21 ZL=29 ZR=6 Plus=40 Minus=45 Up=82 Down=81 Left=80 Right=79. Code 0 at the quit frame
ends the run the way closing the window does.
"""
import re
import sys

KEYS = {"a": 4, "b": 5, "x": 27, "y": 28, "l": 15, "r": 21, "zl": 29, "zr": 6, "plus": 40,
        "minus": 45, "dup": 82, "ddown": 81, "dleft": 80, "dright": 79}

# Presented frames, from the boot: the title screen ("Press L + R") is up by 1500, and the race
# (Mario Bros. Circuit) starts around 4400.
TITLE_FRAME = 1500
ACCELERATE_FRAME = 4250
SOAK_END_FRAME = 90000


def write_config(src, dst):
    with open(src, encoding="utf-8") as fh:
        text = fh.read()
    for button, code in KEYS.items():
        text = re.sub(rf"^player_0_button_{button}=.*$",
                      f'player_0_button_{button}="engine:keyboard,code:{code},toggle:0"', text,
                      flags=re.M)
    with open(dst, "w", encoding="utf-8") as fh:
        fh.write(text)


def tap(events, frame, *buttons, hold=8):
    for b in buttons:
        events.append((frame, KEYS[b]))
    for b in buttons:
        events.append((frame + hold, -KEYS[b]))


def race_script(soak=False, quit_frame=0):
    events = []
    tap(events, TITLE_FRAME, "l", "r", hold=10)
    # Default choices through the menus: Grand Prix, first character and kart, Mushroom Cup, 50cc,
    # then the cup intro.
    for frame in range(TITLE_FRAME + 120, TITLE_FRAME + 2400, 90):
        tap(events, frame, "a")
    if soak:
        # Accelerate, but let go of A for 6 frames every 240: the presses also confirm the
        # results screens, so the Grand Prix goes on to its next races.
        for frame in range(ACCELERATE_FRAME, SOAK_END_FRAME, 240):
            events.append((frame, KEYS["a"]))
            events.append((frame + 234, -KEYS["a"]))
    else:
        events.append((ACCELERATE_FRAME, KEYS["a"]))
    events.sort(key=lambda e: (e[0], e[1] < 0))
    if quit_frame > 0:
        # The emulator runs the script in order: nothing may come after the quit.
        events = [e for e in events if e[0] <= quit_frame] + [(quit_frame, 0)]
    return ",".join(f"f{f}:{c}" for f, c in events)


if __name__ == "__main__":
    write_config(sys.argv[1], sys.argv[2])
    with open(sys.argv[3], "w", encoding="ascii") as fh:
        fh.write(race_script(len(sys.argv) > 4 and sys.argv[4] == "soak",
                             int(sys.argv[5]) if len(sys.argv) > 5 else 0))
