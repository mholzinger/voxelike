#!/usr/bin/env python3
"""Generate md_src/font.s from font8x8_basic.h (Daniel Hepper, public domain).

usage: tools/gen_font.py path/to/font8x8_basic.h > md_src/font.s

Emits the 96 printable ASCII glyphs, space first, as Mega Drive 4bpp tiles
with every set pixel as nibble F. The 68K loader masks that down to a color.
"""
import re, sys

rows = re.findall(r"\{\s*((?:0x[0-9A-Fa-f]{2},?\s*){8})\}", open(sys.argv[1]).read())
if len(rows) != 128:
    sys.exit(f"expected 128 glyphs, found {len(rows)}")

print("\t.section .text\n")
print("\t.global font_data")
print("font_data:")
for code in range(0x20, 0x80):
    print(f";// {code:02X}")
    for byte in (int(b, 16) for b in re.findall(r"0x[0-9A-Fa-f]{2}", rows[code])):
        # bit 0 is the leftmost pixel in the source font
        nibbles = "".join("F" if byte & (1 << x) else "0" for x in range(8))
        print(f"\t\t.long 0x{nibbles}")
    print()
