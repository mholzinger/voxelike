#!/usr/bin/env python3
"""Bake the shape of the Sega logo into sh_src/sega_logo.h.

usage: tools/bake_sega.py Sega-logo.png

The header is kept in the repository, so a build needs neither the picture
nor an image library. The shape is one bit a pixel at the size the logo
ends at on screen, with the lines inside its letters left open; sega.c
shows it smaller by leaving pixels out. Smaller than half that the lines
are thinner than a pixel and would come and go, so there is the shape a
second time with them filled in.
"""
import os, sys
from PIL import Image, ImageFilter

W, H = 304, 90       # 76 by 15 characters of 4 by 6 pixels
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
picture = Image.open(sys.argv[1]).convert("RGBA")
# Blue is ink, on white or on nothing. Work at a size that is quick, then
# down by areas.
picture.thumbnail((2000, 2000), Image.BOX)
red, _, _, alpha = picture.split()
ink = Image.composite(red.point(lambda v: 255 if v < 128 else 0), Image.new("L", picture.size, 0),
                      alpha.point(lambda v: 255 if v >= 128 else 0))
box = ink.getbbox()
small = ink.crop(box).resize((W, H), Image.BOX).point(lambda v: 255 if v >= 128 else 0)
# The lines are 20 across here and the gaps between letters twice that.
closed = ink.filter(ImageFilter.MaxFilter(25)).filter(ImageFilter.MinFilter(25))
solid = closed.crop(box).resize((W, H), Image.BOX).point(lambda v: 255 if v >= 128 else 0)
# Where a letter ends and the next begins: the gaps between them, and for
# the G and the A, which touch, the G's straight right side.
wide = box[2] - box[0]
splits = [round(x * W / wide) for x in (480, 962, 1418)]

path = os.path.join(ROOT, "sh_src/sega_logo.h")
with open(path, "w") as f:
    f.write("/* Made by tools/bake_sega.py: the Sega logo's shape, a bit a pixel, the high bit first. */\n")
    f.write(f"#define SEGA_W {W}\n#define SEGA_H {H}\n")
    f.write(f"static const uint16_t sega_split[3] = {{ {', '.join(map(str, splits))} }};\n")
    f.write("static const uint8_t sega_solid[SEGA_H][SEGA_W / 8] = {\n")
    px = solid.load()
    for y in range(H):
        row = [sum((px[x + b, y] > 0) << (7 - b) for b in range(8)) for x in range(0, W, 8)]
        f.write("    { " + ", ".join(f"0x{v:02X}" for v in row) + " },\n")
    f.write("};\n")
    # Each row again as its stretches of ink, a stretch ending where a
    # letter does: finding them a bit at a time was most of a frame.
    f.write("/* A row's stretches of ink: where one starts, and where it ends with its letter in the top four bits. */\n")
    f.write("typedef struct { uint16_t from, to; } sega_run_t;\n")
    for name, image in (("sega_shape", small), ("sega_filled", solid)):
        px = image.load()
        runs, starts = [], []
        for y in range(H):
            starts.append(len(runs))
            x = 0
            while x < W:
                if not px[x, y]:
                    x += 1
                    continue
                letter = sum(x >= s for s in splits)
                end = x
                while end < W and px[end, y] and sum(end >= s for s in splits) == letter: end += 1
                runs.append((x, end | letter << 12))
                x = end
        starts.append(len(runs))
        f.write(f"static const uint16_t {name}_row[SEGA_H + 1] = {{ {', '.join(map(str, starts))} }};\n")
        f.write(f"static const sega_run_t {name}_run[{len(runs)}] = {{\n")
        for i in range(0, len(runs), 8):
            f.write("    " + " ".join(f"{{{a}, 0x{b:04X}}}," for a, b in runs[i:i + 8]) + "\n")
        f.write("};\n")
print(path)
px = small.load()
for y in range(0, H, 2):
    print("".join("#" if px[x, y] else " " for x in range(0, W, 2)))
