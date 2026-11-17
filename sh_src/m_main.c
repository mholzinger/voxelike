#include "mars.h"
#include "voxel.h"
#include "text.h"
#include "persp.h"
#include "sfx.h"
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
extern int plat_step_frames;

static int cam_x, cam_y;         /* where the camera is */
static int shown_x, shown_y;     /* where the framebuffer on screen has it */
static int cam_snap = 1;
static int blanked;
static uint32_t anim_tick;       /* vblank the animation has been run up to */

/* Time spent bringing the last frame's framebuffer up to date, in FRT
 * ticks, for a debugger or a RAM dump to read. Nothing in the game reads
 * it, so it has to be marked used to survive the link. */
__attribute__((used)) volatile uint16_t game_render_ticks;

#ifdef PERSP
uint16_t persp_yaw = 0xC000;

/* For the HUD of a measuring build. */
uint16_t perf_fps, perf_mean, perf_worst;

/* The camera's buttons.
 *   X, Z            swing it round the hero
 *   Y tapped        swing behind the hero, so up on the pad is the way he faces
 *   Y tapped twice  swing back to north
 *   Y held          tip the view over into the flat one, or back out of it
 *   MODE held       pull back to show more, for as long as it is held
 *   MODE tapped     next picture resolution: 160x112, 320x112, 320x224, and
 *                   fine around the hero with the rest coarse
 * The flat view is the fixed renderer's own, at its full frame rate; the
 * perspective one flattens until the two pictures meet and then hands over.
 * Turning the camera from the flat view tips it back first. */
#define Y_TAP      15      /* vblanks: a press shorter than this is a tap */
#define Y_AGAIN    30      /* vblanks: a second tap this soon makes it a double */
#define SWING      0x500   /* turn per frame when the camera moves by itself */
#define TIP        8       /* of 256 per frame: tipping over takes half a second */
#define NORTH      0xC000

static int cam_pull;       /* 0 close in .. 256 pulled back */
static int cam_flat;       /* 0 full perspective .. 256 none */
#ifndef PERSP_RES
#define PERSP_RES PERSP_RES_COARSE
#endif
int cam_res = PERSP_RES;   /* PERSP_RES_x; make RES=n picks the one to start in */
uint32_t cam_res_at;       /* vblank it was last changed on, 0 for never */
static int flat_wanted;

static void camera_buttons(void) {
	static const uint16_t behind[4] = { 0x4000, 0x8000, NORTH, 0x0000 };
	static uint16_t prev, target;
	static uint32_t y_since, tapped_at, mode_since;
	static int swinging, tapped, tipped;
	uint16_t pad = MARS_SYS_COMM8;
	uint32_t now = MARS_SYS_COMM12;

	if(pad & (SEGA_CTRL_X | SEGA_CTRL_Z)) {
		swinging = 0;
		flat_wanted = 0;
	}
	if(pad & SEGA_CTRL_X) persp_yaw -= 0x200;
	if(pad & SEGA_CTRL_Z) persp_yaw += 0x200;

	if((pad & SEGA_CTRL_Y) && !(prev & SEGA_CTRL_Y)) {
		y_since = now;
		tipped = 0;
	}
	if(!(pad & SEGA_CTRL_Y) && (prev & SEGA_CTRL_Y) && now - y_since < Y_TAP) {
		target = (tapped && now - tapped_at < Y_AGAIN) ? NORTH : behind[vox_hero_face & 3];
		swinging = 1;
		tapped = 1;
		tapped_at = now;
		if(target != NORTH) flat_wanted = 0;
	}
	if((pad & SEGA_CTRL_Y) && now - y_since >= Y_TAP && !tipped) {
		tipped = 1;
		flat_wanted = !flat_wanted;
	}
	if((pad & SEGA_CTRL_MODE) && !(prev & SEGA_CTRL_MODE)) mode_since = now;
	if(!(pad & SEGA_CTRL_MODE) && (prev & SEGA_CTRL_MODE) && now - mode_since < Y_TAP) {
		cam_res = (cam_res + 1) % PERSP_RES_COUNT;
		cam_res_at = now ? now : 1;
	}
	int pulling = (pad & SEGA_CTRL_MODE) && now - mode_since >= Y_TAP;
	prev = pad;

	/* The flat view looks north. */
	if(flat_wanted && persp_yaw != NORTH) {
		target = NORTH;
		swinging = 1;
	}
	if(swinging) {
		int16_t d = (int16_t)(target - persp_yaw);   /* the short way round */
		if(d > SWING) d = SWING;
		if(d < -SWING) d = -SWING;
		persp_yaw += d;
		if(persp_yaw == target) swinging = 0;
	}
	cam_flat = clamp(cam_flat + (flat_wanted ? TIP : -TIP), 0, 256);
	cam_pull = clamp(cam_pull + (pulling ? 24 : -24), 0, 256);
}
#endif

/* Close a quarter of the distance each frame, at least one fat pixel and
 * at most four: quick to catch up, soft to settle. */
static int ease(int from, int to) {
	int d = to - from;
	if(d == 0) return from;
	int step = d / 4;
	if(step == 0) step = d > 0 ? 1 : -1;
	return from + clamp(step, -4, 4);
}

static uint32_t who(int cx, int cy) { return rogue_who(cx, cy); }
static int alive(uint32_t id)       { return rogue_alive(id); }
static int hp(uint32_t id)          { return rogue_hp(id); }

static const vox_source_t source = { rogue_under, who, alive, hp, rogue_ground };

/* Turn what the renderer saw happen into noise. A blow that lands is heard
 * as the hit, not as the swing as well. */
static void sounds(void) {
	static const struct { int event, effect; } heard[] = {
		{ VOX_STEPPED, SFX_STEP }, { VOX_HIT, SFX_HIT }, { VOX_HURT, SFX_HURT },
		{ VOX_BURST, SFX_BURST },
	};
	if((vox_events & VOX_SWUNG) && !(vox_events & (VOX_HIT | VOX_BURST))) sfx(SFX_SWING);
	for(unsigned i = 0; i < sizeof(heard) / sizeof(heard[0]); i++) {
		if(vox_events & heard[i].event) sfx(heard[i].effect);
	}
	vox_events = 0;
	sfx_flush();
}

/* One step of the dungeon display, one vblank long when it keeps up: fold
 * in Rogue's latest screen, move the camera toward the hero, bring the
 * hidden framebuffer up to date and show it. */
void game_frame(void) {
	if(plat_screen_changed) {
		int room[4];
		plat_screen_changed = 0;
		vox_load_screen(rogue_screen(), &source, rogue_room(room) ? room : 0,
		                rogue_depth(), plat_step_frames);
		for(uint32_t id; (id = rogue_attacker()) != 0; ) vox_jab(id);
#ifdef PERSP
		persp_sync();
#endif
	}
	if(blanked) {
		vox_palette(1);
		blanked = 0;
		anim_tick = MARS_SYS_COMM12;
	}
	sounds();
	/* Stairs or a teleport: be there, do not pan across the level. */
	if(vox_hero_jumped) {
		vox_hero_jumped = 0;
		cam_snap = 1;
	}

	/* Things move by the clock, so a slow frame does not slow the game. */
	int steps = clamp((int)(MARS_SYS_COMM12 - anim_tick), 1, 3);
	anim_tick = MARS_SYS_COMM12;
	int first = cam_snap;
	while(steps-- > 0) {
		vox_animate();
		int tx = clamp(vox_hero_x + CELL / 2 - VIEW_W / 2, CAM_X_MIN, CAM_X_MAX);
		int ty = clamp(vox_hero_y + CELL / 2 - VIEW_H / 2 - 4, CAM_Y_MIN, CAM_Y_MAX);
		if(cam_snap) {
			cam_x = tx;
			cam_y = ty;
			cam_snap = 0;
		}
		cam_x = ease(cam_x, tx);
		cam_y = ease(cam_y, ty);
	}

#ifdef PERSP
	static int settle, was_flat;
	camera_buttons();
	if(PERSP > 1) persp_yaw += 0x100;   /* PERSP=2 turns by itself, for measuring */

	int is_flat = (cam_flat == 256 && persp_yaw == NORTH && cam_pull == 0);
	if(is_flat && !was_flat) {
		/* Hand over to the fixed renderer, which has to start from nothing. */
		vox_forget();
		first = 1;
	}
	was_flat = is_flat;

	if(!is_flat) {
		/* While anything moves, and for the frame it comes to rest. */
		if(vox_moving || settle) persp_creatures();
		settle = vox_moving;

		/* Flat, the picture is centred where the fixed view's camera is,
		 * which stops at the map's edges; in perspective, on the hero. */
		int hx = vox_hero_x + CELL / 2, hy = vox_hero_y + CELL / 2;
		int fx = cam_x + VIEW_W / 2, fy = cam_y + VIEW_H / 2 + 4;
		int px = hx + (((fx - hx) * cam_flat) >> 8);
		int py = hy + (((fy - hy) * cam_flat) >> 8);

		uint16_t p0 = frt_read();
		persp_render((uint16_t *)&MARS_FRAMEBUFFER, px, py, persp_yaw, cam_pull, cam_flat, cam_res);
		game_render_ticks = (uint16_t)(frt_read() - p0);
		swapBuffers();

		/* Once a second: frames shown, and the mean and worst time to draw one. */
		static uint32_t since, sum;
		static uint16_t frames, worst;
		frames++;
		sum += game_render_ticks;
		if(game_render_ticks > worst) worst = game_render_ticks;
		if(MARS_SYS_COMM12 - since >= 60) {
			perf_fps = frames;
			perf_mean = (uint16_t)(sum / frames);
			perf_worst = worst;
			since = MARS_SYS_COMM12;
			frames = worst = 0;
			sum = 0;
		}
		return;
	}
#endif
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
	sfx_flush();
	wait_vblank();
}

/* Primary CPU main loop */
int m_main(void) {
	Hw32xInit(MARS_VDP_MODE_256, 0);
	Hw32xDelay(1); // Wait for MD's first VInt to complete

	frt_init();
	vox_init();
#ifdef PERSP
	persp_init();
#endif

	/* The moment Start goes down seeds the dungeon. */
	text_begin();
	text_put(14, 10, "VOXELIKE32X", -1, TEXT_YELLOW, 0);
	text_put(14, 14, "PRESS START", -1, TEXT_WHITE, 0);
	text_commit();
#ifdef BOT
	/* Nobody is there to press it. */
	for(uint32_t t = MARS_SYS_COMM12; MARS_SYS_COMM12 - t < 120 && !(MARS_SYS_COMM8 & SEGA_CTRL_START); );
#else
	while(!(MARS_SYS_COMM8 & SEGA_CTRL_START));
#endif

	rogue_start();
	rogue_play();   /* draws and reads the pad from inside plat_getkey */
	return 0;
}
