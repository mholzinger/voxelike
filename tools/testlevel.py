#!/usr/bin/env python3
"""Make port/snap_test.h: a level with every case the renderers have to draw.

usage: tools/testlevel.py

make SNAP=2 builds a ROM that shows this level and stands the hero at each
of its stations in turn; tools/snapbench.py times and photographs each.
The maze is the one out of port/snap_data.h, a level that came up in play.
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 80, 24
g = [[" "] * W for _ in range(H)]
lift = [[0] * W for _ in range(H)]
rooms = {}

def put(x, y, ch): g[y][x] = ch

def room(name, x0, y0, x1, y1, height):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            edge_y, edge_x = y in (y0, y1), x in (x0, x1)
            g[y][x] = "-" if edge_y else "|" if edge_x else "."
            lift[y][x] = height
    rooms[name] = (x0, y0, x1, y1)

def hall(*points):
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        for x in range(min(ax, bx), max(ax, bx) + 1):
            for y in range(min(ay, by), max(ay, by) + 1):
                if g[y][x] == " ": g[y][x] = "#"

def scatter(text, cells):
    for ch, (x, y) in zip(text, cells): put(x, y, ch)

# A plain lit room with a door in each wall, things on its floor and two monsters.
room("plain", 3, 3, 17, 8, 2)
for x, y in ((10, 3), (10, 8), (3, 5), (17, 6)): put(x, y, "+")
scatter("!*?)]:=/", [(5, 4), (7, 6), (12, 7), (14, 4), (9, 7), (15, 7), (5, 7), (12, 4)])
scatter("KS", [(13, 5), (6, 6)])
# Stairs against each wall, in a corner and in the open.
room("stairs", 24, 3, 38, 8, 3)
for x, y in ((24, 6), (38, 6), (34, 8)): put(x, y, "+")
for x, y in ((28, 4), (31, 7), (25, 4), (37, 4), (37, 7), (30, 5)): put(x, y, "%")
# A dark room, all of it found.
room("dark", 44, 3, 58, 8, 1)
for x, y in ((44, 6), (50, 8)): put(x, y, "+")
scatter("!*B", [(47, 4), (55, 7), (53, 5)])
# The largest room Rogue makes, crowded.
room("crowd", 2, 13, 27, 20, 4)
put(12, 13, "+")
scatter("ABCEHIJKORSZ", [(4, 14), (8, 15), (11, 18), (15, 14), (17, 16), (20, 18), (22, 15), (24, 17),
                         (6, 18), (13, 16), (19, 14), (25, 19)])
scatter("!?*):]=/!*", [(5, 16), (9, 14), (10, 17), (16, 18), (18, 15), (21, 16), (23, 19), (7, 19), (14, 19), (26, 14)])
# A small room at the far end of a passage.
room("end", 60, 14, 68, 19, 2)
put(68, 16, "+")

hall((10, 2), (10, 1), (22, 1))                      # north door of the plain room
hall((10, 9), (10, 10))                              # its south door
hall((2, 5), (1, 5), (1, 10))                        # its west door
hall((18, 6), (23, 6))                               # east, to the stairs room
hall((39, 6), (43, 6))                               # the stairs room to the dark one
hall((34, 9), (34, 10)); hall((50, 9), (50, 10)); hall((12, 12), (12, 11), (12, 10))
hall((1, 10), (70, 10))                              # a long passage east and west
hall((70, 1), (70, 21)); hall((69, 16), (70, 16))    # and one north and south

# The maze: what a level in play had found of its passages.
snap = open(os.path.join(ROOT, "port/snap_data.h")).read()
cells = re.search(r"snap_screen\[[^\]]*\] = \{([^}]*)\}", snap).group(1)
old = [int(v) & 0x7F for v in cells.replace("\n", " ").split(",") if v.strip()]
for y in range(7, 18):
    for x in range(11, 32):
        if chr(old[y * W + x]) in "#@" and g[y + 4][x + 23] == " ": g[y + 4][x + 23] = "#"

# A passage runs at ground level and steps up halfway where it meets a door.
for y in range(H):
    for x in range(W):
        if g[y][x] != "#": continue
        beside = [lift[y + dy][x + dx] for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                  if 0 <= x + dx < W and 0 <= y + dy < H and g[y + dy][x + dx] == "+"]
        lift[y][x] = max(beside) // 2 if beside else 0

for x, ch in enumerate("Level: 1  Gold: 0      Hp: 12(12)  Str: 16(16)  Arm: 4   Exp: 1/0"): put(x, 23, ch)

# Where the hero is stood, and the lit room he is then in if he is in one.
stations = [
    ("plain room",        10, 5, "plain"),
    ("doorway",           10, 8, "plain"),
    ("stairs room",       33, 6, "stairs"),
    ("dark room",         50, 5, None),
    ("dark room corner",  46, 7, None),
    ("crowded room",      14, 17, "crowd"),
    ("passage east-west", 42, 10, None),
    ("passage south end", 70, 20, None),
    ("passage middle",    70, 11, None),
    ("passage by a room", 22, 6, None),
    ("maze",              48, 14, None),
    ("maze edge",         40, 11, None),
]

def table(name, rows):
    body = ",\n    ".join(", ".join(str(v) for v in row) for row in rows)
    return f"static const unsigned char {name}[24 * 80] = {{\n    {body}\n}};\n"

path = os.path.join(ROOT, "port/snap_test.h")
with open(path, "w") as f:
    f.write("/* Made by tools/testlevel.py: a level with every case the renderers draw. */\n")
    f.write(table("snap_screen", [[ord(c) for c in row] for row in g]))
    f.write(table("snap_lift", lift))
    f.write("/* x, y, whether the room is lit, and its walls' x0 y0 x1 y1. */\n")
    f.write(f"#define SNAP_STATIONS {len(stations)}\n")
    f.write("static const unsigned char snap_stations[SNAP_STATIONS][7] = {\n")
    for name, x, y, lit in stations:
        r = rooms[lit] if lit else (0, 0, 0, 0)
        f.write(f"    {{ {x}, {y}, {1 if lit else 0}, {r[0]}, {r[1]}, {r[2]}, {r[3]} }},   /* {name} */\n")
    f.write("};\n")
    f.write("static const char *const snap_names[SNAP_STATIONS] = {\n")
    for name, *_ in stations:
        f.write(f'    "{name.upper()}",\n')
    f.write("};\n")
with open(os.path.join(ROOT, "port/snap_test.txt"), "w") as f:
    f.write("\n".join(name for name, *_ in stations) + "\n")
print(path)
marks = {(x, y): str(i % 10) for i, (_, x, y, _) in enumerate(stations)}
for y in range(H):
    print("".join(marks.get((x, y), g[y][x]) for x in range(W)).rstrip())
for i, (name, x, y, lit) in enumerate(stations): print(f"  {i % 10}: {name} at {x},{y}" + (f" in the lit {lit} room" if lit else ""))
