# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a copy of a CLI config with some settings changed: variant.py src dst key=value ..."""
import re
import sys

src, dst, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
with open(src, encoding="utf-8") as fh:
    text = fh.read()
for pair in pairs:
    key, value = pair.split("=", 1)
    if not re.search(rf"^{re.escape(key)}=", text, flags=re.M):
        raise SystemExit(f"no setting {key}")
    text = re.sub(rf"^{re.escape(key)}\\default=.*$", f"{key}\\\\default=false", text, flags=re.M)
    text = re.sub(rf"^{re.escape(key)}=.*$", f"{key}={value}", text, flags=re.M)
with open(dst, "w", encoding="utf-8") as fh:
    fh.write(text)
print(f"{dst}: {' '.join(pairs)}")
