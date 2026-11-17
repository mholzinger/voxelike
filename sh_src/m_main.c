#include "mars.h"
#include "voxel.h"
#include "text.h"
#include "../port/plat.h"

uint32_t lastTick = 0;
uint16_t currentFB = 0;

/* Swap backbuffer and wait for next tick from MD */
static void swapBuffers(void) {
	while(lastTick == MARS_SYS_COMM12);
	MARS_VDP_FBCTL = currentFB ^ 1;
	while((MARS_VDP_FBCTL & MARS_VDP_FS) == currentFB);
	currentFB ^= 1;
	lastTick = MARS_SYS_COMM12;
}

static void wait_vblank(void) {
	uint32_t t = MARS_SYS_COMM12;
	while(MARS_SYS_COMM12 == t);
}

/* Scanlines 2k and 2k+1 both show framebuffer row k: free vertical doubling,
 * so the renderer only ever touches VIEW_H rows. */
static void doubled_line_table(void) {
	volatile uint16_t *fb = &MARS_FRAMEBUFFER;
	for(int i = 0; i < 224; i++) fb[i] = 0x100 + (i >> 1) * 160;
}

/* FRT at Phi/128: one tick is ~5.56 us, a 60 fps frame is ~3000 ticks. */
static void frt_init(void) {
	SH2_FRT_TIER  = 0x01;
	SH2_FRT_TCR   = 0x02;
	SH2_FRT_FTCSR = 0;
}

static inline uint16_t frt_read(void) {
	uint8_t hi = SH2_FRT_FRCH;
	uint8_t lo = SH2_FRT_FRCL;
	return ((uint16_t)hi << 8) | lo;
}

static int clamp(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

extern int plat_screen_changed;

static int cam_x, cam_y;
static int cam_snap = 1;
static int blanked;

/* Render time of the last drawn frame in FRT ticks, for a debugger or a
 * RAM dump to read. */
uint16_t game_render_ticks;

/* One step of the dungeon display: fold in Rogue's latest screen, ease the
 * camera toward the hero and redraw. When nothing moved there is nothing to
 * draw, so the step just waits out a vblank and the last picture stays up. */
void game_frame(void) {
	int dirty = plat_screen_changed || blanked;
	if(plat_screen_changed) {
		plat_screen_changed = 0;
		vox_load_screen(rogue_screen(), rogue_under);
	}
	blanked = 0;

	int tx = clamp(vox_hero_cx * CELL + CELL / 2 - VIEW_W / 2, 0, MAP_W * CELL - VIEW_W);
	int ty = clamp(vox_hero_cy * CELL + CELL / 2 - VIEW_H / 2 - 4, -16, MAP_H * CELL - VIEW_H + 8);
	if(cam_snap) {
		cam_x = tx;
		cam_y = ty;
		cam_snap = 0;
		dirty = 1;
	}
	if(cam_x != tx || cam_y != ty) {
		/* two voxels a frame */
		cam_x += clamp(tx - cam_x, -2, 2);
		cam_y += clamp(ty - cam_y, -2, 2);
		dirty = 1;
	}

	if(!dirty) {
		wait_vblank();
		return;
	}

	uint16_t t0 = frt_read();
	vox_render((uint16_t *)&MARS_FRAMEBUFFER + 0x100, cam_x, cam_y);
	game_render_ticks = (uint16_t)(frt_read() - t0);

	swapBuffers();
}

/* One step of a text page: the dungeon picture goes black behind it. */
void game_blank(void) {
	if(blanked) {
		wait_vblank();
		return;
	}
	volatile uint16_t *fb = &MARS_FRAMEBUFFER + 0x100;
	for(int i = 0; i < VIEW_H * 160; i++) fb[i] = 0;
	swapBuffers();
	blanked = 1;
}

/* Primary CPU main loop */
int m_main(void) {
	Hw32xInit(MARS_VDP_MODE_256, 0);
	Hw32xDelay(1); // Wait for MD's first VInt to complete

	doubled_line_table();
	swapBuffers();
	doubled_line_table();

	frt_init();
	vox_init();

	/* The moment Start goes down seeds the dungeon. */
	text_begin();
	text_put(14, 10, "VOXELIKE32X", -1, TEXT_YELLOW, 0);
	text_put(14, 14, "PRESS START", -1, TEXT_WHITE, 0);
	text_commit();
	while(!(MARS_SYS_COMM8 & SEGA_CTRL_START));

	rogue_start();
	rogue_play();   /* draws and reads the pad from inside plat_getkey */
	return 0;
}
