# voxelike32x

Rogue, drawn as a voxel world on the Sega 32X. Fixed tilted top-down camera in
the spirit of 3D Dot Game Heroes.

Status: playable. Rogue 5.4.4 runs unmodified behind the voxel renderer, with
its messages, status, inventory, item prompts and tombstone on screen and every
common command reachable from the pad. The hero and all 26 monsters are voxel
models that walk from cell to cell instead of jumping, the world takes turns
on a clock while the player stands still, the dungeon scrolls smoothly, and
Start begins a new game after death.
Not done: saving, high scores and naming items.

## Build

```
export MARSDEV=/path/to/marsdev/mars
make            # rom/voxelike.32x
make shot       # build, run headless in ares, write shots/latest.png
make shot FRAMES=320 INPUT=keys.csv
make deploy     # scp to a MiSTer (MISTER=root@host)
```

`make shot` needs the headless ares fork (`ARES=` overrides its path).

## How the renderer works

- The level is Rogue's own 80x24 curses screen, held as a character grid in
  RAM. `vox_load_screen` turns each character into terrain (an 8x8 footprint
  of voxel columns, up to 15 tall) or a creature standing on terrain.
- One voxel is 2x2 screen pixels. Horizontal doubling is a 16-bit write and
  vertical doubling comes from the 32X line table.
- The camera is a fixed oblique projection: voxel (x, y, z) lands on column
  `x - cam_x`, row `(y - z) - cam_y`. Each column is walked front to back and
  painted bottom to top, so every terrain pixel is written once.
- Because the camera never turns, a creature always lands on the same pixels.
  Creatures are baked into column sprites and stamped after the terrain, each
  clipped by whatever terrain stands in front of it.
- Rogue only knows cells. The renderer keeps a voxel position for every
  creature (told apart by Rogue's own monster pointer) and walks it to its
  cell with a small hop: 8 frames for a turn the player took, 24 for one the
  world took by itself. The camera follows the hero's in-between position.
- Each framebuffer keeps a rendered strip of the world: every row, and 312
  columns that slide along the framebuffer like a tape. Moving the camera is a
  line table rewrite plus the few columns that came into view. A change in the
  dungeon repaints only the rows of the columns through the changed cells.
- CRAM is `material * 8 + shade`. The hero's room is bright, what he can reach
  in a passage is bright, everything remembered is dim.
- A room's south wall is drawn two voxels tall so it cannot hide the floor
  behind it.

## Creatures

`models/creatures.txt` holds the hero and the 26 monsters as two ASCII
drawings each, a front view in material keys and a side silhouette; a voxel
exists where both agree. `models/materials.txt` is the palette.
`tools/bake_models.py` turns them into `sh_src/baked.h` at build time, and

```
tools/bake_models.py --preview shots/creatures.png
```

draws every creature with the renderer's projection, no ROM needed.

## Time

Rogue is still turn based inside, and its source is still untouched. Left
alone for 24 frames, the port hands Rogue a rest command so monsters keep
moving; the command menu, prompts and text pages stop the clock. Those idle
turns run with the hunger daemon switched off, so thinking costs no food.
A `--More--` in the dungeon moves on by itself after a second, and the last
message stays up for a second and a half even though Rogue has wiped it.
The numbers are `STEP`, `IDLE_TURN`, `MORE_WAIT` and `MSG_HOLD` in
`sh_src/rogue_plat.c`.

## Controls

| Pad | Rogue |
|---|---|
| D-pad, 8 directions | move, hold to keep walking. Attack by walking into a monster |
| A | command menu: inventory, eat, quaff, read, wield, wear, rings, zap, throw, drop, climb, discoveries, quit |
| B | search. In a menu or prompt: back out |
| C | rest one turn. In a menu: choose. At a yes/no prompt: no |
| Start | go down stairs when standing on them, otherwise pick up |
| any button | dismiss `--More--`, a text page or the tombstone |

When Rogue asks "which object?" a list of the pack opens. When it asks for a
direction, use the D-pad. At a yes/no prompt A is yes.

## Text

Text is drawn on the Mega Drive layer, 40 columns of an 8x8 font, in front of
the 32X picture. `sh_src/rogue_plat.c` decides what Rogue is waiting for from
where it left the cursor and what the message line says, then shows the
matching screen. Rogue's 80 column message line is word wrapped onto two rows,
and its status line is rebuilt in a compact form from the same variables.

## Tools

- `make shot` writes the voxel picture after FRAMES frames.
- `tools/screen.py --frames N --input keys.csv` prints Rogue's own 80x24 text
  screen from emulated RAM. Use it to tell a game bug from a render bug.
- `tools/frametimes.py --frames N --input keys.csv` reports render time per
  frame in FRT ticks (5.56 us each; a 60 fps frame has about 3000).
- Input scripts are `frame,button,value` lines. Boot takes about 2 seconds, so
  press Start no earlier than frame 150.

## Credits and licenses

- Boot code, linker scripts and Makefile base: Marsdev 32X skeleton by Andrew
  DeRosier, MIT.
- Build flags, the `.ramtext` section, unsolicited pad publishing and the
  deploy probe are lessons carried over from 32x-builder.
- `md_src/font.s`: generated by `tools/gen_font.py` from font8x8_basic by
  Daniel Hepper, public domain (IBM VGA glyphs).
- `rogue/`: Rogue 5.4.4, unmodified (BSD 3-clause, Michael Toy, Ken Arnold,
  Glenn Wichman). See `rogue/UPSTREAM.md`. `rogue/LICENSE.TXT` must ship with
  any ROM release.
