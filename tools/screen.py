#!/usr/bin/env python3
"""Run the ROM headless and print Rogue's 80x24 text screen from emulated RAM.

usage: tools/screen.py [--frames N] [--input keys.csv] [--shot out.png]

The voxel picture shows what the player sees; this shows what Rogue thinks
is on screen, which is the fastest way to tell a game bug from a render bug.
"""
import argparse, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARES = os.environ.get("ARES", os.path.expanduser(
    "~/src/ares-debug/build_macos/headless-ui/Release/ares-headless"))

ap = argparse.ArgumentParser()
ap.add_argument("--frames", type=int, default=300)
ap.add_argument("--input")
ap.add_argument("--shot")
args = ap.parse_args()

addr = None
for line in open(os.path.join(ROOT, "rom/voxelike.lst")):
    parts = line.split()
    if parts[-1] == "_stdscr_cells":
        addr = parts[0]
if addr is None:
    sys.exit("_stdscr_cells not in rom/voxelike.lst; run make first")

with tempfile.TemporaryDirectory() as tmp:
    out = os.path.join(tmp, "scr.bin")
    cmd = [ARES, "--frames", str(args.frames), "--dump", f"sdram:0x{addr}:1920:{out}"]
    if args.input:
        cmd += ["--input", args.input]
    if args.shot:
        cmd += ["--screenshot", f"{args.frames - 1}:{args.shot}"]
    cmd.append(os.path.join(ROOT, "rom/voxelike.32x"))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    data = open(out, "rb").read()

if args.shot:
    subprocess.run(["sips", "-z", "448", "640", args.shot], stdout=subprocess.DEVNULL)

for y in range(24):
    row = data[y * 80:(y + 1) * 80]
    print("".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else "?" for c in row).rstrip())
