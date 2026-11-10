# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes a labelled contact sheet of frame captures (TGA/PNG) so a run can be checked at a glance."""
import os
import sys

from PIL import Image, ImageDraw

src, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
thumb_w = int(sys.argv[4]) if len(sys.argv) > 4 else 320
first = int(sys.argv[5]) if len(sys.argv) > 5 else 0
count = int(sys.argv[6]) if len(sys.argv) > 6 else 10**9

names = sorted(n for n in os.listdir(src) if n.lower().endswith((".tga", ".png", ".bmp")))
names = names[first:first + count]
thumbs = []
for name in names:
    img = Image.open(os.path.join(src, name)).convert("RGB")
    h = int(img.height * thumb_w / img.width)
    img = img.resize((thumb_w, h))
    ImageDraw.Draw(img).text((4, 4), os.path.splitext(name)[0][-14:], fill=(255, 255, 0))
    thumbs.append(img)
if not thumbs:
    raise SystemExit("no images")
rows = (len(thumbs) + cols - 1) // cols
th = thumbs[0].height
sheet = Image.new("RGB", (cols * thumb_w, rows * th), (20, 20, 20))
for i, t in enumerate(thumbs):
    sheet.paste(t, ((i % cols) * thumb_w, (i // cols) * th))
sheet.save(out)
print(f"{len(thumbs)} frames -> {out} ({sheet.width}x{sheet.height})")
