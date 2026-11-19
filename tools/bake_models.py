#!/usr/bin/env python3
"""Bake models/materials.txt and models/creatures.txt for the renderer.

usage: tools/bake_models.py [--header sh_src/baked.h] [--solid sh_src/baked3d.h]
                            [--preview shots/creatures.png]

The camera never turns, so a voxel creature always lands on the same pixels.
Each creature is therefore baked into eight column sprites, one per voxel
column of its cell, that the renderer stamps after the terrain behind it.
The preview draws every creature with the renderer's projection so the art
can be checked without building a ROM.

A camera that does turn (sh_src/persp.c) needs the creature itself. --solid
writes each one as 8x8 columns of voxels, the same shape as a terrain model,
so the renderer can stand it on a cell. Columns are solid from the ground
up: where the side view is cut away under something solid the layer is
filled out to it, a gap the front view shows (between legs, under a belly)
is filled with shadow, and a creature drawn hovering is set down on the
floor.
"""
import argparse, os, struct, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CELL, MAX_LAYERS = 8, 14
ROWS = CELL + MAX_LAYERS          # sprite rows: 14 above the cell, 8 across it
ORIGIN = MAX_LAYERS + 1           # sprite row of the cell's back edge at floor level


def die(msg):
    sys.exit(f"bake_models: {msg}")


def read_materials(path):
    mats = []
    for n, line in enumerate(open(path), 1):
        line = line.split("#")[0].split()
        if not line:
            continue
        if len(line) != 5:
            die(f"{path}:{n}: want name r g b key")
        name, r, g, b, key = line
        mats.append((name, (int(r), int(g), int(b)), key))
    if len(mats) > 31:
        die(f"{path}: {len(mats)} materials, CRAM holds 31")
    return mats


def read_creatures(path, keys):
    creatures, cur = {}, None
    for n, raw in enumerate(open(path), 1):
        line = raw.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        if line.startswith("="):
            _, ch, name = line.split(None, 2)
            cur = creatures[ch] = {"name": name, "layers": []}
            continue
        parts = line.split()
        if cur is None or len(parts) not in (2, 3) or any(len(p) != CELL for p in parts):
            die(f"{path}:{n}: want 8 front characters, 8 side characters and maybe 8 back ones")
        for c in parts[0] + "".join(parts[2:]):
            if c != "." and c not in keys:
                die(f"{path}:{n}: no material has key '{c}'")
        if parts[1].strip(".#"):
            die(f"{path}:{n}: side may only hold . and #")
        cur["layers"].append(parts)
    for ch, c in creatures.items():
        if not 0 < len(c["layers"]) <= MAX_LAYERS:
            die(f"{path}: '{ch}' has {len(c['layers'])} layers, limit is {MAX_LAYERS}")
    return creatures


def bake(creature, keys):
    """Return sprite[lx][row]: 0 empty, else material * 8, +1 on a top face."""
    layers = creature["layers"]
    height = len(layers)

    def voxel(x, y, z):
        if not 0 <= z < height:
            return 0
        front, side = layers[height - 1 - z][:2]
        return keys[front[x]] if front[x] != "." and side[y] == "#" else 0

    sprite = [[0] * ROWS for _ in range(CELL)]
    for x in range(CELL):
        for y in range(CELL):                 # back to front: nearer voxels win
            for z in range(height):
                mat = voxel(x, y, z)
                if not mat:
                    continue
                row = y - z - 1 + ORIGIN
                sprite[x][row] = mat * 8
                if not voxel(x, y, z + 1):
                    sprite[x][row - 1] = mat * 8 + 1
    return sprite


def solid(creature, keys, shadow):
    """Return cols[x][y]: the materials of a column, bottom first."""
    layers = creature["layers"]
    while layers and not layers[-1][0].strip("."):
        layers = layers[:-1]              # hovering: set it down
    height = len(layers)
    cols = [[[] for _ in range(CELL)] for _ in range(CELL)]

    def filler(x, z):
        """What stands in for a missing voxel under a solid one: the layer's
        own material if the creature is that wide there, otherwise shadow."""
        front = layers[height - 1 - z][0]
        return keys[front[x]] if front[x] != "." else shadow

    for z in range(height):
        front, side = layers[height - 1 - z][:2]
        # The back half of a layer is coloured from the back view, which is
        # drawn as seen from behind and so runs the other way. Without one
        # there is no face to show: it takes the layer's commonest material.
        back = layers[height - 1 - z][2][::-1] if len(layers[height - 1 - z]) == 3 else None
        used = [c for c in front if c != "."]
        plain = keys[max(set(used), key=used.count)] if used else 0
        for x in range(CELL):
            for y in range(CELL):
                if front[x] != "." and side[y] == "#":
                    col = cols[x][y]
                    col += [filler(x, k) for k in range(len(col), z)]
                    behind = keys[back[x]] if back and back[x] != "." else plain
                    col.append(keys[front[x]] if y >= CELL // 2 else behind)
    return cols


def write_solid(path, order, solids, gear):
    out = ["/* Generated by tools/bake_models.py from models/. Do not edit. */",
           "#ifndef BAKED3D_H", "#define BAKED3D_H", "", '#include "voxel.h"', "",
           "/* Creatures as voxel columns: [creature][x][y], y 0 at its back.",
           " * Creature 0 is the hero, 1..26 are Rogue's monsters A..Z. */",
           f"static const vcol_t ent_solid[{len(order)}][8][8] = {{"]
    for ch, name in order:
        out.append(f"    {{ /* {ch} {name} */")
        for x in range(CELL):
            cells = []
            for y in range(CELL):
                col = solids[ch][x][y]
                cells.append("{ %d, { %s } }" % (len(col), ", ".join(str(m * 8) for m in col) or "0"))
            out.append("        { " + ", ".join(cells) + " },")
        out.append("    },")
    out += ["};", "",
            "/* What the hero wears and holds, from models/gear.txt. */",
            "enum {"]
    out += [f"    GEAR_{name.upper()}," for name, _ in gear]
    out += ["    GEAR_COUNT", "};",
            f"static const vcol_t gear_solid[{len(gear)}][8][8] = {{"]
    for name, cols in gear:
        out.append(f"    {{ /* {name} */")
        for x in range(CELL):
            cells = []
            for y in range(CELL):
                col = cols[x][y]
                cells.append("{ %d, { %s } }" % (len(col), ", ".join(str(m * 8) for m in col) or "0"))
            out.append("        { " + ", ".join(cells) + " },")
        out.append("    },")
    out += ["};", "", "#endif", ""]
    open(path, "w").write("\n".join(out))


def read_terrain(path, keys):
    """Read 8x8 top textures, with the same material keys as creatures."""
    textures = {}
    for number, line in enumerate(open(path), 1):
        row = line.strip()
        if not row or row.startswith("#"):
            continue
        if row.startswith("= "):
            name = row[2:]
            if name not in ("floor", "wall_cap") or name in textures:
                die(f"{path}:{number}: invalid or duplicate terrain name")
            textures[name] = []
        elif not textures or len(row) != CELL or any(c not in keys for c in row):
            die(f"{path}:{number}: want eight material keys")
        else:
            textures[name].append([keys[c] * 8 for c in row])
    if set(textures) != {"floor", "wall_cap"} or any(len(rows) != CELL for rows in textures.values()):
        die(f"{path}: want one 8x8 floor and one 8x8 wall_cap")
    return textures


def write_header(path, mats, order, sprites, textures):
    out = ["/* Generated by tools/bake_models.py from models/. Do not edit. */",
           "#ifndef BAKED_H", "#define BAKED_H", "", "#include <stdint.h>", "",
           "enum {"]
    out += [f"    MT_{name}{' = 1' if i == 0 else ''}," for i, (name, _, _) in enumerate(mats)]
    out += ["    MT_COUNT", "};", "",
            "static const uint8_t mat_rgb[MT_COUNT][3] = {"]
    out += [f"    [MT_{name}] = {{ {r:2}, {g:2}, {b:2} }}," for name, (r, g, b), _ in mats]
    out += ["};", ""]
    for name, rows in textures.items():
        out.append(f"static const uint8_t terrain_{name}[8][8] = {{")
        out += ["    { " + ", ".join(map(str, row)) + " }," for row in rows]
        out += ["};", ""]
    out += [
            "/* Creature column sprites: [creature][voxel column][row], top row first.",
            " * 0 is empty, otherwise a CRAM base index with bit 0 set on top faces.",
            " * Row ENT_ORIGIN is the back edge of the creature's cell at floor level.",
            " * Creature 0 is the hero, 1..26 are Rogue's monsters A..Z. */",
            f"#define ENT_ROWS   {ROWS}", f"#define ENT_ORIGIN {ORIGIN}",
            f"#define N_ENTS     {len(order)}", "",
            "static const uint8_t ent_sprite[N_ENTS][8][ENT_ROWS] = {"]
    for ch, name in order:
        out.append(f"    {{ /* {ch} {name} */")
        for col in sprites[ch]:
            out.append("        { " + ", ".join(f"{v:3}" for v in col) + " },")
        out.append("    },")
    out += ["};", "", "#endif", ""]
    open(path, "w").write("\n".join(out))


def write_png(path, width, height, pixels):
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    raw = b"".join(b"\0" + bytes(pixels[y * width * 3:(y + 1) * width * 3]) for y in range(height))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" +
                           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def write_preview(path, mats, order, sprites):
    per_row, scale, pad = 9, 8, 2
    cw, ch_ = CELL + pad, ROWS + pad
    rows = -(-len(order) // per_row)
    W, H = per_row * cw * scale, rows * ch_ * scale
    px = [18, 16, 24] * (W * H)

    def shade(mat, s):
        r, g, b = mats[mat - 1][1]
        return [c * (s + 2) // 9 * 255 // 31 for c in (r, g, b)]

    def plot(cx, cy, fx, fy, rgb):
        x0, y0 = (cx * cw + pad + fx) * scale, (cy * ch_ + pad + fy) * scale
        for y in range(y0, y0 + scale):
            for x in range(x0, x0 + scale):
                px[(y * W + x) * 3:(y * W + x) * 3 + 3] = rgb

    floor = next(i for i, m in enumerate(mats, 1) if m[0] == "STONE")
    for i, (ch, _) in enumerate(order):
        cx, cy = i % per_row, i // per_row
        for x in range(CELL):
            for y in range(CELL):                      # the floor tile under it
                plot(cx, cy, x, y - 1 + ORIGIN, shade(floor, 7))
            for row, v in enumerate(sprites[ch][x]):
                if v:
                    plot(cx, cy, x, row, shade(v >> 3, 7 if v & 1 else 5))
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    write_png(path, W, H, px)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header")
    ap.add_argument("--solid")
    ap.add_argument("--preview")
    args = ap.parse_args()

    mats = read_materials(os.path.join(ROOT, "models/materials.txt"))
    keys = {key: i for i, (_, _, key) in enumerate(mats, 1) if key != "-"}
    creatures = read_creatures(os.path.join(ROOT, "models/creatures.txt"), keys)

    wanted = ["@"] + [chr(c) for c in range(ord("A"), ord("Z") + 1)]
    missing = [ch for ch in wanted if ch not in creatures]
    if missing:
        die("no creature for " + " ".join(missing))
    order = [(ch, creatures[ch]["name"]) for ch in wanted]
    sprites = {ch: bake(creatures[ch], keys) for ch in wanted}

    if args.header:
        textures = read_terrain(os.path.join(ROOT, "models/terrain.txt"), keys)
        write_header(args.header, mats, order, sprites, textures)
    if args.solid:
        shadow = next(i for i, m in enumerate(mats, 1) if m[0] == "SHADOW")
        gear = read_creatures(os.path.join(ROOT, "models/gear.txt"), keys)
        write_solid(args.solid, order, {ch: solid(creatures[ch], keys, shadow) for ch in wanted},
                    [(g["name"], solid(g, keys, shadow)) for g in gear.values()])
    if args.preview:
        write_preview(args.preview, mats, order, sprites)


main()
