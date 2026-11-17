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

## Living heights

Rogue's map is flat. The renderer is not obliged to be:

- Every room stands on a brick plinth 2 to 4 voxels high, a different height
  from its neighbours (`rogue_ground` in `port/rogue_port.c` decides, from
  the room's number and the dungeon level). Passages run at ground level and
  step up halfway where they meet a door.
- The stairs' cell is sunk two voxels below its room, so the way down is a
  pit with steps into it.
- Rogue stops showing the floor of a dark room the moment the hero steps
  off it. The renderer keeps what was seen and draws it dimmer, so a dark
  room is explored and stays explored.

Creatures and falling pieces stand on whatever ground is under them. Both
renderers draw the plinths.

## Perspective prototype

`make PERSP=1` builds the same game with a second renderer, `sh_src/persp.c`,
in place of the cached one: a camera with perspective that circles the hero.
Hold X or Z to swing it. `PERSP=2` turns it by itself, for measuring. Plain
`make` brings the normal ROM back.

- It is a voxel-space renderer: one ray per screen column across the ground,
  nearest first. A cell that is one height all over (nothing, floor, wall) is
  settled in one step, a run of empty cells in one more, and only the rest
  are walked voxel by voxel.
- Both SH-2s draw: the primary the even columns, the secondary the odd ones,
  started through COMM6. The secondary purges its cache first, because it
  has not seen what the primary wrote to the world.
- The camera basis and per-column reciprocals come from SH2ZAM
  (`third_party/SH2ZAM`, MIT, a vendored subset).
- Each cell's facts sit in one 16 byte record, a cache line, and a floor's
  colours in 64 bytes: on hardware the SH-2's 4K cache decides the speed.
- Measured in ares over three dungeon levels played by the bot
  (`make BOT=1 PERSP=2`): a frame takes 3395 ticks at the median and 4941 at
  worst, inside the 6000 that 30 frames a second allows. One SH-2 and no
  shortcuts gave 5 frames a second; whole-cell steps 16; the second SH-2 28;
  empty runs and cheaper pixel math 30 and up. ares counts instructions and
  not memory waits, so hardware will differ.
- For the number that counts, `make BOT=1 PERSP=2 deploy` puts a ROM on the
  MiSTer that needs no pad: it starts itself, the bot plays, the camera
  turns, and a yellow line reads `FPS`, `MEAN` and `WORST` (FRT ticks to
  draw a frame; 3000 is one vblank). Not run on hardware yet.
- Creatures are real voxel models here: `tools/bake_models.py --solid` turns
  the same two drawings into 8x8 columns of voxels, and the renderer stands
  them on the cells they are over, turned to face the way they last moved,
  mid-step and mid-hop included. A flash colours the whole model.
- Up on the D-pad is away from the camera: the pad's direction is turned by
  the camera's, to the nearest eighth of a circle.
- Prototype limits: a creature is solid from the ground up (a gap under a
  belly is shadow, and fliers stand on the floor), its back is drawn without
  a face but otherwise from the front view, the dead vanish with no burst of
  voxels, and far floor shimmers. The build uses 225K of the 256K of RAM.

## Combat on screen

Rogue reports a fight as text. The renderer shows it by watching the game's
own data, with no change to Rogue's source:

- The hero jabs at a monster he walks into.
- A monster that strikes at the hero jabs back a moment later. The linker's
  `--wrap=attack` routes Rogue's one call to `attack()` through
  `port/rogue_port.c`, which notes who struck.
- Anything that lost hit points since the last turn flashes: monsters white,
  the hero red.
- A monster that is gone from Rogue's monster list died, and its sprite is
  thrown apart as loose voxels that fall, lie a moment and vanish.

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
- `make BOT=1 && tools/botrun.py --frames 36000` plays the game with no
  pad. The bot (`port/bot.c`, test builds only) collects what lies on each
  level, tries it, fights what stands next to it and takes the stairs; it
  cannot die or starve. The tool saves screenshots and contact sheets to
  `shots/bot/` and prints the level reached over time, frame times and the
  final screen. Ten emulated minutes take about five and reach level 19.
  Run plain `make` afterwards to get the playable ROM back.
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
