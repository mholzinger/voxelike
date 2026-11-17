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

static int cam_x, cam_y;         /* where the camera is */
static int shown_x, shown_y;     /* where the framebuffer on screen has it */
static int cam_snap = 1;
static int blanked;

/* Time spent bringing the last frame's framebuffer up to date, in FRT
 * ticks, for a debugger or a RAM dump to read. Nothing in the game reads
 * it, so it has to be marked used to survive the link. */
__attribute__((used)) volatile uint16_t game_render_ticks;

/* Close a quarter of the distance each frame, at least one fat pixel and
 * at most four: quick to catch up, soft to settle. */
static int ease(int from, int to) {
	int d = to - from;
	if(d == 0) return from;
	int step = d / 4;
	if(step == 0) step = d > 0 ? 1 : -1;
	return from + clamp(step, -4, 4);
}

/* One step of the dungeon display, one vblank long when it keeps up: fold
 * in Rogue's latest screen, move the camera toward the hero, bring the
 * hidden framebuffer up to date and show it. */
void game_frame(void) {
	if(plat_screen_changed) {
		int room[4];
		plat_screen_changed = 0;
		vox_load_screen(rogue_screen(), rogue_under, rogue_room(room) ? room : 0);
	}
	if(blanked) {
		vox_palette(1);
		blanked = 0;
	}

	int tx = clamp(vox_hero_cx * CELL + CELL / 2 - VIEW_W / 2, CAM_X_MIN, CAM_X_MAX);
	int ty = clamp(vox_hero_cy * CELL + CELL / 2 - VIEW_H / 2 - 4, CAM_Y_MIN, CAM_Y_MAX);
	int first = cam_snap;
	if(cam_snap) {
		cam_x = tx;
		cam_y = ty;
		cam_snap = 0;
	}
	cam_x = ease(cam_x, tx);
	cam_y = ease(cam_y, ty);

	uint16_t t0 = frt_read();
	int drawn = vox_prepare((uint16_t *)&MARS_FRAMEBUFFER, currentFB, cam_x, cam_y);
	game_render_ticks = (uint16_t)(frt_read() - t0);

	/* Nothing new to show: the picture on screen is already this one. */
	if(!drawn && !first && cam_x == shown_x && cam_y == shown_y) {
		wait_vblank();
		return;
	}
	swapBuffers();
	shown_x = cam_x;
	shown_y = cam_y;
}

/* One step of a text page: the dungeon picture goes black behind it. */
void game_blank(void) {
	if(!blanked) {
		vox_palette(0);
		blanked = 1;
	}
	wait_vblank();
}

/* Primary CPU main loop */
int m_main(void) {
	Hw32xInit(MARS_VDP_MODE_256, 0);
	Hw32xDelay(1); // Wait for MD's first VInt to complete

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
