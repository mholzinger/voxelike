#!/usr/bin/env python3
"""Run the ROM headless and report per-frame render time.

usage: tools/frametimes.py [--frames N] [--input keys.csv] [--from F]

Reads game_render_ticks (FRT ticks, 5.56 us each) once per emulated frame.
A 60 fps frame has about 3000 ticks in total. These are emulator numbers:
use them to compare builds. A zero tick sample means the renderer reused
its visible frame. Check a MiSTer to measure hardware frame rates.
"""
import argparse, csv, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARES = os.environ.get("ARES", os.path.expanduser(
    "~/src/ares-debug/build_macos/headless-ui/Release/ares-headless"))

ap = argparse.ArgumentParser()
ap.add_argument("--frames", type=int, default=600)
ap.add_argument("--input")
ap.add_argument("--from", dest="start", type=int, default=240,
                help="first frame to include (skip boot and the title)")
args = ap.parse_args()

addr = None
for line in open(os.path.join(ROOT, "rom/voxelike.lst")):
    parts = line.split()
    if parts[-1] == "_game_render_ticks":
        addr = parts[0]
if addr is None:
    sys.exit("_game_render_ticks not in rom/voxelike.lst; run make first")

with tempfile.TemporaryDirectory() as tmp:
    log = os.path.join(tmp, "frames.csv")
    cmd = [ARES, "--frames", str(args.frames), "--gameframe", f"sdram:0x{addr}",
           "--gameframe-log", log, "--gameframe-watch", f"sdram:0x{addr}:2"]
    if args.input:
        cmd += ["--input", args.input]
    cmd.append(os.path.join(ROOT, "rom/voxelike.32x"))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = list(csv.reader(open(log)))[1:]

samples = [int(r[2], 16) for r in rows[args.start:] if len(r) > 2 and r[2]]
if not samples:
    sys.exit("no frames logged")
ticks = [value for value in samples if value]
if not ticks:
    print(f"all {len(samples)} samples reused the visible frame")
    sys.exit(0)
ticks_sorted = sorted(ticks)
over = sum(1 for t in ticks if t > 2500)
print(f"render samples {len(ticks)}  reused {len(samples) - len(ticks)}  median {ticks_sorted[len(ticks) // 2]}  "
      f"p95 {ticks_sorted[len(ticks) * 95 // 100]}  max {ticks_sorted[-1]}  "
      f"over 2500 ticks: {over}")
