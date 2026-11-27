# voxelike32x - Rogue in a voxel engine for the Sega 32X
# Based on the Marsdev 32X skeleton Makefile. Both m68k and SH-2 code is built;
# the usual variables are split into "MD" and "SH" versions.

# Default path, can be overridden by setting MARSDEV before calling make
MARSDEV ?= ${HOME}/mars
MDBIN    = $(MARSDEV)/m68k-elf/bin
SHBIN    = $(MARSDEV)/sh-elf/bin

ROMDIR  := rom
TARGET  ?= $(ROMDIR)/voxelike
MDTARGET = $(ROMDIR)/md_start

# m68k GCC and Binutils
MDCC   = $(MDBIN)/m68k-elf-gcc
MDNM   = $(MDBIN)/m68k-elf-nm
MDOBJC = $(MDBIN)/m68k-elf-objcopy

# sh2 GCC and Binutils
SHCC   = $(SHBIN)/sh-elf-gcc
SHAS   = $(SHBIN)/sh-elf-as
SHNM   = $(SHBIN)/sh-elf-nm
SHOBJC = $(SHBIN)/sh-elf-objcopy

# Some files needed are in a versioned directory
MDCC_VER := $(shell $(MDCC) -dumpversion)
SHCC_VER := $(shell $(SHCC) -dumpversion)

# Need the LTO plugin so NM can dump our symbol table
MDPLUGIN = $(MARSDEV)/m68k-elf/libexec/gcc/m68k-elf/$(MDCC_VER)/liblto_plugin.so
SHPLUGIN = $(MARSDEV)/sh-elf/libexec/gcc/sh-elf/$(SHCC_VER)/liblto_plugin.so

MDLIBS   = -L$(MARSDEV)/m68k-elf/lib/gcc/m68k-elf/$(MDCC_VER) -lgcc
SHLIBS   = -L$(MARSDEV)/sh-elf/lib/gcc/sh-elf/$(SHCC_VER) -lgcc

MDCCFLAGS  = -m68000 -mshort -Wall -Wextra -pedantic -std=c99 -ffreestanding
SHCCFLAGS  = -m2 -mb -Wall -Wextra -pedantic -std=c99 -ffreestanding

MDASFLAGS  = -x assembler-with-cpp -Imd_src -m68000 -Wa,--register-prefix-optional
SHASFLAGS  = -I$(ROMDIR) --small

MDLDFLAGS  = -T md_src/md.ld -nostdlib
# --wrap=attack, death, get_item: see __wrap_attack and __wrap_get_item in port/rogue_port.c
SHLDFLAGS  = -T sh_src/mars.ld -nostdlib -Wl,--wrap=attack -Wl,--wrap=death -Wl,--wrap=get_item

# Extra options set by debug or release target
MDEXTRA =
SHEXTRA =

MDSS    = $(wildcard md_src/*.s)
MDCS    = $(wildcard md_src/*.c)
MDOBJS  = $(MDSS:.s=.o) $(MDCS:.c=.o)

SHSS    = $(wildcard sh_src/*.s)
SHCS    = $(wildcard sh_src/*.c)
SHOBJS  = $(SHSS:.s=.o) $(SHCS:.c=.o)

# Rogue 5.4.4 (vendored, unmodified) and the layer that stands in for Unix.
# Old C, so its own dialect and no LTO; -w because the 1980s code is noisy.
RGCS    = $(wildcard rogue/*.c)
PTCS    = $(wildcard port/*.c)
SHOBJS += $(RGCS:.c=.o) $(PTCS:.c=.o)
RGFLAGS = -m2 -mb -O2 -fomit-frame-pointer -ffreestanding -Iport/include -Irogue -Iport

# Kinds of build. The stamps make switching between them recompile what
# differs.
#   make            the game: the 3D camera, with the flat view inside it
#   make FLAT=1     the flat view alone, with no 3D renderer (sh_src/persp.c)
#   make RES=n      start in resolution n: 0 160x112, 1 320x112, 2 320x224
#                   (320x112 if not given)
#   make STATS=1    show frames a second and the time to draw one
#   make PERSP=2    turn the camera by itself, for measuring
#   make BOT=1      a ROM that plays itself, for tools/botrun.py
#   make VERSION=v the name on the title screen (a release's; otherwise the
#                   commit's)
#   make ARMOR=n WEAPON=n   dress the hero by Rogue's numbers whatever he has
#                   (armor 0 leather .. 7 plate, weapon 0 mace .. 8 spear, -1 none)
#   make BENCH=n    the same picture every run, for timing: dungeon n, the
#                   hero standing in it, and the camera going round him. Each
#                   turn of the camera is the same 256 frames, and the yellow
#                   line gives the mean and worst time to draw one of them
#   make FULL_VIEW=1   rays cross the whole map during play. Without it they
#                   stop two cells outside the room the hero is in (or two
#                   cells round him in a passage or a dark room)
#   make RENDER_TEST=n   timing by a bot in dungeon n: up to 32 frames each
#                   in rooms, passages and fights, every one drawn twice,
#                   without the creatures and with them. The yellow lines
#                   give the mean of each. Add FULL_VIEW=1 to compare
PERSP ?= 1
ifdef RENDER_TEST
override BOT := 1
override STATS := 1
SHCCFLAGS += -DRENDER_TEST=$(RENDER_TEST)
RGFLAGS   += -DRENDER_TEST=$(RENDER_TEST)
endif
ifdef FOLD_CHECK
SHCCFLAGS += -DFOLD_CHECK
endif
ifdef FULL_VIEW
SHCCFLAGS += -DFULL_VIEW
endif
ifdef FLAT
override PERSP :=
endif
# Rooms, passages and dark rooms are drawn as polygons in the rays' bitmaps
# (sh_src/poly.c), and the rays draw only the scripted scenes. make POLY=1
# is the polygons in flat colours, and make POLY= is the rays for everything.
POLY ?= 2
ifeq ($(PERSP),)
override POLY :=
endif
ifneq ($(POLY),)
SHCCFLAGS += -DPOLY=$(POLY) $(if $(POLY_OFF),-DPOLY_OFF=$(POLY_OFF)) $(if $(POLY_PROF),-DPOLY_PROF)
endif
# make POLYBENCH=1: a room of flat polygons painted by the fill hardware,
# timed, in rom/polybench.32x. POLYBENCH=2 draws on one SH-2. EYE=n is the
# camera's height and distance, 128 (near, the default) to 208 (pulled back).
# ACTORS=n stands n creatures in the room, 5 unless given, up to 8.
# POLYBENCH=3 has each SH-2 draw its side of column SPLIT (160 unless given),
# writing spans itself; SOFT=1 writes spans that way with one SH-2 drawing.
# A creature goes to pieces every 48 frames; SHATTER=0 leaves that out.
# TEX=1 with POLYBENCH=3 gives the floor a bitmap, sampled once a column;
# TEX=2 samples it once a pixel.
ifdef POLYBENCH
TARGET := $(ROMDIR)/polybench
SHCCFLAGS += -DPOLYBENCH=$(POLYBENCH) -DPOLYBENCH_EYE=$(if $(EYE),$(EYE),128) $(if $(COUNT),-DPOLYBENCH_COUNT) $(if $(ACTORS),-DPOLYBENCH_ACTORS=$(ACTORS)) $(if $(SPLIT),-DPOLYBENCH_SPLIT=$(SPLIT)) $(if $(SHATTER),-DPOLYBENCH_SHATTER=$(SHATTER)) $(if $(SOFT),-DPOLYBENCH_SOFT) $(if $(TEX),-DPOLYBENCH_TEX=$(TEX))
endif
# make HALL_NEAR=1: in a passage the rays stop two cells from the hero, as
# they did, and not at the ends of the passage.
ifdef HALL_NEAR
SHCCFLAGS += -DHALL_NEAR
endif
# TERRAIN_ROM=1: the terrain models stay in the ROM and are read from it.
ifdef TERRAIN_ROM
SHCCFLAGS += -DTERRAIN_ROM
endif
# FRINGE_ALL=1: rock lines every passage found, not only those near the hero.
ifdef FRINGE_ALL
SHCCFLAGS += -DFRINGE_ALL
endif
# HALL_REACH=n: how many cells of a passage the rays see either side of the
# hero, before the border of two more.
ifdef HALL_REACH
SHCCFLAGS += -DHALL_REACH=$(HALL_REACH)
endif
ifdef HALL_POLY
SHCCFLAGS += -DHALL_POLY=$(HALL_POLY)
endif
ifdef BENCH
override PERSP := 2
override STATS := 1
SHCCFLAGS += -DBENCH=$(BENCH)
endif
ifdef ARMOR
SHCCFLAGS += -DSHOW_ARMOR=$(ARMOR)
endif
ifdef WEAPON
SHCCFLAGS += -DSHOW_WEAPON=$(WEAPON)
endif
# make SNAP=1: the renderer is shown the level tools/snap.py took out of a
# save state, to time one view before and after a change. SNAP=2: the level
# tools/testlevel.py makes, with every case there is to draw, the hero at
# each of its stations in turn: tools/snapbench.py times each.
# It drives itself and ends on a table of each station's median and worst
# ticks, for a machine with no debugger. TAG=name puts a name on the table.
ifdef SNAP
override STATS := 1
RGFLAGS   += -DSNAP=$(SNAP)
SHCCFLAGS += -DSNAP=$(SNAP) $(if $(TAG),-DSNAP_TAG=$(TAG))
endif
ifdef BOT
RGFLAGS   += -DBOT
SHCCFLAGS += -DBOT
endif
ifneq ($(PERSP),)
SHCCFLAGS += -DPERSP=$(PERSP) $(if $(RES),-DPERSP_RES=$(RES)) $(if $(STATS),-DSTATS)
endif
ifneq ($(RES),)
ifeq ($(filter $(RES),0 1 2),)
$(error RES must be 0 (160x112), 1 (320x112), or 2 (320x224))
endif
endif
.DEFAULT_GOAL := all
BOTSTAMP := $(ROMDIR)/.bot-$(if $(BOT),on,off)$(if $(RENDER_TEST),-test$(RENDER_TEST))$(if $(SNAP),-snap$(SNAP))$(if $(TAG),-tag$(TAG))
PERSPSTAMP := $(ROMDIR)/.persp-$(if $(PERSP),$(PERSP),off)-runtime-camera$(if $(FULL_VIEW),-full)$(if $(FOLD_CHECK),-foldcheck)$(if $(RENDER_TEST),-test$(RENDER_TEST))$(if $(RES),-res$(RES))$(if $(STATS),-stats)$(if $(BENCH),-bench$(BENCH))$(if $(ARMOR),-armor$(ARMOR))$(if $(WEAPON),-weapon$(WEAPON))$(if $(POLY),-poly$(POLY))$(if $(POLY_OFF),-off$(POLY_OFF))$(if $(POLY_PROF),-prof)$(if $(SNAP),-snap$(SNAP))$(if $(TAG),-tag$(TAG))$(if $(HALL_NEAR),-hallnear)$(if $(TERRAIN_ROM),-terrainrom)$(if $(FRINGE_ALL),-fringeall)$(if $(HALL_REACH),-reach$(HALL_REACH))$(if $(HALL_POLY),-hallpoly$(HALL_POLY))$(if $(POLYBENCH),-polybench$(POLYBENCH)-$(EYE)-$(COUNT)-$(ACTORS)-$(SPLIT)-$(SOFT)-$(SHATTER)-$(TEX))
# File times here are whole seconds. A stamp waits one out before it is
# made, so that it is strictly newer than everything the last build left,
# and everything that depends on the kind of build is made again.
BOTOBJS   = port/bot.o port/rogue_port.o sh_src/rogue_plat.o sh_src/m_main.o
PERSPOBJS = sh_src/rogue_plat.o sh_src/m_main.o sh_src/persp.o sh_src/s_main.o sh_src/voxel.o sh_src/polybench.o sh_src/poly.o
$(BOTSTAMP):
	@mkdir -p $(ROMDIR)
	@rm -f $(ROMDIR)/.bot-*
	@sleep 1
	@touch $@
$(PERSPSTAMP):
	@mkdir -p $(ROMDIR)
	@rm -f $(ROMDIR)/.persp-*
	@sleep 1
	@touch $@
$(BOTOBJS): $(BOTSTAMP)

# What the title screen calls this build. A release is made with
# make VERSION=0.1.0-alpha.3; anything else names the commit it was built
# from. The header is only written when the name changes.
VERSION ?= dev-$(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
sh_src/version.h: FORCE
	@echo '#define VERSION "$(VERSION)"' | cmp -s - $@ || \
		{ sleep 1; echo '#define VERSION "$(VERSION)"' > $@; echo "VERSION $(VERSION)"; }
sh_src/m_main.o: sh_src/version.h
.PHONY: FORCE
FORCE:
$(PERSPOBJS): $(PERSPSTAMP)

# SH2ZAM fixed-point math (vendored subset, MIT): its own dialect.
SH2ZDIR  = third_party/SH2ZAM
SHOBJS  += $(SH2ZDIR)/src/sh2zam.o $(SH2ZDIR)/src/sh2zam_trig.o
SHCCFLAGS += -I$(SH2ZDIR)/include

# Headless emulator used by 'make shot' (ares fork in ~/src/ares-debug)
ARES   ?= $(HOME)/src/ares-debug/build_macos/headless-ui/Release/ares-headless
FRAMES ?= 180

# Where make deploy copies the ROM: set MISTER in the environment or on the
# command line.
MISTER ?= root@mister.local

.PHONY: all release debug shot deploy ship clean snaproms

all: release

release: MDEXTRA  = -O2 -fomit-frame-pointer -flto -fuse-linker-plugin
# -fno-thread-jumps: sh-elf GCC 15.2.0 at O2+LTO has jump-threaded a main-loop
# frame counter out of existence in 32x-builder (bisected there 2026-08-21).
release: SHEXTRA  = -O2 -fomit-frame-pointer -flto -fuse-linker-plugin -fno-thread-jumps
release: $(TARGET).32x $(TARGET).lst $(MDTARGET).lst

debug: MDEXTRA = -g -Og -DDEBUG
debug: SHEXTRA = -g -Og -DDEBUG
debug: $(TARGET).32x $(TARGET).lst $(MDTARGET).lst

# Build, run headless for FRAMES frames, save the last frame to shots/latest.png
shot: release
	@mkdir -p shots
	$(ARES) --frames $(FRAMES) $(if $(INPUT),--input $(INPUT)) \
		--screenshot $$(( $(FRAMES) - 1 )):shots/latest.png $(TARGET).32x
	@sips -z 448 640 shots/latest.png >/dev/null

# Probes usb0 then usb1 via /proc/mounts so an ejected drive's empty mount
# directory is never mistaken for a live one.
deploy: release
	@DIR=$$(ssh $(MISTER) 'for u in usb0 usb1; do \
		grep -q " /media/$$u " /proc/mounts && [ -d /media/$$u/games/S32X ] && \
		{ echo /media/$$u/games/S32X; break; }; done'); \
	test -n "$$DIR" || { echo "no mounted /media/usbN/games/S32X on $(MISTER)"; exit 1; }; \
	echo "==> Copying $(TARGET).32x to $(MISTER):$$DIR/" && \
	scp $(TARGET).32x $(MISTER):$$DIR/voxelike.32x

$(ROMDIR):
	@mkdir -p $@

# Symbol listings for both CPUs
$(MDTARGET).lst: $(MDTARGET).elf
	@$(MDNM) --plugin=$(MDPLUGIN) -n $< > $@

$(TARGET).lst: $(TARGET).elf
	@$(SHNM) --plugin=$(SHPLUGIN) -n $< > $@

# m68k stuff
$(MDTARGET).bin: $(MDTARGET).elf
	@echo "Stripping ELF header from M68K program"
	@$(MDOBJC) -O binary $< $@

$(MDTARGET).elf: $(MDOBJS) | $(ROMDIR)
	$(MDCC) $(MDLDFLAGS) $^ -o $@ $(MDLIBS)

md_src/%.o: md_src/%.s
	@echo "MDAS $<"
	@$(MDCC) $(MDASFLAGS) -c $< -o $@

md_src/%.o: md_src/%.c
	@echo "MDCC $<"
	@$(MDCC) $(MDCCFLAGS) $(MDEXTRA) -MMD -MP -c $< -o $@

# sh2 stuff
$(TARGET).32x: $(TARGET).elf
	@echo "Stripping ELF header from SH-2 program"
	@$(SHOBJC) -O binary $< $(ROMDIR)/temp.32x
	@dd if=$(ROMDIR)/temp.32x of=$@ bs=8192 conv=sync 2>/dev/null
	@rm -f $(ROMDIR)/temp.32x

$(TARGET).elf: $(SHOBJS) Makefile sh_src/mars.ld | $(ROMDIR)
	$(SHCC) $(SHLDFLAGS) $(SHOBJS) -o $@ $(SHLIBS)

# Materials and creature sprites, baked from the art in models/
sh_src/baked.h: models/materials.txt models/creatures.txt models/terrain.txt tools/bake_models.py
	@echo "BAKE $@"
	@python3 tools/bake_models.py --header $@

sh_src/voxel.o: sh_src/baked.h

# The terrain models, made here and kept in the ROM: they were 39K of the
# SH-2's memory when the game made them as it started.
HOSTCC ?= cc
$(ROMDIR)/bake_terrain: tools/bake_terrain.c sh_src/terrain.h sh_src/voxel.h sh_src/baked.h | $(ROMDIR)
	@echo "HOSTCC $<"
	@$(HOSTCC) -O1 -w -iquote sh_src -o $@ $<

sh_src/baked_terrain.h: $(ROMDIR)/bake_terrain
	@echo "BAKE $@"
	@$(ROMDIR)/bake_terrain > $@

sh_src/voxel.o: sh_src/baked_terrain.h

sh_src/baked3d.h: models/materials.txt models/creatures.txt models/gear.txt tools/bake_models.py
	@echo "BAKE $@"
	@python3 tools/bake_models.py --solid $@

sh_src/persp.o: sh_src/baked3d.h

sh_src/splash_frames.h: tools/bake_splash.py md_src/font.s
	@echo "BAKE $@"
	@python3 tools/bake_splash.py md_src/font.s $@

sh_src/splash.o: sh_src/splash_frames.h

# mars_start.s embeds the 68K program
sh_src/mars_start.o: $(MDTARGET).bin

sh_src/%.o: sh_src/%.s
	@echo "SHAS $<"
	@$(SHAS) $(SHASFLAGS) $< -o $@

$(SH2ZDIR)/src/%.o: $(SH2ZDIR)/src/%.c
	@echo "SHCC $<"
	@$(SHCC) $(filter-out -std=c99 -pedantic,$(SHCCFLAGS)) -std=c11 $(SHEXTRA) -DSH2Z_REQUIRE_SH2=1 -MMD -MP -c $< -o $@

rogue/%.o: rogue/%.c
	@echo "RGCC $<"
	@$(SHCC) $(RGFLAGS) -std=gnu89 -w -MMD -MP -c $< -o $@

port/rogue_port.o: port/rogue_port.c
	@echo "RGCC $<"
	@$(SHCC) $(RGFLAGS) -std=gnu89 -w -MMD -MP -c $< -o $@

port/%.o: port/%.c
	@echo "RGCC $<"
	@$(SHCC) $(RGFLAGS) -std=gnu99 -Wall -Wextra -fno-builtin -MMD -MP -c $< -o $@

sh_src/%.o: sh_src/%.c
	@echo "SHCC $<"
	@$(SHCC) $(SHCCFLAGS) $(SHEXTRA) -MMD -MP -c $< -o $@

# Release what is on development: see tools/ship.sh
# The two ROMs to time on a machine: the self-driving test level with the
# terrain models in memory and with them in the ROM. Each ends on a table
# that says which it is. The game's own ROM is put back afterwards.
snaproms:
	$(MAKE) SNAP=2 POLY=2 TAG=RAM
	cp $(ROMDIR)/voxelike.32x $(ROMDIR)/snap-ram.32x
	$(MAKE) SNAP=2 POLY=2 TERRAIN_ROM=1
	cp $(ROMDIR)/voxelike.32x $(ROMDIR)/snap-rom.32x
	$(MAKE) POLY=2

ship:
	@YES=$(YES) tools/ship.sh

clean:
	rm -f $(MDOBJS) $(SHOBJS) $(MDOBJS:.o=.d) $(SHOBJS:.o=.d)
	rm -f $(MDTARGET).bin $(MDTARGET).elf $(MDTARGET).lst
	rm -f $(TARGET).32x $(TARGET).elf $(TARGET).lst
	rm -f sh_src/baked.h sh_src/baked_terrain.h sh_src/version.h sh_src/splash_frames.h

-include $(MDOBJS:.o=.d) $(SHOBJS:.o=.d)
