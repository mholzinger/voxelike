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
SHLDFLAGS  = -T sh_src/mars.ld -nostdlib

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

# make BOT=1 builds a ROM that plays itself, for tools/botrun.py. The stamp
# makes switching between the two kinds of build recompile what differs.
ifdef BOT
RGFLAGS   += -DBOT
SHCCFLAGS += -DBOT
endif
.DEFAULT_GOAL := all
BOTSTAMP := $(ROMDIR)/.bot-$(if $(BOT),on,off)
$(BOTSTAMP):
	@mkdir -p $(ROMDIR)
	@rm -f $(ROMDIR)/.bot-*
	@touch $@
port/bot.o sh_src/rogue_plat.o: $(BOTSTAMP)

# Headless emulator used by 'make shot' (ares fork in ~/src/ares-debug)
ARES   ?= $(HOME)/src/ares-debug/build_macos/headless-ui/Release/ares-headless
FRAMES ?= 180

MISTER ?= root@mister.office.local

.PHONY: all release debug shot deploy clean

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

$(TARGET).elf: $(SHOBJS) | $(ROMDIR)
	$(SHCC) $(SHLDFLAGS) $^ -o $@ $(SHLIBS)

# Materials and creature sprites, baked from the art in models/
sh_src/baked.h: models/materials.txt models/creatures.txt tools/bake_models.py
	@echo "BAKE $@"
	@python3 tools/bake_models.py --header $@

sh_src/voxel.o: sh_src/baked.h

# mars_start.s embeds the 68K program
sh_src/mars_start.o: $(MDTARGET).bin

sh_src/%.o: sh_src/%.s
	@echo "SHAS $<"
	@$(SHAS) $(SHASFLAGS) $< -o $@

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

clean:
	rm -f $(MDOBJS) $(SHOBJS) $(MDOBJS:.o=.d) $(SHOBJS:.o=.d)
	rm -f $(MDTARGET).bin $(MDTARGET).elf $(MDTARGET).lst
	rm -f $(TARGET).32x $(TARGET).elf $(TARGET).lst
	rm -f sh_src/baked.h

-include $(MDOBJS:.o=.d) $(SHOBJS:.o=.d)
