#!/usr/bin/env python3
"""Let the test bot play and report what happened.

usage: make BOT=1 && tools/botrun.py [--frames N] [--every K] [--start F]

Needs a ROM built with BOT=1. Presses Start at frame F (which seeds the
dungeon), saves a screenshot every K frames to shots/bot/ plus contact
sheets, and prints the dungeon level over time, frame time statistics and
Rogue's final text screen.
"""
import argparse, csv, glob, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARES = os.environ.get("ARES", os.path.expanduser(
    "~/src/ares-debug/build_macos/headless-ui/Release/ares-headless"))

ap = argparse.ArgumentParser()
ap.add_argument("--frames", type=int, default=18000)
ap.add_argument("--every", type=int, default=300)
ap.add_argument("--start", type=int, default=150)
args = ap.parse_args()

if not os.path.exists(os.path.join(ROOT, "rom/.bot-on")):
    sys.exit("the ROM was not built with BOT=1")

sym = {}
for line in open(os.path.join(ROOT, "rom/voxelike.lst")):
    parts = line.split()
    sym[parts[-1]] = parts[0]

shots = os.path.join(ROOT, "shots/bot")
os.makedirs(shots, exist_ok=True)
for f in glob.glob(os.path.join(shots, "*.png")):
    os.remove(f)

with tempfile.TemporaryDirectory() as tmp:
    keys = os.path.join(tmp, "keys.csv")
    open(keys, "w").write(f"{args.start},start,1\n{args.start + 8},start,0\n")
    log = os.path.join(tmp, "frames.csv")
    scr = os.path.join(tmp, "scr.bin")
    ticks_at = sym["_game_render_ticks"]
    cmd = [ARES, "--frames", str(args.frames), "--input", keys,
           "--gameframe", f"sdram:0x{ticks_at}", "--gameframe-log", log,
           "--gameframe-watch", f"sdram:0x{ticks_at}:2",
           "--gameframe-watch", f"sdram:0x{sym['_level']}:4",
           "--dump", f"sdram:0x{sym['_stdscr_cells']}:1920:{scr}"]
    frames = list(range(args.start + 120, args.frames, args.every))
    for f in frames:
        cmd += ["--screenshot", f"{f}:{shots}/f{f:06d}.png"]
    cmd.append(os.path.join(ROOT, "rom/voxelike.32x"))
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rows = list(csv.reader(open(log)))[1:]
    data = open(scr, "rb").read()

level, last = [], None
ticks = []
for r in rows[args.start + 90:]:
    if len(r) < 4 or not r[2]:
        continue
    tick = int(r[2], 16)
    if tick:
        ticks.append(tick)
    lv = int(r[3], 16)
    if lv != last:
        level.append((int(r[0]), lv))
        last = lv
print("level by frame:", " ".join(f"{f}:L{lv}" for f, lv in level))
ts = sorted(ticks)
if ts:
    print(f"render samples {len(ts)}  median {ts[len(ts) // 2]}  p95 {ts[len(ts) * 95 // 100]}  "
          f"max {ts[-1]}  over 2500 ticks: {sum(t > 2500 for t in ts)}")
else:
    print("no render samples")
slow = [(int(r[0]), int(r[2], 16)) for r in rows[args.start + 90:]
        if len(r) > 2 and r[2] and int(r[2], 16) > 2500]
if slow:
    print("slow frames:", " ".join(f"{f}:{t}" for f, t in slow[:20]))

for y in range(24):
    row = data[y * 80:(y + 1) * 80]
    line = "".join(chr(c & 0x7F) if 32 <= (c & 0x7F) < 127 else "?" for c in row).rstrip()
    if line:
        print(line)

try:
    from PIL import Image
except ImportError:
    sys.exit(0)
PER = 20
for n in range(0, len(frames), PER):
    part = frames[n:n + PER]
    sheet = Image.new("RGB", (5 * 320, 4 * 224))
    for i, f in enumerate(part):
        im = Image.open(f"{shots}/f{f:06d}.png").convert("RGB").resize((320, 224), Image.NEAREST)
        sheet.paste(im, ((i % 5) * 320, (i // 5) * 224))
    sheet.save(f"{shots}/sheet{n // PER:02d}.png")
print(f"screenshots and contact sheets in {shots}")
