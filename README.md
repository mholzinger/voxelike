# voxelike32x

Rogue, as a 3D voxel world on the Sega 32X. The original game runs
untouched underneath; what you see is a dungeon of voxels and a camera that
circles the hero, pulls back, and tips over into a flat view and out again.

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
make VERSION=0.1.0-alpha.1   # the name on the title screen; without it, dev- and the commit
make ARMOR=7 WEAPON=5   # dress the hero for looking at the art (Rogue's numbers)
make RES=0      # start in 160x112. RES=1 is 320x112 (default), RES=2 is 320x224
make shot       # build, run headless in ares, write shots/latest.png
make shot FRAMES=320 INPUT=keys.csv
make deploy     # scp to a MiSTer (MISTER=root@host)
make ship       # release what is on development: shows it, asks, pushes main
```

Releases: every push to `main` is one. `.github/workflows/build.yml` builds
the ROM with the prebuilt Marsdev toolchain, names it with `VERSION` and the
next number after the newest tag (`0.1.0-alpha.3`), and publishes it with
the commits since the last release as its notes. A push to `development` is
built the same way and kept as a workflow artifact, not released. `make
ship` is the way to push to `main`: it lists what would go out and the
number it would get, asks, and then moves `main` up to `development`.

`make shot` needs the headless ares fork (`ARES=` overrides its path). The
Makefile lists the rest: `PERSP=2` turns the camera by itself for measuring
and `BOT=1` builds a ROM that plays itself.

## Title screen

An amber `@` takes two steps through a chunky ASCII dungeon. The camera
tips toward the horizon, revealing the cubes that make up each glyph.
They gather into **Voxelike**, with the amber cubes finishing the V.
Start skips to the finished title during the animation. Press it again
to fly through the logo. In a 3D build, the camera dives toward the
generated spawn tile and the hero rises through it with his equipped
gear. BOT and BENCH builds open on the finished title and go straight
to gameplay.

`tools/bake_splash.py` uses the same font as the Mega Drive text layer to
build the animation into the ROM. The 32X draws it as 2x2 pixel blocks
with its own palette, then switches to the dungeon palette for play.

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

## What the hero wears and holds

In the 3D view the hero shows his armour and his weapon. `models/gear.txt`
holds a distinct body for each of Rogue's eight armours and a model for all
nine wieldable weapons, including an arrow, a dart and a shuriken. The port
tells the renderer what the hero has equipped, and `persp.c` adds the weapon
to the body wherever it stands
clear of it. A voxel column is solid from the ground up, so a weapon stands
on the floor by his right hand or has shadow under it. `make ARMOR=7
WEAPON=5` dresses him so whatever he owns, for looking at the art. The flat
view still shows him plain.

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

The camera keeps its heading as the hero walks through rooms and passages.
If a wall hides him, it waits for a pause in movement and swings toward a
clearer view from one of the four map directions. A new direction press
pauses the swing. The camera checks visibility again from the new tile
before resuming. Manual camera turns hold off recovery for three seconds.

Up on the D-pad is the map direction nearest to away from the camera.
A new press uses the angle visible when it begins. A held direction
keeps its meaning through a swing, and one quick tap can queue during
a walking or attack animation.

| Pad | Camera |
|---|---|
| X, Z | swing left and right around the hero |
| Y, tapped | swing behind the hero, so up on the D-pad is the way he faces |
| Y, tapped twice | swing back to north |
| Y, held | tip the view over into the flat one, or back out of it |
| MODE, held | pull back to show more, until it is let go |
| MODE, tapped | next resolution: 160x112, 320x112, 320x224 |

The resolutions are there to be compared by playing. A voxel at the hero
covers the same 2x2 screen pixels in all three; what changes is how many
pixels are worked out and how many are copies. In one lit room in ares they
ran at 30, 20 and 15 frames a second. Each has its own compiled copy of the
ray routine, so nothing is decided per pixel. Black is written as CRAM
index 1, not 0: the 32X drops a zero written to the framebuffer as a byte.

The game starts at 320×112. It works out all 320 columns and repeats each
row on two scanlines. Use `make RES=0` for 160×112 or `make RES=2` for native
320×224.

Floor tiles and wall caps use 8×8 material maps in `models/terrain.txt`.
The maps adapt Kenney textures to the dungeon palette and run through
the same asset baker as the creature models. Both renderers
draw the same terrain geometry and textures.

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
  the columns' slopes from a table made when the focal length or resolution
  changes. Zooming by moving the eye reuses the slopes and scanline
  projection, and rebuilds depth entries only across the range rays can hit.
- Normal builds keep the picture on screen when the camera, terrain,
  creatures and particles are still. STATS and BENCH builds continue
  drawing frames for timing.
- Each cell's facts sit in one 16 byte record, a cache line, and a floor's
  colours in 64 bytes.
- Measured on a MiSTer with `tools/mbench.sh` (a fixed dungeon, the camera
  going once round it, 256 frames): a frame takes 4298 FRT ticks on average
  and 4894 at worst at 160x112, inside the 6000 that 30 frames a second
  allows.
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
  - The compiler's pixel loops had waste in them (a colour looked up
    twice, constants loaded every pixel) that went away when the loop was
    written as a run with the pixel in a variable: 4766 to 4529 at
    160x112, with no assembly.
  - The SH-2 shifts one place at a time, so the compiler shifts a signed
    number right by calling a routine, and that routine is in ROM. Two
    table lookups a cell did it. Shifting an unsigned number there, which
    is done in line, gained 4 to 5 percent.
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

- A bump attack uses the wielded weapon’s own wind-up, contact and
  follow-through. Heavy weapons lift or sweep, and daggers and spears
  thrust.
- A monster that strikes at the hero jabs back a moment later. The linker's
  `--wrap=attack` routes Rogue's one call to `attack()` through
  `port/rogue_port.c`, which notes who struck.
- Whoever takes a hit flashes white. In 3D, a neighboring enemy brings
  the camera to finishing-shot distance and frames both fighters. It
  eases back when they separate.
- A monster that is gone from Rogue's monster list died, and its sprite is
  thrown apart as loose voxels that fall, lie a moment and vanish. In 3D,
  nearby defeats get a short shot that frames both fighters, moves closer,
  then shows a brief red spray and the opponent's breakup before returning
  to gameplay. A small stain stays on the death tile for the current floor.

## Player death

The camera pushes in to make the hero about 50 percent larger. The fatal
blow sends out a spray of red voxels while the hero is still standing.
A beat later, the equipped hero breaks into chunks that keep their model
colours. The camera
pulls back while the dungeon fades to black, then Rogue’s headstone shows
the cause of death and gold. Press Start on the headstone to begin again.

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

Rogue is turn based, and so is this: nothing moves until the hero does. A
turn is shown over 8 frames, so holding a direction walks smoothly and
monsters move as he moves. C rests a turn. (The port can hand Rogue a rest
command by itself when the pad is left alone, so that monsters keep moving:
set `IDLE_TURN` to a number of frames. Those turns run with the hunger
daemon switched off. It is off because the hero died of it.)
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
Item names in the controller lists omit Rogue's inventory letters. When a
pickup offers a shortcut, A closes the notice and C uses the item shown.

## Text

Text is drawn on the Mega Drive layer, 40 columns of an 8x8 font, in front of
the 32X picture. `sh_src/rogue_plat.c` decides what Rogue is waiting for from
where it left the cursor and what the message line says, then shows the
matching screen. Rogue's 80 column message line wraps into a framed dialogue box,
and its stats feed a framed HUD with a health meter and pixel icons for
depth, gold, strength, armor and experience. The health meter turns red
at a quarter health. Controller lists scroll when the choices would
otherwise overlap the status panel. Continuation prompts appear in the
box footer with controller labels.

When Rogue asks which object, the list offers only what fits the question:
potions to quaff, scrolls to read, and for a scroll that identifies one kind
of thing, that kind. Anything can be wielded or thrown, so those two list
the whole pack. The port learns what Rogue wants by having the linker route
`get_item` through it (`--wrap`), as it does `attack`. Before this an identify scroll listed everything, refused all but
one line of it, and could not be backed out of.

Questions a keyboard answers with one key are lists too. The table `asks`
in `sh_src/rogue_plat.c` holds the words that give a question away and a
key for each line: Discoveries asks which kind (everything, potions,
scrolls, rings, wands and staffs) and a ring asks which hand. "What do you
want to call it?" offers a few short names (`guesses`, in the same file)
and types the one chosen. A question that is in neither gets A for yes, C
for no and B to back out.

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
  the same number to within a tick or two. `tools/mbench.sh out.png RES=0`
  times another resolution. Without RES it measures 320×112.
- `tools/profile.py prof.csv` turns `ares-headless --profile prof.csv` into
  time by function; `tools/raysize.sh` lists the size of what runs from RAM.
- Input scripts are `frame,button,value` lines. For a normal build, press Start at
  frame 470 or later to enter from the finished title, or press and
  release Start once to skip the animation, then press again to play.
  For BOT or BENCH builds, press Start at frame 150.

## Credits and licenses

This project is free: BSD 3-clause, see `LICENSE`. The parts below came from
elsewhere and keep their own terms.

- Boot code, linker scripts and Makefile base: Marsdev 32X skeleton by Andrew
  DeRosier, MIT.
- Build flags, the `.ramtext` section, unsolicited pad publishing and the
  deploy probe are lessons carried over from 32x-builder.
- The floor and wall-cap material maps adapt `floor_stone.png`
  and `wall_brick_sand_center.png` from Kenney's Retro Textures Fantasy (CC0).
- `md_src/font.s`: generated by `tools/gen_font.py` from font8x8_basic by
  Daniel Hepper, public domain (IBM VGA glyphs).
- `third_party/SH2ZAM/`: fixed-point math by the SH2ZAM contributors, MIT,
  a subset checked byte for byte against upstream. See its `UPSTREAM.md`.
- `rogue/`: Rogue 5.4.4, unmodified (BSD 3-clause, Michael Toy, Ken Arnold,
  Glenn Wichman). See `rogue/UPSTREAM.md`. `rogue/LICENSE.TXT` must ship with
  any ROM release.

## Inspiration

- 3D Dot Game Heroes (Silicon Studio, 2009), for the idea of an old flat
  game stood up in voxels. None of its art or assets are used here.
