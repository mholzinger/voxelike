#!/usr/bin/env python3
"""Bake the title's voxel animation into sparse, 2x2-pixel runs for the ROM."""
import math
import re
import sys
from pathlib import Path

WIDTH, HEIGHT = 160, 112
LAST_FRAME = 118  # 20 drawings per second on a 60 Hz console
EXIT_FRAMES = 16

DUNGEON = (
    "          ----",
    "         #|..|",
    "  ########+..|",
    "          |..|",
    "          ----",
)
LOGO = (
    ("#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.."),
    ("....", "....", ".##.", "#..#", "#..#", "#..#", ".##."),
    (".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"),
    (".....", ".....", ".###.", "#...#", "#####", "#....", ".####"),
    ("##", ".#", ".#", ".#", ".#", ".#", "##"),
    ("#", ".", "#", "#", "#", "#", "#"),
    ("#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."),
    (".....", ".....", ".###.", "#...#", "#####", "#....", ".####"),
)
# Front, near side, right side, for each of the terminal and hero materials.
PALETTE = ((0, 0, 0), (30, 28, 22), (17, 18, 16), (10, 13, 13),
           (31, 22, 7), (22, 12, 3), (14, 7, 2))


def ease(value):
    value = min(1, max(0, value))
    return value * value * (3 - 2 * value)


def lerp(a, b, amount):
    return a + (b - a) * amount


def glyphs(path):
    rows = re.findall(r"\.long\s+0x([0-9a-fA-F]{8})", path.read_text())
    return {chr(32 + i): [(x, y) for y, row in enumerate(rows[i*8:i*8+8])
                           for x, pixel in enumerate(row) if pixel != "0"]
            for i in range(len(rows) // 8)}


def dungeon(font, step):
    cubes = []
    for y, row in enumerate(DUNGEON):
        for x, char in enumerate(row):
            if (x, y) == (7 + step, 2):
                char = "@"
            cubes.extend((x * 8 + dx - 63, y * 8 + dy - 19, char == "@")
                         for dx, dy in font[char])
    return cubes


def title_cells():
    cells = []
    width = sum(len(letter[0]) for letter in LOGO) + (len(LOGO) - 1) * 2
    left = -width / 2
    for letter_idx, letter in enumerate(LOGO):
        for y, row in enumerate(letter):
            for x, pixel in enumerate(row):
                if pixel == "#":
                    amber = letter_idx == 0 and y >= 5
                    cells.append((left + x, y - 3, amber))
        left += len(letter[0]) + 2
    return cells


def assembly(source):
    """Assign every dungeon pixel a place in the face or depth of a letter."""
    targets = title_cells()
    paths = []
    for amber in (True, False):
        starts = sorted(cube for cube in source if cube[2] == amber)
        ends = sorted(cube for cube in targets if cube[2] == amber)
        for i, end in enumerate(ends):
            first = i * len(starts) // len(ends)
            last = (i + 1) * len(starts) // len(ends)
            for layer, start in enumerate(starts[first:last]):
                paths.append((start, end, layer))
    return paths


def polygon(image, points, color):
    """Fill at pixel centers so adjacent faces share a clean edge."""
    edges = list(zip(points, points[1:] + points[:1]))
    for y in range(max(0, math.ceil(min(p[1] for p in points) - .5)),
                   min(HEIGHT, math.ceil(max(p[1] for p in points) - .5))):
        scan = y + .5
        cuts = [lerp(a[0], b[0], (scan - a[1]) / (b[1] - a[1]))
                for a, b in edges if min(a[1], b[1]) <= scan < max(a[1], b[1])]
        if len(cuts) < 2:
            continue
        left = max(0, math.ceil(min(cuts) - .5))
        right = min(WIDTH, math.ceil(max(cuts) - .5))
        if right > left:
            image[y][left:right] = bytes([color]) * (right - left)


def render(cubes, tilt, skew, lens, center_y):
    image = [bytearray(WIDTH) for _ in range(HEIGHT)]
    sine, cosine = math.sin(tilt), math.cos(tilt)

    def project(x, y, z):
        depth = y * sine + z * cosine
        scale = lens / (lens - depth)
        return (80 + (x + z * skew) * scale, center_y + (y * cosine - z * sine) * scale)

    # Positive Y is the near edge. The cube backs extend away from the eye.
    for x, y, z, size, amber in sorted(cubes, key=lambda cube: cube[1] * sine + cube[2] * cosine):
        color = 4 if amber else 1
        # Remove cubes as their leading face reaches the camera's near plane.
        if (y + size) * sine + z * cosine >= lens - 8:
            continue
        front = [project(x + u * size, y + v * size, z)
                 for u, v in ((0, 0), (1, 0), (1, 1), (0, 1))]
        back = [project(x + u * size, y + v * size, z - size)
                for u, v in ((0, 0), (1, 0), (1, 1), (0, 1))]
        polygon(image, [front[2], front[3], back[3], back[2]], color + 1)
        # Perspective can expose the left or right side depending on position.
        for a, b in ((0, 3), (2, 1)):
            face = [front[a], front[b], back[b], back[a]]
            area = sum(p[0] * q[1] - q[0] * p[1] for p, q in zip(face, face[1:] + face[:1]))
            if area < 0:
                polygon(image, face, color + 2)
        polygon(image, front, color)
    return image


def draw(frame, scenes, paths):
    reveal = ease((frame - 22) / 23)
    # Lift the camera as the first glyphs peel up, then hold it while
    # the player follows the flights.
    camera = ease((frame - 50) / 21)
    settle = ease((frame - 103) / 15)
    tilt = 1.18 * reveal - .83 * camera - .13 * settle
    skew = .26 * reveal + .22 * camera + .16 * settle
    lens = lerp(10000, 185, reveal)
    center_y = 51 + 5 * reveal - 7 * camera
    if frame <= 50:
        step = int(frame >= 8) + int(frame >= 16)
        cubes = [(x, y, 0, 1, amber) for x, y, amber in scenes[step]]
    else:
        cubes = []
        for (x, y, amber), (tx, ty, _), layer in paths:
            # A wave runs down the corridor into the room. The upper
            # and lower pieces split into lanes as they leave the glyphs.
            delay = (x + 56) * .23 + (y + 20) * .13 + layer * 1.2
            if amber:
                delay = 30 + layer * .55
            progress = (frame - 50 - delay) / (34 if amber else 29)
            t = ease(progress)
            lift = math.sin(math.pi * t)
            arc = -25 if y < 1 else 19
            if amber:
                arc = 28
            # Rear cubes arrive behind the face, giving the letters depth.
            # Only the nearest few can be seen in the finished title.
            depth = 2 * layer / (layer + 2)
            cubes.append((lerp(x, tx * 3, t),
                          lerp(y, ty * 3, t) + arc * lift,
                          -depth * t + 7 * lift,
                          lerp(1, 3, t), amber))
    return render(cubes, tilt, skew, lens, center_y)


def flythrough(frame, paths):
    """Dolly through the word while its blocks peel out of the flight path."""
    t = frame / EXIT_FRAMES
    travel = t * t * 217
    peel = ease(t * 1.65)
    cubes = []
    for _, (tx, ty, amber), layer in paths:
        x, y = tx * 3, ty * 3
        depth = 2 * layer / (layer + 2)
        # Fan the cubes around the lens, with a small roll in screen space.
        side = -1 if x < 0 else 1
        x += side * peel * (5 + abs(y) * .8) - y * peel * .7
        y += peel * (tx * .55 + (-9 if ty < 0 else 9))
        cubes.append((x, y, travel - depth + abs(tx) * peel * 1.4, 3, amber))
    return render(cubes, .22, lerp(.64, .15, peel), 185, 49)


def frame_runs(image):
    """Each run is (row, column, packed length/color). 0xff ends the frame."""
    for y, row in enumerate(image):
        x = 0
        while x < WIDTH:
            color = row[x]
            right = x + 1
            while right < min(x + 32, WIDTH) and row[right] == color:
                right += 1
            if color:
                yield from (y, x, ((right - x - 1) << 3) | color)
            x = right
    yield 255


def bake(font_path, output):
    font = glyphs(font_path)
    scenes = [dungeon(font, step) for step in range(3)]
    paths = assembly(scenes[-1])
    frames = {}
    offsets = []
    stream = bytearray()
    images = (draw(frame, scenes, paths) if frame <= LAST_FRAME
              else flythrough(frame - LAST_FRAME, paths)
              for frame in range(LAST_FRAME + EXIT_FRAMES + 1))
    for image in images:
        runs = bytes(frame_runs(image))
        if runs not in frames:
            frames[runs] = len(stream)
            stream.extend(runs)
        offsets.append(frames[runs])

    with output.open("w") as out:
        print("/* Generated by tools/bake_splash.py. */", file=out)
        print(f"#define SPLASH_TITLE_FRAME {LAST_FRAME}", file=out)
        print("static const uint16_t splash_palette[] = {", file=out)
        for rgb in PALETTE:
            print("    COLOR({}, {}, {}),".format(*rgb), file=out)
        print("};", file=out)
        for kind, name, data in (("uint32_t", "splash_offsets", offsets),
                                 ("uint8_t", "splash_runs", stream)):
            print(f"static const {kind} {name}[] = {{", file=out)
            for i in range(0, len(data), 16):
                print("    " + ", ".join(map(str, data[i:i+16])) + ",", file=out)
            print("};", file=out)


if __name__ == "__main__":
    bake(Path(sys.argv[1]), Path(sys.argv[2]))
