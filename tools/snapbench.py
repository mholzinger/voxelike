#!/usr/bin/env python3
"""Run the self-driving SNAP ROM and print its table.

usage: make SNAP=2 POLY=2 && tools/snapbench.py [--shots DIR] [--label NAME]

The ROM needs no pad: it stands the hero at each station of the baked
level, turns the camera once round him in 128 frames and times each, then
shows the median and worst of every station on screen (m_main.c). This runs
it in ares, reads that table out of memory, prints it in FRT ticks (5.56 us
each; a 60 fps frame has about 3,000, a 30 fps one 6,000) and saves two
pictures of each station and one of the table. On a machine there is only
the screen to read: the same table, of the same 128 pictures.
"""
import argparse, csv, os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARES = os.environ.get("ARES", os.path.expanduser("~/src/ares-debug/build_macos/headless-ui/Release/ares-headless"))
STATION = 144 * 3    # vblanks a station can take: 144 frames at 20 a second
ap = argparse.ArgumentParser()
ap.add_argument("--shots", default=os.path.join(ROOT, "shots/snap"))
ap.add_argument("--label", default="")
args = ap.parse_args()

sym = {}
for line in open(os.path.join(ROOT, "rom/voxelike.lst")):
    parts = line.split()
    sym[parts[-1]] = parts[0]
if "_snap_done" not in sym:
    sys.exit("the ROM was not built with SNAP: make SNAP=2 POLY=2")
names_file = os.path.join(ROOT, "port/snap_test.txt")
names = open(names_file).read().split("\n")[:-1] if os.path.exists(names_file) else []
names = names or ["the level"]
count = len(names)
frames = 600 + count * STATION
os.makedirs(args.shots, exist_ok=True)
label = args.label + "-" if args.label else ""

with tempfile.TemporaryDirectory() as tmp:
    log, table, done = (os.path.join(tmp, n) for n in ("frames.csv", "table.bin", "done.bin"))
    cmd = [ARES, "--frames", str(frames),
           "--gameframe", f"sdram:0x{sym['_game_render_ticks']}", "--gameframe-log", log,
           "--gameframe-watch", f"sdram:0x{sym['_snap_at']}:4",
           "--gameframe-watch", f"sdram:0x{sym['_snap_done']}:4",
           "--dump", f"sdram:0x{sym['_snap_result']}:{count * 4}:{table}",
           "--dump", f"sdram:0x{sym['_snap_done']}:4:{done}"]
    prof = os.path.join(tmp, "prof.bin")
    if "_snap_prof" in sym: cmd += ["--dump", f"sdram:0x{sym['_snap_prof']}:{16 * 9 * 4}:{prof}"]
    shots = {}
    for at in range(0, frames, 30):
        shots[at] = os.path.join(tmp, f"{at}.png")
        cmd += ["--screenshot", f"{at}:{shots[at]}"]
    shots[frames - 1] = os.path.join(tmp, "table.png")
    cmd += ["--screenshot", f"{frames - 1}:{shots[frames - 1]}"]
    subprocess.run(cmd + [os.path.join(ROOT, "rom/voxelike.32x")], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if int.from_bytes(open(done, "rb").read(), "big") != count:
        sys.exit(f"the ROM had not timed all {count} stations after {frames} frames")
    kinds = open(prof, "rb").read() if "_snap_prof" in sym else b""
    raw = open(table, "rb").read()
    result = [(int.from_bytes(raw[i * 4:i * 4 + 2], "big"), int.from_bytes(raw[i * 4 + 2:i * 4 + 4], "big"))
              for i in range(count)]
    # Two pictures a station: a third and two thirds of the way round.
    rows = list(csv.reader(open(log)))[1:]
    seen = {}
    for at, path in sorted(shots.items()):
        if at >= len(rows) or len(rows[at]) < 4 or not rows[at][2] or not rows[at][3]: continue
        if int(rows[at][3], 16): continue
        seen.setdefault(int(rows[at][2], 16), []).append(path)
    for station, paths in seen.items():
        if not 0 <= station < count: continue
        name = names[station].replace(" ", "-")
        for k in (1, 2):
            os.replace(paths[len(paths) * k // 3], os.path.join(args.shots, f"{label}{station:02d}-{name}-{k - 1}.png"))
    os.replace(shots[frames - 1], os.path.join(args.shots, f"{label}table.png"))

print(f"{'station':<22}{'median':>8}{'worst':>7}   fps at worst")
for name, (median, worst) in zip(names, result):
    print(f"{name:<22}{median:>8}{worst:>7}   {60 // -(-worst // 3000)}")
print(f"{'mean, worst':<22}{sum(m for m, _ in result) // count:>8}{max(w for _, w in result):>7}")
print("pictures in", args.shots)
if kinds:
    # make POLY_PROF=1: the primary CPU's mean ticks a frame on each kind of thing.
    heads = ["floor", "ground", "ledge", "wall", "terrain", "creature", "draw", "frame", "other cpu"]
    print("\nprimary CPU, mean ticks a frame; 'rest' is draw less the kinds, the walk over the cells")
    print(f"{'station':<22}" + "".join(f"{h:>10}" for h in heads) + f"{'rest':>8}")
    for n, name in enumerate(names):
        v = [int.from_bytes(kinds[(n * 9 + k) * 4:(n * 9 + k) * 4 + 4], "big") // 128 for k in range(9)]
        print(f"{name:<22}" + "".join(f"{x:>10}" for x in v) + f"{v[6] - sum(v[:6]):>8}")
