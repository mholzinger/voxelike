#include "mars.h"
#include "voxel.h"
#include "text.h"
#include "persp.h"
#include "sfx.h"
#include "splash.h"
#include "sh2zam/sh2zam.h"
#include "../port/plat.h"
#include "version.h"

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
int camera_yaw_settled = 1;

/* For the HUD of a measuring build. */
uint16_t perf_fps, perf_mean, perf_worst;

/* The camera's buttons.
 *   X, Z            swing it round the hero
 *   Y tapped        swing behind the hero, so up on the pad is the way he faces
 *   Y tapped twice  swing back to north
 *   Y held          tip the view over into the flat one, or back out of it
 *   (nothing)       it swings if a wall hides the hero
 *   MODE held       pull back to show more, for as long as it is held
 *   MODE tapped     next picture resolution: 160x112, 320x112, 320x224
 * The flat view is the fixed renderer's own, at its full frame rate; the
 * perspective one flattens until the two pictures meet and then hands over.
 * Turning the camera from the flat view tips it back first. */
#define Y_TAP      15      /* vblanks: a press shorter than this is a tap */
#define Y_AGAIN    30      /* vblanks: a second tap this soon makes it a double */
#define SWING      0x500   /* turn per frame when a button swings the camera */
#define SWING_GENTLY 0x300 /* and at most this when it moves by itself */
#define TIP        8       /* of 256 per frame: tipping over takes half a second */
#define NORTH      0xC000
#define NORMAL_PULL (-54)  /* eye distance 111 instead of 122, 10% tighter */
#define FIGHT_PULL  (-144)  /* same framing during a duel and its finish */

static int cam_pull;       /* 0 close in .. 256 pulled back */
static int cam_flat;       /* 0 full perspective .. 256 none */
#ifndef PERSP_RES
#define PERSP_RES PERSP_RES_WIDE
#endif
int cam_res = PERSP_RES;   /* PERSP_RES_x; make RES=n picks the one to start in */
uint32_t cam_res_at;       /* vblank it was last changed on, 0 for never */
static int flat_wanted;
static int duel_mix, duel_x, duel_y;

static int duel_pull(int pull) {
	return pull + (FIGHT_PULL - pull) * duel_mix / 256;
}

static void duel_centre(int *x, int *y) {
	*x += (duel_x - *x) * duel_mix / 512;
	*y += (duel_y - *y) * duel_mix / 512;
}


/* Keep the pulled-back and flat endpoints as designed. */
static int gameplay_pull(void) {
	return cam_pull + NORMAL_PULL * (256 - cam_pull) * (256 - cam_flat) / (256 * 256);
}


/* Allow a short pause between taps before automatic wall recovery. */
#define TAP_GAP       20
#define HANDS_OFF     180
#define TALL          5      /* voxels over the hero's floor that hide him */
#define HIDDEN        3      /* hidden_from() at which he cannot be seen whole */

/* How much stands in the way of seeing the hero from the direction yaw
 * looks along: the cells beside him on the eye's side, and one farther. */
static int hidden_at(int cx, int cy, uint16_t yaw) {
	static const int16_t around[] = { 0, 0x1800, -0x1800 };
	int floor = vox_height_at(cx, cy) - 1;
	int hx = cx * CELL + CELL / 2, hy = cy * CELL + CELL / 2;
	int score = 0;

	for(unsigned i = 0; i < sizeof(around) / sizeof(around[0]); i++) {
		sh2z_fx_t s, c;
		sh2z_sincos((uint16_t)(yaw + around[i]), &s, &c);
		/* Towards the eye is backwards along the view. */
		int x = hx - ((c * 10) >> 16), y = hy - ((s * 10) >> 16);
		/* A wall straight between is what hides him; one to the side of
		 * that only crowds the view. */
		if(vox_height_at(x >> 3, y >> 3) - floor >= TALL) score += i ? 1 : 3;
		if(i == 0) {
			x = hx - ((c * 18) >> 16);
			y = hy - ((s * 18) >> 16);
			if(vox_height_at(x >> 3, y >> 3) - floor >= 2 * TALL) score += 2;
		}
	}
	return score;
}

static int hidden_from(uint16_t yaw) {
	return hidden_at(vox_hero_cx, vox_hero_cy, yaw);
}

/* Prefer less obstruction, then the shortest swing. Keep the current
 * angle as an option so recovery can leave it alone if no view improves it. */
static uint16_t best_view(uint16_t first, int count, int step) {
	uint16_t best_yaw = persp_yaw;
	int best = hidden_from(persp_yaw) * 1024;

	for(int i = 0; i < count; i++) {
		uint16_t yaw = (uint16_t)(first + i * step);
		int swing = (int16_t)(yaw - persp_yaw);
		if(swing < 0) swing = -swing;
		int score = hidden_from(yaw) * 1024 + (swing >> 6);
		if(score < best) {
			best = score;
			best_yaw = yaw;
		}
	}
	return best_yaw;
}

static void camera_buttons(void) {
	static const uint16_t behind[4] = { 0x4000, 0x8000, NORTH, 0x0000 };
	static uint16_t prev, target;
	static uint32_t y_since, tapped_at, mode_since, hands_at;
	static int swinging, tapped, tipped, gently;
	uint16_t pad = MARS_SYS_COMM8;
	uint32_t now = MARS_SYS_COMM12;

	static uint32_t pushed_at;
	static int recovery_x, recovery_y, recovery_depth;
	int direction = pad & (SEGA_CTRL_UP | SEGA_CTRL_DOWN | SEGA_CTRL_LEFT | SEGA_CTRL_RIGHT);
	int manual = pad & (SEGA_CTRL_X | SEGA_CTRL_Y | SEGA_CTRL_Z);
	if(direction) pushed_at = now ? now : 1;
	if(manual) {
		hands_at = now ? now : 1;
		if(gently) swinging = gently = 0;
	}
	int resting = !direction && !vox_hero_busy && (!pushed_at || now - pushed_at >= TAP_GAP);
	int quiet = !hands_at || now - hands_at >= HANDS_OFF;
	if(PERSP == 1 && quiet && resting && !flat_wanted && (!swinging || gently)) {
		int depth = rogue_depth();
		/* If movement interrupted a swing, see what hides the hero
		 * from the new tile before resuming it. */
		if(!swinging || recovery_x != vox_hero_cx || recovery_y != vox_hero_cy ||
		   recovery_depth != depth) {
			target = hidden_from(persp_yaw) >= HIDDEN ? best_view(NORTH, 4, 0x4000) : persp_yaw;
			swinging = gently = target != persp_yaw;
			recovery_x = vox_hero_cx;
			recovery_y = vox_hero_cy;
			recovery_depth = depth;
		}
	}

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
		gently = 0;
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
		gently = 0;
	}
	if(swinging && (!gently || resting)) {
		int16_t d = (int16_t)(target - persp_yaw);   /* the short way round */
		int most = SWING;
		if(gently) {
			/* By itself: unhurried, easing in to rest. */
			most = (d < 0 ? -d : d) / 4;
			most = clamp(most, 0xC0, SWING_GENTLY);
		}
		if(d > most) d = (int16_t)most;
		if(d < -most) d = (int16_t)-most;
		persp_yaw += d;
		if(persp_yaw == target) swinging = gently = 0;
	}
	camera_yaw_settled = !swinging && !manual;
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

static void load_dungeon_screen(int stage_defeat) {
	int room[4];
	plat_screen_changed = 0;
	int terrain_changed = vox_load_screen(rogue_screen(), &source, rogue_room(room) ? room : 0,
	                                      rogue_depth(), plat_step_frames, stage_defeat);
	for(uint32_t id; (id = rogue_attacker()) != 0; ) vox_jab(id);
#ifdef PERSP
	persp_sync(terrain_changed);
#else
	(void)terrain_changed;
#endif
}

#ifdef PERSP
static int update_hero_gear(void) {
	int armor, weapon;
	rogue_gear(&armor, &weapon);
#ifdef SHOW_ARMOR
	armor = SHOW_ARMOR;
#endif
#ifdef SHOW_WEAPON
	weapon = SHOW_WEAPON;
#endif
	int dressed = armor != vox_hero_armor || weapon != vox_hero_weapon;
	vox_hero_armor = armor;
	vox_hero_weapon = weapon;
	return dressed;
}

/* Favor a three-quarter side view with both fighters clear of walls. */
static uint16_t defeat_yaw(int x, int y, int dx, int dy) {
	uint16_t best_yaw = persp_yaw;
	int best = INT32_MAX;
	int span = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
	if(!span) return best_yaw;
	for(unsigned i = 0; i < 16; i++) {
		uint16_t yaw = (uint16_t)(i * 0x1000);
		sh2z_fx_t sin, cos;
		sh2z_sincos(yaw, &sin, &cos);
		int depth = (dx * cos + dy * sin) >> 8;
		if(depth < 0) depth = -depth;
		int swing = (int16_t)(yaw - persp_yaw);
		if(swing < 0) swing = -swing;
		int cover = hidden_from(yaw) + hidden_at(x / CELL, y / CELL, yaw);
		/* A 22.5-degree offset from broadside shows depth and the gap
		 * between the fighters. A clear view matters more than the angle. */
		int profile = depth / span - 98;
		if(profile < 0) profile = -profile;
		int score = cover * 1024 + profile * 3 + (swing >> 8);
		if(score < best) {
			best = score;
			best_yaw = yaw;
		}
	}
	return best_yaw;
}

/* Use a private camera angle and return to the gameplay view before
 * reading the next command. Rogue has already resolved the killing hit. */
static void play_defeat(int enemy_x, int enemy_y) {
	enum { APPROACH = 15, FLASH_AT = 17, IMPACT = 22, RETREAT = 34, END = 50 };
	int hx = vox_hero_x + CELL / 2, hy = vox_hero_y + CELL / 2;
	int dx = enemy_x - hx, dy = enemy_y - hy;
	int swing = (int16_t)(defeat_yaw(enemy_x, enemy_y, dx, dy) - persp_yaw);
	int from_x = hx, from_y = hy;
	duel_centre(&from_x, &from_y);
	int from_pull = duel_pull(NORMAL_PULL);
	int previous = 0;
	uint32_t start = MARS_SYS_COMM12;
	update_hero_gear();
	persp_creatures(0);
	for(;;) {
		int t = clamp((int)(MARS_SYS_COMM12 - start), 0, END);
		if(t >= FLASH_AT && t < IMPACT && previous < FLASH_AT) {
			vox_defeat_flash();
			persp_creatures(0);
		}
		if(t >= IMPACT && previous < IMPACT) {
			vox_release_defeat();
			sounds();
		}
		if(t >= IMPACT) {
			int steps = t - (previous < IMPACT ? IMPACT : previous);
			while(steps-- > 0) vox_animate();
			persp_creatures(0);
		}
		int p = clamp(t, 0, APPROACH) * 256 / APPROACH
		      - clamp(t - RETREAT, 0, END - RETREAT) * 256 / (END - RETREAT);
		int eased = ((p * p >> 8) * (768 - 2 * p)) >> 8;
		uint16_t yaw = (uint16_t)(persp_yaw + swing * eased / 256);
		int approach = clamp(t, 0, APPROACH) * 256 / APPROACH;
		int remaining = 256 - (((approach * approach >> 8) * (768 - 2 * approach)) >> 8);
		int x = hx + dx * eased / 512 + (from_x - hx) * remaining / 256;
		int y = hy + dy * eased / 512 + (from_y - hy) * remaining / 256;
		int pull = NORMAL_PULL + (FIGHT_PULL - NORMAL_PULL) * eased / 256 +
		           (from_pull - NORMAL_PULL) * remaining / 256;
		persp_render((uint16_t *)&MARS_FRAMEBUFFER, x, y, yaw, pull, 0, cam_res);
		swapBuffers();
		if(t == END) break;
		previous = t;
	}
	anim_tick = MARS_SYS_COMM12;
	duel_mix = 0;
}

#endif

/* One step of the dungeon display, one vblank long when it keeps up: fold
 * in Rogue's latest screen, move the camera toward the hero, bring the
 * hidden framebuffer up to date and show it. */
void game_frame(void) {
	int stage_defeat = 0;
#ifdef PERSP
	int manual = MARS_SYS_COMM8 & (SEGA_CTRL_X | SEGA_CTRL_Y | SEGA_CTRL_Z | SEGA_CTRL_MODE);
	stage_defeat = PERSP == 1 && !manual && !cam_flat && !cam_pull && !flat_wanted &&
	               camera_yaw_settled;
#endif
	int screen_changed = plat_screen_changed;
	if(screen_changed) load_dungeon_screen(stage_defeat);
	if(blanked) {
		vox_palette(VOX_FULL_LIGHT);
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
#ifdef PERSP
	int duel_step = steps * 32;
#endif
#if defined(PERSP) && !defined(STATS)
	int particles = vox_particles_active();
#endif
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
#if !defined(STATS)
	static struct {
		int valid, x, y, pull, flat, res;
		uint16_t yaw;
	} shown;
#endif
	camera_buttons();
	if(PERSP > 1) persp_yaw += 0x100;   /* PERSP=2 turns by itself, for measuring */

	int is_flat = (cam_flat == 256 && persp_yaw == NORTH && cam_pull == 0);
	if(is_flat && !was_flat) {
		/* Hand over to the fixed renderer, which has to start from nothing. */
		vox_forget();
		first = 1;
	}
	was_flat = is_flat;

	int enemy_x, enemy_y;
	if(vox_defeat_target(&enemy_x, &enemy_y)) play_defeat(enemy_x, enemy_y);
	if(PERSP != 1 || first || manual || cam_flat || cam_pull || flat_wanted) {
		duel_mix = 0;
	} else if(vox_duel_target(&duel_x, &duel_y)) {
		duel_mix = clamp(duel_mix + duel_step, 0, 256);
	} else {
		duel_mix = clamp(duel_mix - duel_step, 0, 256);
	}
#if !defined(STATS)
	if(is_flat) shown.valid = 0;
#endif
	if(!is_flat) {
		/* While anything moves, and for the frame it comes to rest, and
		 * when the hero has changed what he wears or holds. */
		int dressed = update_hero_gear();
		int creatures_changed = vox_moving || settle || dressed;
		if(creatures_changed) persp_creatures(0);
		settle = vox_moving;

		/* Flat, the picture is centred where the fixed view's camera is,
		 * which stops at the map's edges; in perspective, on the hero. */
		int hx = vox_hero_x + CELL / 2, hy = vox_hero_y + CELL / 2;
		int fx = cam_x + VIEW_W / 2, fy = cam_y + VIEW_H / 2 + 4;
		int px = hx + (((fx - hx) * cam_flat) >> 8);
		int py = hy + (((fy - hy) * cam_flat) >> 8);
		duel_centre(&px, &py);
		int pull = duel_pull(gameplay_pull());

#if !defined(STATS)
		/* The visible buffer is already valid. Keep polling and animating
		 * on the vblank clock, without sending duplicate rays. */
		if(shown.valid && !first && !screen_changed && !creatures_changed && !particles &&
		   shown.x == px && shown.y == py && shown.yaw == persp_yaw &&
		   shown.pull == pull && shown.flat == cam_flat && shown.res == cam_res) {
			game_render_ticks = 0;
			wait_vblank();
			return;
		}
		shown.valid = 1;
		shown.x = px;
		shown.y = py;
		shown.yaw = persp_yaw;
		shown.pull = pull;
		shown.flat = cam_flat;
		shown.res = cam_res;
#endif

		uint16_t p0 = frt_read();
		persp_render((uint16_t *)&MARS_FRAMEBUFFER, px, py, persp_yaw, pull, cam_flat, cam_res);
		game_render_ticks = (uint16_t)(frt_read() - p0);
		swapBuffers();

		/* Once a second: frames shown, and the mean and worst time to draw one. */
		static uint32_t since, sum;
		static uint16_t frames, worst;
		frames++;
		sum += game_render_ticks;
		if(game_render_ticks > worst) worst = game_render_ticks;
#ifdef BENCH
		/* Once a turn of the camera, which is the same frames every time. */
		if(frames == 256) {
#else
		if(MARS_SYS_COMM12 - since >= 60) {
#endif
			perf_fps = (uint16_t)(frames * 60 / (MARS_SYS_COMM12 - since));
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

/* Rogue still has its dungeon screen here, including the fatal attacker.
 * Play the scene on the vblank clock without taking any more game turns. */
void game_death(void) {
	enum { PUSH = 24, BURST = 32, RETREAT = 59, END = 92 };
#ifdef PERSP
	update_hero_gear();
	int start_pull = duel_pull(gameplay_pull());
	int start_flat = cam_flat;
	const int closest = -172;
#endif
	load_dungeon_screen(0);
	vox_hero_hit_flash(0);
#ifdef PERSP
	persp_creatures(0);
#endif
	text_begin();
	text_commit();
	vox_palette(VOX_FULL_LIGHT);
#ifdef PERSP
	int hx = vox_hero_cx * CELL + CELL / 2, hy = vox_hero_cy * CELL + CELL / 2;
	int fx = cam_x + VIEW_W / 2, fy = cam_y + VIEW_H / 2 + 4;
	int from_x = hx, from_y = hy;
	duel_centre(&from_x, &from_y);
#else
	int x = clamp(vox_hero_x + CELL / 2 - VIEW_W / 2, CAM_X_MIN, CAM_X_MAX);
	int y = clamp(vox_hero_y + CELL / 2 - VIEW_H / 2 - 4, CAM_Y_MIN, CAM_Y_MAX);
#endif
	uint32_t start = MARS_SYS_COMM12;
	int advanced = PUSH - 4;
	int light = VOX_FULL_LIGHT;
	for(;;) {
		int t = clamp((int)(MARS_SYS_COMM12 - start), 0, END);
#ifdef PERSP
		int previous = advanced;
#endif
		while(advanced < t) {
			if(++advanced == BURST - 4) vox_hero_hit_flash(1);
			if(advanced == BURST) {
#ifdef PERSP
				persp_hero_shatter();
#else
				vox_hero_shatter(0);
#endif
				vox_events |= VOX_BURST;
			}
			vox_animate();
		}
		if(t >= PUSH - 4) sounds();
#ifdef PERSP
		int push = clamp(t, 0, PUSH);
		int retreat = clamp(t - RETREAT, 0, END - RETREAT);
		/* At its closest, the hero is about 50 percent larger than in
		 * the normal gameplay view. */
		int pull = start_pull + (closest - start_pull) * push / PUSH
		         + (256 - closest) * retreat * retreat / ((END - RETREAT) * (END - RETREAT));
		int flat = start_flat * (PUSH - push) / PUSH;
		int x = hx + ((fx - hx) * flat >> 8) + (from_x - hx) * (PUSH - push) / PUSH;
		int y = hy + ((fy - hy) * flat >> 8) + (from_y - hy) * (PUSH - push) / PUSH;
		if(advanced != previous) persp_creatures(0);
		persp_render((uint16_t *)&MARS_FRAMEBUFFER, x, y, persp_yaw, pull, flat, cam_res);
#else
		vox_prepare((uint16_t *)&MARS_FRAMEBUFFER, currentFB, x, y);
#endif
		int next_light = VOX_FULL_LIGHT * clamp(END - t, 0, END - RETREAT) / (END - RETREAT);
		if(next_light != light) {
			vox_palette(next_light);
			light = next_light;
		}
		swapBuffers();
		if(t == END) break;
	}
	blanked = 1;
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

static void title_screen(void) {
	const unsigned last = splash_title_frame();
	splash_palette_init();
#if !defined(BOT) && !defined(BENCH)
	uint32_t start = MARS_SYS_COMM12;
	unsigned shown = last;
	while(!(MARS_SYS_COMM8 & SEGA_CTRL_START)) {
		unsigned frame = (MARS_SYS_COMM12 - start) / SPLASH_FRAME_TICKS;
		if(frame >= last) break;
		if(frame == shown) {
			wait_vblank();
			continue;
		}
		splash_draw(frame);
		swapBuffers();
		shown = frame;
	}
#endif
	splash_draw(last);
	swapBuffers();

	text_begin();
	text_put(14, 18, "PRESS START", -1, TEXT_WHITE, 0);
	text_put((TEXT_W - (int)sizeof VERSION + 1) / 2, 24, VERSION, -1, TEXT_WHITE, 0);
	text_commit();
#if defined(BOT) || defined(BENCH)
	/* Nobody is there to press it. */
	for(uint32_t t = MARS_SYS_COMM12; MARS_SYS_COMM12 - t < 120 && !(MARS_SYS_COMM8 & SEGA_CTRL_START); );
#else
	/* A press that skipped the animation must be released before playing. */
	while(MARS_SYS_COMM8 & SEGA_CTRL_START) wait_vblank();
	while(!(MARS_SYS_COMM8 & SEGA_CTRL_START)) wait_vblank();
#endif
}

/* Start has already seeded and generated Rogue's first level by this point. */
static void enter_dungeon(void) {
	text_begin();
	text_commit();

#if !defined(BOT) && !defined(BENCH)
	unsigned first = splash_title_frame();
	unsigned last = splash_last_frame();
	uint32_t start = MARS_SYS_COMM12;
	for(unsigned frame = first + 1; frame <= last; frame++) {
		while(MARS_SYS_COMM12 - start < (frame - first) * SPLASH_FRAME_TICKS)
			wait_vblank();
		splash_draw(frame);
		swapBuffers();
	}
#endif

#if defined(PERSP) && !defined(BOT) && !defined(BENCH)
	enum { DIVE = 27, RISE = 46, HOLD = 8, PULL_BACK = 28, CLOSEST = -200 };
	const int pull_at = DIVE + RISE + HOLD;
	const int end = pull_at + PULL_BACK;
	update_hero_gear();
	load_dungeon_screen(0);
	persp_yaw = best_view((uint16_t)(persp_yaw - 0x7000), 15, 0x1000);

	int x = vox_hero_x + CELL / 2, y = vox_hero_y + CELL / 2;
	int height = persp_hero_height();
	int last_buried = -1;
	vox_palette(VOX_FULL_LIGHT);
	uint32_t entered = MARS_SYS_COMM12;
	for(;;) {
		int t = clamp((int)(MARS_SYS_COMM12 - entered), 0, end);
		int rise = clamp(t - DIVE, 0, RISE);
		int buried = height - rise * height / RISE;
		int pull = CLOSEST;
		if(t < DIVE) {
			pull = 144 + (CLOSEST - 144) * t * t / (DIVE * DIVE);
		} else if(t > pull_at) {
			int p = (t - pull_at) * 256 / PULL_BACK;
			int eased = ((p * p >> 8) * (768 - 2 * p)) >> 8;
			pull = CLOSEST + (NORMAL_PULL - CLOSEST) * eased / 256;
		}
		if(buried != last_buried) {
			persp_creatures(buried);
			last_buried = buried;
		}
		persp_render((uint16_t *)&MARS_FRAMEBUFFER, x, y, persp_yaw, pull, 0, cam_res);
		swapBuffers();
		if(t == end) break;
	}
	anim_tick = MARS_SYS_COMM12;
#else
	/* Splash indices stay black until the normal dungeon draw. */
	for(unsigned i = 0; i < 8; i++)
		(&MARS_CRAM)[i] = 0;
#endif
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
	title_screen();
	rogue_start();
	enter_dungeon();
	rogue_play();   /* draws and reads the pad from inside plat_getkey */
	return 0;
}
