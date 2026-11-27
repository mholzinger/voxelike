#!/usr/bin/env python3
"""Bake the level in an ares save state into port/snap_data.h for make SNAP=1.

usage: tools/snap.py state.bs1 [rom.32x [rom.lst]]

A save state only loads in the ROM that made it, so it cannot time a change
to the code. This takes what the renderer was looking at out of the state:
Rogue's screen as the hero had uncovered it and how high each cell's floor
stood. make SNAP=1 builds a ROM that shows that level whatever is played,
so the same view can be timed before and after. The ROM and its .lst must
be the ones the state was saved from (rom/voxelike.* unless given).
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARES = os.environ.get("ARES", os.path.expanduser("~/src/ares-debug/build_macos/headless-ui/Release/ares-headless"))
state = sys.argv[1]
rom = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "rom/voxelike.32x")
lst = sys.argv[3] if len(sys.argv) > 3 else os.path.splitext(rom)[0] + ".lst"

sym = {}
for line in open(lst):
    parts = line.split()
    sym[parts[-1]] = parts[0]

with tempfile.TemporaryDirectory() as tmp:
    out = {}
    cmd = [ARES, "--frames", "2", "--load-state", state]
    for name in ("_stdscr_cells", "_vox_lift"):
        out[name] = os.path.join(tmp, name)
        cmd += ["--dump", f"sdram:0x{sym[name]}:1920:{out[name]}"]
    subprocess.run(cmd + [rom], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    screen = open(out["_stdscr_cells"], "rb").read()
    lift = open(out["_vox_lift"], "rb").read()

def table(name, data):
    rows = [", ".join(str(b) for b in data[i:i + 80]) for i in range(0, len(data), 80)]
    return f"static const unsigned char {name}[24 * 80] = {{\n    " + ",\n    ".join(rows) + "\n};\n"

path = os.path.join(ROOT, "port/snap_data.h")
with open(path, "w") as f:
    f.write(f"/* Made by tools/snap.py from {os.path.basename(state)}: a level as the hero had found it. */\n")
    # The hero is one station, where the state had him: in a passage.
    at = next(i for i, c in enumerate(screen) if c & 0x7F == ord("@"))
    f.write(table("snap_screen", bytes(ord("#") if i == at else c for i, c in enumerate(screen))))
    f.write(table("snap_lift", lift))
    f.write("/* x, y, whether the room is lit, and its walls' x0 y0 x1 y1. */\n")
    f.write("#define SNAP_STATIONS 1\n")
    f.write(f"static const unsigned char snap_stations[SNAP_STATIONS][7] = {{ {{ {at % 80}, {at // 80}, 0, 0, 0, 0, 0 }} }};\n")
    f.write('static const char *const snap_names[SNAP_STATIONS] = { "THE LEVEL" };\n')
print(path)
for y in range(24):
    print("".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else " " for c in screen[y * 80:y * 80 + 80]).rstrip())
