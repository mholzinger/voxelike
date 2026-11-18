# voxelike32x

Rogue, as a 3D voxel world on the Sega 32X, in the spirit of 3D Dot Game
Heroes. The original game runs untouched underneath; what you see is a
dungeon of voxels and a camera that circles the hero, pulls back, and tips
over into a flat view and out again.

Status: playable, early. Rogue 5.4.4 runs unmodified behind the renderers,
with its messages, status, inventory, item prompts and tombstone on screen
and every common command reachable from the pad. The hero and all 26
monsters are voxel models that walk, hop, jab and fall apart; rooms stand at
different heights; the world takes turns on a clock while the player stands
still; and Start begins a new game after death.
There are sound effects but no music yet.
Not done: music, saving, high scores and naming items. Frame rates here are
from the ares emulator, not yet from hardware.

## Build

```
export MARSDEV=/path/to/marsdev/mars
make            # rom/voxelike.32x: the game
make FLAT=1     # the flat view alone, without the 3D renderer
make STATS=1    # show frames a second and the time to draw one
make BENCH=7    # the same 256 frames every run, for timing (tools/mbench.sh)
make RES=3      # start in a resolution: 0 160x112, 1 320x112, 2 320x224, 3 focus
make shot       # build, run headless in ares, write shots/latest.png
make shot FRAMES=320 INPUT=keys.csv
make deploy     # scp to a MiSTer (MISTER=root@host)
```

`make shot` needs the headless ares fork (`ARES=` overrides its path). The
Makefile lists the rest: `PERSP=2` turns the camera by itself for measuring
and `BOT=1` builds a ROM that plays itself.

## How the flat renderer works

There are two renderers. This one draws the flat view, which the game shows
when the camera is tipped all the way over (and which is the whole game in
a `FLAT=1` build). The 3D one is described under "The 3D camera".

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

## The 3D camera

The game's main view is drawn by `sh_src/persp.c`: a camera with perspective
that circles the hero.

The camera looks after itself, but seldom moves. Up on the D-pad means
away from the camera, so a camera that turns while the player is steering
takes the steering away from him. It turns at two moments only. When the
hero walks into another room, or out into the passages, it takes up a
three-quarter view of the way he came in: within an eighth of a turn of
straight behind him, wherever in that the least stands in the way and the
swing is shortest. And when he walks in behind a wall and cannot be seen
whole, it goes to the nearest place he can be seen from. Inside a room it
otherwise stays where it is. Even then it waits its moment: while a
direction is held and the hero is getting somewhere it does not move,
however many doors he goes through. It moves when the pad is let go, or
when he has walked as far as that direction will take him and stood against
it for a moment; a run of taps counts as holding. What up means is steadier
still. It is always one of the map's four directions, the one nearest to
away from the camera, and it changes only on a push after the pad has
rested, and only if the camera has turned well past halfway to the next
one. So the camera's own small swings leave the controls as they were.
Turning the camera by hand holds all
this off for three seconds. See `follow()` in `sh_src/m_main.c`.

| Pad | Camera |
|---|---|
| X, Z | swing left and right around the hero |
| Y, tapped | swing behind the hero, so up on the D-pad is the way he faces |
| Y, tapped twice | swing back to north |
| Y, held | tip the view over into the flat one, or back out of it |
| MODE, held | pull back to show more, until it is let go |
| MODE, tapped | next resolution: 160x112, 320x112, 320x224, focus |

The resolutions are there to be compared by playing. A voxel at the hero
covers the same 2x2 screen pixels in all three; what changes is how many
pixels are worked out and how many are copies. In one lit room in ares they
ran at 30, 20 and 15 frames a second. Each has its own compiled copy of the
ray routine, so nothing is decided per pixel. Black is written as CRAM
index 1, not 0: the 32X drops a zero written to the framebuffer as a byte.

Focus draws the middle of the screen fine and the rest coarse. The hero is
always at the middle, so a fixed window 64 pixels by 94 scanlines holds him
and the tiles next to him whichever way the camera looks. Coarse columns
that cross the window leave a hole in it, and two fine rays per column fill
the hole; a fine ray starts 32 rows below the window, drawing nothing, so
that whatever stands that near and reaches up into the window is in place
by the first row that shows. Over two bot-played levels in ares a frame
took 5671 ticks at the median and 7681 at worst, between 30 and 20 frames
a second. `make RES=3` starts in it (RES is 0 to 3).

The flat view is the fixed renderer itself, at its 60 frames a second.
Getting there is one continuous move: the focal length grows from 128 to
3072 with the eye going back and up to match, which takes the perspective
out of the picture until a voxel is one fat pixel wherever it stands. That
is the fixed view's projection, so the renderers swap and the picture does
not jump. Turning the camera from the flat view tips it back out first.

- It is a voxel-space renderer: one ray per screen column across the ground,
  nearest first. A cell that is one height all over (nothing, floor, wall) is
  settled in one step, a run of empty cells in one more, and only the rest
  are walked voxel by voxel.
- Both SH-2s draw: the primary the even columns, the secondary the odd ones,
  started through COMM6. The secondary purges its cache first, because it
  has not seen what the primary wrote to the world.
- The camera basis comes from SH2ZAM (`third_party/SH2ZAM`, MIT, a vendored
  subset). Each ray's two reciprocals come from the SH-2's own divider, and
  the columns' slopes from a table made when the lens changes.
- Each cell's facts sit in one 16 byte record, a cache line, and a floor's
  colours in 64 bytes.
- Measured on a MiSTer with `tools/mbench.sh` (a fixed dungeon, the camera
  going once round it, 256 frames): a frame takes 4766 FRT ticks on average
  and 5421 at worst at 160x112, inside the 6000 that 30 frames a second
  allows. Focus mode takes 8224, about 20 frames a second.
- What was learned getting there, each step timed on the MiSTer:
  - It started at 6105. ares said 3307 for the same frames, so ares is for
    checking pictures and counting instructions, not for frame rates.
  - Writing to the framebuffer costs next to nothing, a third wide multiply
    in the floor's pixel loop cost nothing that showed, and keeping the
    depth tables in the cache gained 1.5 percent.
  - Splitting the ray routine so its common path is 1.4K and not 2.3K
    gained 2.8 percent.
  - Sending a ray was the cost nobody was looking at: the code that works
    out its direction ran from ROM, with a divide and two reciprocals done
    by subroutine. From RAM, with the hardware divider, the frame went from
    6105 to 4766.
  - The two SH-2s slow each other: one drawing its half alone took 5303
    where both together took 6105.
  - The cache's two "do not replace" bits (OD and ID in the SH-2's CCR)
    are no help set around a whole frame's rays. With data kept out the
    frame took 13653 where it takes 4765; with code kept out it did not
    finish in the time allowed.
  - `ares-headless --profile` and `tools/profile.py` found it. Measure
    first.
- Creatures are real voxel models here: `tools/bake_models.py --solid` turns
  the same two drawings into 8x8 columns of voxels, and the renderer stands
  them on the cells they are over, turned to face the way they last moved,
  mid-step and mid-hop included. A flash colours the whole model.
- Up on the D-pad is away from the camera: the pad's direction is turned by
  the camera's in quarter turns, and holds on to the one it has.
- The loose voxels of something that died are placed over the finished
  picture where the camera sees them.
- A creature's art may carry a back view (a third group of 8 per layer in
  `models/creatures.txt`); the hero has one. Without it the back half takes
  each layer's commonest material, so there is no face on the back.
- Limits: a creature is solid from the ground up (a gap between
  legs is shadow, and fliers stand on the floor), loose voxels show through
  walls for the moment they last, and far floor shimmers. The build uses
  225K of the 256K of RAM.

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

## Sound

Effects are played by the 68000 on the Mega Drive's PSG (`md_src/sound.c`),
once a frame, so they cost the SH-2s and the picture nothing. An effect is a
short list of per-frame pitch and loudness for one of the PSG's four
channels; effects on different channels play together. The primary SH-2
asks for them by setting bits in COMM10 (`sh_src/sfx.c`).

What is heard comes from what the game already knows: a step when the hero
starts across a cell, a swing or a hit when he strikes, a cry when he is
hurt, a crumble when a monster falls apart, chimes for gold and for
something picked up, a run down the scale on a new level, a fanfare for a
new rank, a blip in menus, and a dirge at the tombstone.

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
| X, Y, Z, MODE | the camera: see "The 3D camera" |

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
- `tools/mbench.sh` times the 3D view on a MiSTer: it builds `make BENCH=7`
  (dungeon 7, the hero standing still, the camera going round him, so every
  run draws the same 256 frames), starts it there, and brings back a
  screenshot with the mean and worst frame on it. The same ROM twice gives
  the same number to within a tick or two. `tools/mbench.sh out.png RES=3`
  times another resolution.
- `tools/profile.py prof.csv` turns `ares-headless --profile prof.csv` into
  time by function; `tools/raysize.sh` lists the size of what runs from RAM.
- Input scripts are `frame,button,value` lines. Boot takes about 2 seconds, so
  press Start no earlier than frame 150.

## Credits and licenses

This project is free: BSD 3-clause, see `LICENSE`. The parts below came from
elsewhere and keep their own terms.

- Boot code, linker scripts and Makefile base: Marsdev 32X skeleton by Andrew
  DeRosier, MIT.
- Build flags, the `.ramtext` section, unsolicited pad publishing and the
  deploy probe are lessons carried over from 32x-builder.
- `md_src/font.s`: generated by `tools/gen_font.py` from font8x8_basic by
  Daniel Hepper, public domain (IBM VGA glyphs).
- `third_party/SH2ZAM/`: fixed-point math by the SH2ZAM contributors, MIT,
  a subset checked byte for byte against upstream. See its `UPSTREAM.md`.
- `rogue/`: Rogue 5.4.4, unmodified (BSD 3-clause, Michael Toy, Ken Arnold,
  Glenn Wichman). See `rogue/UPSTREAM.md`. `rogue/LICENSE.TXT` must ship with
  any ROM release.
