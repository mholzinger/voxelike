#include "mars.h"
#include "voxel.h"
#include "text.h"
#include "sfx.h"
#include "../port/plat.h"

/* The machine side of the Rogue port. Rogue runs as an ordinary blocking
 * program; every time it waits for a key, plat_getkey works out what kind
 * of answer it wants, puts the matching screen up and turns the pad into
 * the key. Rogue itself never learns it is not talking to a keyboard. */

extern void game_frame(void);
extern void game_blank(void);

int plat_screen_changed = 1;

/* The player's own turns take STEP frames on screen. Nothing happens
 * between them: IDLE_TURN is how many vblanks the world would wait before
 * taking a turn of its own, and 0 is never. It was 24 once, and a hero
 * standing still to think was killed where he stood. */
#define STEP          8
#define IDLE_TURN     0
int plat_step_frames = STEP;

#define DIR_MASK      (SEGA_CTRL_UP | SEGA_CTRL_DOWN | SEGA_CTRL_LEFT | SEGA_CTRL_RIGHT)
#define BUTTON_MASK   (SEGA_CTRL_A | SEGA_CTRL_B | SEGA_CTRL_C | SEGA_CTRL_START)
#define ESCAPE        27

/* Timings in vblanks. A fresh direction waits SETTLE before it is read, so
 * the two halves of a diagonal that land a frame apart count as one press. */
#define SETTLE        3
#define REPEAT_FIRST  16
#define REPEAT_NEXT   6
#ifdef BOT
#define MORE_WAIT     10
#define PAGE_WAIT     30    /* the test bot reads fast */
#else
#define MORE_WAIT     60    /* a --More-- in the dungeon moves on by itself */
#define PAGE_WAIT     0     /* a text page waits for the player */
#endif
#define MSG_HOLD      90    /* a message stays up at least this long */

/* The dungeon HUD keeps a one column margin; pages use the full width. */
#define HUD_X         1
#define HUD_W         (TEXT_W - 2)
#define MSG_ROW       1
#define LIST_ROW      3
#define STATUS_ROW    26

unsigned long plat_ticks(void) {
#ifdef BENCH
	return BENCH;   /* the dungeon's seed: the same one every run */
#endif
	return MARS_SYS_COMM12;
}

/* --- pad ------------------------------------------------------------ */

typedef struct {
	uint16_t pressed;   /* buttons that went down this step */
	uint16_t dir;       /* direction that fired this step, with auto-repeat */
	uint16_t held;      /* direction held steady for SETTLE, for walking */
} input_t;

static uint16_t prev_pad = 0xFFFF;   /* nothing counts as newly pressed at first */

#ifdef PERSP
static void pad_turn_latch(uint16_t dir);
#else
#define pad_turn_latch(dir) ((void)0)
#endif

static input_t poll(void (*frame)(void)) {
	static uint32_t dir_due, last_fire, held_since;
	static uint16_t last_dir, last_held;
	input_t in = { 0, 0, 0 };

	frame();

	uint32_t now = MARS_SYS_COMM12;
	uint16_t pad = MARS_SYS_COMM8;
	uint16_t dir = pad & DIR_MASK;
	in.pressed = pad & ~prev_pad;
	prev_pad = pad;
	pad_turn_latch(dir);

	if(dir != last_held) {
		last_held = dir;
		held_since = now;
	}
	if(now - held_since >= SETTLE) in.held = dir;

	if(in.pressed & DIR_MASK) dir_due = now + SETTLE;
	if(dir && (int32_t)(now - dir_due) >= 0) {
		/* the first repeat of a held direction waits longer than the rest */
		int repeating = (dir == last_dir && now - last_fire < REPEAT_FIRST + REPEAT_NEXT);
		dir_due = now + (repeating ? REPEAT_NEXT : REPEAT_FIRST);
		last_dir = dir;
		last_fire = now;
		in.dir = dir;
	}
	return in;
}

/* Wait for any press, or for `frames` vblanks if that is not 0. */
static void wait_any(void (*frame)(void), int frames) {
	uint32_t start = MARS_SYS_COMM12;
	for(;;) {
		if(poll(frame).pressed & (DIR_MASK | BUTTON_MASK)) return;
		if(frames && MARS_SYS_COMM12 - start >= (uint32_t)frames) return;
	}
}

#ifdef PERSP
/* Up on the pad means away from the camera, as near as one of the map's
 * four directions comes to that: never a diagonal, which Rogue will not
 * walk through a door. The pad's direction is turned by quarter turns, and
 * the turn is sticky. It is looked at again only on a push that follows
 * PAD_REST vblanks of the pad let go, so neither a held direction nor a run
 * of taps changes its meaning part way; and it changes then only if the
 * camera has gone more than PAD_STICK from the direction up already has,
 * so the camera's own small swings leave the controls as they were. */
#define PAD_REST   20
#define PAD_STICK  0x2C00   /* a little over a sixth of a turn */

static unsigned pad_turn;   /* in eighths, always even */

static void pad_turn_latch(uint16_t dir) {
	extern uint16_t persp_yaw;
	static uint16_t was;
	static uint32_t let_go;
	static int rested = 1;
	uint32_t now = MARS_SYS_COMM12;

	if(!dir && was) let_go = now;
	if(!dir && now - let_go >= PAD_REST) rested = 1;
	if(dir && rested) {
		uint16_t north = (uint16_t)(persp_yaw - 0xC000);
		int off = (int16_t)(north - (pad_turn << 13));
		if(off < 0) off = -off;
		if(off > PAD_STICK) pad_turn = (((uint16_t)(north + 0x2000) >> 14) & 3) * 2;
		rested = 0;
	}
	was = dir;
}

static uint16_t camera_dir(uint16_t dir) {
	static const uint16_t ring[8] = {
		SEGA_CTRL_UP, SEGA_CTRL_UP | SEGA_CTRL_RIGHT, SEGA_CTRL_RIGHT,
		SEGA_CTRL_DOWN | SEGA_CTRL_RIGHT, SEGA_CTRL_DOWN, SEGA_CTRL_DOWN | SEGA_CTRL_LEFT,
		SEGA_CTRL_LEFT, SEGA_CTRL_UP | SEGA_CTRL_LEFT,
	};
	unsigned turn = pad_turn;

	for(int i = 0; i < 8; i++) {
		if(ring[i] == dir) return ring[(i + turn) & 7];
	}
	return dir;
}
#else
#define camera_dir(dir) (dir)
#endif

/* Rogue's vi keys for the pad's eight directions. */
static int dir_key(uint16_t dir) {
	switch(dir) {
	case SEGA_CTRL_UP:                     return 'k';
	case SEGA_CTRL_DOWN:                   return 'j';
	case SEGA_CTRL_LEFT:                   return 'h';
	case SEGA_CTRL_RIGHT:                  return 'l';
	case SEGA_CTRL_UP | SEGA_CTRL_LEFT:    return 'y';
	case SEGA_CTRL_UP | SEGA_CTRL_RIGHT:   return 'u';
	case SEGA_CTRL_DOWN | SEGA_CTRL_LEFT:  return 'b';
	case SEGA_CTRL_DOWN | SEGA_CTRL_RIGHT: return 'n';
	default:                               return 0;
	}
}

/* --- text ----------------------------------------------------------- */

/* Length of an 80 column row without its trailing blanks. */
static int row_len(const unsigned char *row, int cols) {
	while(cols > 0 && (row[cols - 1] & 0x7F) == ' ') cols--;
	return cols;
}

static int contains(const unsigned char *row, int len, const char *word) {
	for(int i = 0; i < len; i++) {
		int j = 0;
		while(word[j] && i + j < len && (row[i + j] & 0x7F) == word[j]) j++;
		if(!word[j]) return 1;
	}
	return 0;
}

/* Put text at (x, y) wrapped at word breaks to width w. Returns the row
 * after the last one used. */
static int put_wrapped(int x, int y, int w, const unsigned char *s, int len, int solid) {
	while(len > 0) {
		int n = len;
		if(n > w) {
			n = w;
			while(n > w / 2 && (s[n] & 0x7F) != ' ') n--;
			if((s[n] & 0x7F) != ' ') n = w;
		}
		text_put(x, y++, (const char *)s, n, TEXT_WHITE, solid);
		s += n;
		len -= n;
		while(len > 0 && (*s & 0x7F) == ' ') { s++; len--; }
	}
	return y;
}

#define RES_SHOWN     150   /* vblanks a new resolution's name stays up */

#if defined(PERSP) && defined(STATS)
static char *put_number(char *p, const char *label, unsigned n) {
	char digits[8];
	int len = 0;

	while(*label) *p++ = *label++;
	do { digits[len++] = (char)('0' + n % 10); n /= 10; } while(n);
	while(len) *p++ = digits[--len];
	return p;
}
#endif

/* Rogue wipes its message line at the next key, and with the world taking
 * turns by itself that can be a moment later. The last message is kept
 * here so it stays readable for MSG_HOLD. */
static unsigned char held_msg[MAP_W];
static int held_len;
static uint32_t held_at;
static int hud_holding;   /* the HUD is showing a message Rogue has wiped */

/* Rogue's message line and a compact status line, over the dungeon. */
static void compose_hud(void) {
	const unsigned char *scr = rogue_screen();
	int len = row_len(scr, MAP_W);
	char status[96];

	hud_holding = 0;
	if(len > 0) {
		for(int i = 0; i < len; i++) held_msg[i] = scr[i];
		held_len = len;
		held_at = MARS_SYS_COMM12;
	} else if(held_len && MARS_SYS_COMM12 - held_at < MSG_HOLD) {
		scr = held_msg;
		len = held_len;
		hud_holding = 1;
	} else {
		held_len = 0;
	}

	text_begin();
	put_wrapped(HUD_X, MSG_ROW, HUD_W, scr, len, 1);
	rogue_status(status);
	text_put(HUD_X, STATUS_ROW, status, HUD_W, TEXT_WHITE, 1);
#ifdef PERSP
	{
		static const char *const res_name[] = { "160x112", "320x112", "320x224", "FOCUS" };
		extern int cam_res;
		extern uint32_t cam_res_at;
#ifdef STATS
		/* A measuring build says how it is doing: frames a second, then
		 * the mean and worst FRT ticks to draw one (3000 is one vblank). */
		extern uint16_t perf_fps, perf_mean, perf_worst;
		char line[48], *p = line;
		p = put_number(p, "FPS ", perf_fps);
		p = put_number(p, " MEAN ", perf_mean);
		p = put_number(p, " WORST ", perf_worst);
		*p++ = ' ';
		for(const char *r = res_name[cam_res]; *r; ) *p++ = *r++;
		*p = 0;
		text_put(HUD_X, STATUS_ROW - 2, line, HUD_W, TEXT_YELLOW, 1);
		(void)cam_res_at;
#else
		/* The resolution names itself for a moment when it is changed. */
		if(cam_res_at && MARS_SYS_COMM12 - cam_res_at < RES_SHOWN) {
			text_put(HUD_X, STATUS_ROW - 2, res_name[cam_res], HUD_W, TEXT_YELLOW, 1);
		}
#endif
	}
#endif
}

static int row_lead(const unsigned char *row, int len) {
	int lead = 0;
	while(lead < len && (row[lead] & 0x7F) == ' ') lead++;
	return lead;
}

/* A full screen of Rogue text (inventory, discoveries, the tombstone) on
 * a 40 column display. Rows too wide to fit give up as much of their
 * shared indent as they have, the other rows follow them left so pictures
 * stay aligned, and whatever is still too wide wraps. */
static void show_page(const unsigned char *cells, int lines, int cols) {
	int indent = cols;
	for(int r = 0; r < lines; r++) {
		const unsigned char *row = cells + r * cols;
		int len = row_len(row, cols);
		if(len > TEXT_W && row_lead(row, len) < indent) indent = row_lead(row, len);
	}
	if(indent == cols) indent = 0;

	text_begin();
	int y = 1;
	for(int r = 0; r < lines && y < TEXT_H - 1; r++) {
		const unsigned char *row = cells + r * cols;
		int len = row_len(row, cols);
		int skip = row_lead(row, len) < indent ? row_lead(row, len) : indent;
		if(len == 0) { y++; continue; }
		y = put_wrapped(0, y, TEXT_W, row + skip, len - skip, 0);
	}
	text_commit();
}

/* --- lists ---------------------------------------------------------- */

/* A cursor list over the dungeon. line(i) names entry i. Returns the entry
 * chosen with A or C, or -1 when B backs out. */
static int choose(int n, const char *(*line)(int)) {
	static const char blank[] = "                                      ";
	int sel = 0, width = 0;

	for(int i = 0; i < n; i++) {
		int len = 0;
		while(line(i)[len]) len++;
		if(len > width) width = len;
	}
	width += 3;   /* cursor, and a space of margin on the right */
	if(width > HUD_W) width = HUD_W;
	int x = TEXT_W - HUD_X - width;   /* against the right edge, off the hero */

	for(;;) {
		compose_hud();
		for(int i = 0; i < n; i++) {
			int on = (i == sel);
			text_put(x, LIST_ROW + i, blank, width, TEXT_WHITE, 1);
			text_put(x, LIST_ROW + i, on ? "> " : "  ", -1, TEXT_YELLOW, 1);
			text_put(x + 2, LIST_ROW + i, line(i), HUD_W - 2,
			         on ? TEXT_YELLOW : TEXT_WHITE, 1);
		}
		text_commit();

		for(;;) {
			input_t in = poll(game_frame);
			if(in.pressed & (SEGA_CTRL_A | SEGA_CTRL_C)) { sfx(SFX_BLIP); return sel; }
			if(in.pressed & SEGA_CTRL_B) return -1;
			if(in.dir & SEGA_CTRL_UP)   { sfx(SFX_BLIP); sel = (sel + n - 1) % n; break; }
			if(in.dir & SEGA_CTRL_DOWN) { sfx(SFX_BLIP); sel = (sel + 1) % n; break; }
		}
	}
}

static const struct { const char *name; char key; } commands[] = {
	{ "Inventory",      'i' },
	{ "Eat",            'e' },
	{ "Quaff potion",   'q' },
	{ "Read scroll",    'r' },
	{ "Wield weapon",   'w' },
	{ "Wear armor",     'W' },
	{ "Take off armor", 'T' },
	{ "Put on ring",    'P' },
	{ "Remove ring",    'R' },
	{ "Zap wand",       'z' },
	{ "Throw",          't' },
	{ "Drop",           'd' },
	{ "Climb stairs",   '<' },
	{ "Discoveries",    'D' },
	{ "Quit",           'Q' },
};
#define N_COMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

static const char *command_line(int i) {
	return commands[i].name;
}

#define MAX_PACK 23   /* Rogue's MAXPACK */
static char pack_text[MAX_PACK][TEXT_W];
static char pack_key[MAX_PACK];

static const char *pack_line(int i) {
	return pack_text[i];
}

/* Answer "which object do you want to ...?" by picking from the pack. */
static int pick_item(void) {
	int n = 0;
	while(n < MAX_PACK && (pack_key[n] = (char)rogue_pack(n, pack_text[n], HUD_W - 2)) != 0) n++;
	if(n == 0) return ESCAPE;

	int sel = choose(n, pack_line);
	return sel < 0 ? ESCAPE : pack_key[sel];
}

/* --- what Rogue is waiting for -------------------------------------- */

/* Rogue left the cursor on the message line: it wants an answer to
 * whatever it just asked there. */
static int prompt_key(void) {
	const unsigned char *scr = rogue_screen();
	int len = row_len(scr, MAP_W);

	/* An identify scroll for one kind of thing keeps asking until it gets
	 * that kind. With none in the pack the only way out is Rogue's own
	 * list, which notices there is nothing to choose and lets go. */
	static int refused;

	if(contains(scr, len, "--More--")) {
		if(contains(scr, len, "must identify")) refused = 1;
		wait_any(game_frame, MORE_WAIT);
		return ' ';
	}
	if(refused && contains(scr, len, "for list")) {
		refused = 0;
		return '*';
	}
#ifdef BOT
	for(int i = 0; i < STEP; i++) poll(game_frame);
	return rogue_bot_answer();
#endif
	if(contains(scr, len, "for list")) return pick_item();

	/* A direction, or a yes or no: A is yes, C is no, B backs out. */
	for(;;) {
		input_t in = poll(game_frame);
		if(in.dir) return dir_key(camera_dir(in.dir));
		if(in.pressed & SEGA_CTRL_A) return 'y';
		if(in.pressed & SEGA_CTRL_C) return 'n';
		if(in.pressed & (SEGA_CTRL_B | SEGA_CTRL_START)) return ESCAPE;
	}
}

static uint16_t step_dir;        /* the last key was a step this way... */
static int step_cx, step_cy;     /* ...from this cell */

/* The hero is about to step one cell. If a monster is in the way the step
 * is a blow, and the hero jabs at it. */
static int step_key(uint16_t pad_dir) {
	uint16_t dir = camera_dir(pad_dir);
	int dx = ((dir & SEGA_CTRL_RIGHT) != 0) - ((dir & SEGA_CTRL_LEFT) != 0);
	int dy = ((dir & SEGA_CTRL_DOWN) != 0) - ((dir & SEGA_CTRL_UP) != 0);

	step_dir = pad_dir;
	step_cx = vox_hero_cx;
	step_cy = vox_hero_cy;
	if(rogue_who(vox_hero_cx + dx, vox_hero_cy + dy)) {
		vox_hero_lunge(dx, dy);
		step_dir = 0;
	}
	return dir_key(dir);
}

static int command_key(void) {
	static uint16_t blocked;   /* held direction that goes nowhere */
	uint32_t idle_since = MARS_SYS_COMM12;

	rogue_idle(0);
	plat_step_frames = STEP;
#ifdef BOT
	int key = rogue_bot_key();
	if(key) {
		static const char keys[] = "hjklyubn";
		static const int8_t kdx[] = { -1, 0, 0, 1, -1, 1, -1, 1 };
		static const int8_t kdy[] = { 0, 1, -1, 0, -1, -1, 1, 1 };

		for(int i = 0; i < STEP || vox_hero_busy; i++) poll(game_frame);
		for(int i = 0; i < 8; i++) {
			if(keys[i] == key && rogue_who(vox_hero_cx + kdx[i], vox_hero_cy + kdy[i]))
				vox_hero_lunge(kdx[i], kdy[i]);
		}
		return key;
	}
#endif
	for(;;) {
		input_t in = poll(game_frame);   /* which also takes in Rogue's screen */

		/* A step that left the hero where he was cost no turn. Trying it
		 * again every frame would stop the clock; wait for the world to
		 * move on, or for the player to try something else. */
		if(step_dir) {
			blocked = (step_cx == vox_hero_cx && step_cy == vox_hero_cy) ? step_dir : 0;
			step_dir = 0;
		}
		if(in.held != blocked) blocked = 0;

		/* One step per walk across a cell: a held direction keeps walking. */
		if(in.held && !blocked && !vox_hero_busy && dir_key(in.held)) return step_key(in.held);
		if(hud_holding && MARS_SYS_COMM12 - held_at >= MSG_HOLD) {
			compose_hud();
			text_commit();
		}
		/* Nothing from the player: the world takes a turn anyway. By the
		 * clock, not by frames, which the two views draw at different rates. */
		if(vox_hero_busy) idle_since = MARS_SYS_COMM12;
#ifdef BENCH
		/* Nothing may change between runs, but the readout must. */
		static uint32_t told;
		idle_since = MARS_SYS_COMM12;
		if(idle_since - told >= 60) {
			told = idle_since;
			compose_hud();
			text_commit();
		}
#endif
		if(IDLE_TURN && MARS_SYS_COMM12 - idle_since >= IDLE_TURN) {
			rogue_idle(1);
			plat_step_frames = IDLE_TURN;
			return '.';
		}
		if(in.pressed & SEGA_CTRL_B) return 's';      /* search */
		if(in.pressed & SEGA_CTRL_C) return '.';      /* rest a turn */
		if(in.pressed & SEGA_CTRL_START) {
			/* on stairs: go down. anywhere else: pick up. */
			return (rogue_under(vox_hero_cx, vox_hero_cy) == '%') ? '>' : ',';
		}
		if(in.pressed & SEGA_CTRL_A) {
			int sel = choose(N_COMMANDS, command_line);
			if(sel >= 0) return commands[sel].key;
			compose_hud();
			text_commit();
			idle_since = MARS_SYS_COMM12;
		}
	}
}

/* Hear it when the player has more gold, more things or more rank than at
 * the last key, or stands on a new level. */
static void gains(void) {
	static int had_gold, had_things, had_rank, had_depth, started;
	int gold, things, rank, depth = rogue_depth();

	rogue_tally(&gold, &things, &rank);
	if(started) {
		if(depth != had_depth) sfx(SFX_STAIRS);
		else if(rank > had_rank) sfx(SFX_LEVELUP);
		else if(gold > had_gold) sfx(SFX_GOLD);
		else if(things > had_things) sfx(SFX_ITEM);
	}
	started = 1;
	had_gold = gold;
	had_things = things;
	had_rank = rank;
	had_depth = depth;
}

/* A page Rogue showed in a window of its own, waiting for the next key. */
static const unsigned char *page_cells;
static int page_lines, page_cols;

int plat_getkey(void) {
	const unsigned char *scr = rogue_screen();
	const unsigned char *last = scr + (MAP_H - 1) * MAP_W;

	if(page_cells) {
		show_page(page_cells, page_lines, page_cols);
		page_cells = 0;
		wait_any(game_blank, PAGE_WAIT);
		return ' ';
	}
	/* The tombstone is drawn on the main screen and wants a return. */
	if(contains(last, MAP_W, "[Press return")) {
		sfx(SFX_DEATH);
		show_page(scr, MAP_H, MAP_W);
		wait_any(game_blank, PAGE_WAIT);
		return '\n';
	}

	gains();

	/* Whatever Rogue drew since the last key is now worth showing. */
	plat_screen_changed = 1;
	compose_hud();
	text_commit();

	return rogue_cursor_row() == 0 ? prompt_key() : command_key();
}

void plat_show(const unsigned char *cells, int lines, int cols, int main_screen) {
	if(main_screen) {
		plat_screen_changed = 1;
		page_cells = 0;
	} else {
		page_cells = cells;
		page_lines = lines;
		page_cols = cols;
	}
}

/* Linker symbols _stext and _sdata (C names lose the underscore): where the
 * initialised data's image sits in the ROM, and how long it is. Their
 * addresses are the values. */
extern char stext[], sdata[];

/* Start over the way the console does at power on: reload initialised data
 * from the ROM, then run the boot code again, which clears everything else.
 * Rogue keeps its whole game in globals, so this is what gives it a fresh
 * one. This code and the boot code it jumps to are not disturbed by the
 * copy: one runs from ROM, the other is rewritten with the same bytes. */
static void restart(void) {
	const volatile uint8_t *src = (const volatile uint8_t *)(0x02000000 + (uint32_t)stext);
	volatile uint8_t *dst = (volatile uint8_t *)0x06000000;
	for(uint32_t n = (uint32_t)sdata; n; n--) *dst++ = *src++;
	((void (*)(void))0x06000240)();   /* primary SH-2 entry */
}

/* The game ended (death, victory or quitting). */
void plat_exit(int status) {
	(void)status;
	text_begin();
	text_put(16, 12, "THE END", -1, TEXT_YELLOW, 0);
	text_put(8, 14, "PRESS START TO PLAY AGAIN", -1, TEXT_WHITE, 0);
	text_commit();
#ifdef BOT
	wait_any(game_blank, 120);
#else
	while(!(poll(game_blank).pressed & SEGA_CTRL_START));
#endif

	text_begin();
	text_commit();
	restart();
	for(;;);
}
