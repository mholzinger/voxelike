#include "mars.h"
#include "voxel.h"
#include "text.h"
#include "../port/plat.h"

/* The machine side of the Rogue port. Rogue runs as an ordinary blocking
 * program; every time it waits for a key, plat_getkey works out what kind
 * of answer it wants, puts the matching screen up and turns the pad into
 * the key. Rogue itself never learns it is not talking to a keyboard. */

extern void game_frame(void);
extern void game_blank(void);

int plat_screen_changed = 1;

#define DIR_MASK      (SEGA_CTRL_UP | SEGA_CTRL_DOWN | SEGA_CTRL_LEFT | SEGA_CTRL_RIGHT)
#define BUTTON_MASK   (SEGA_CTRL_A | SEGA_CTRL_B | SEGA_CTRL_C | SEGA_CTRL_START)
#define ESCAPE        27

/* Timings in vblanks. A fresh direction waits SETTLE before it is read, so
 * the two halves of a diagonal that land a frame apart count as one press. */
#define SETTLE        3
#define REPEAT_FIRST  16
#define REPEAT_NEXT   6

/* The dungeon HUD keeps a one column margin; pages use the full width. */
#define HUD_X         1
#define HUD_W         (TEXT_W - 2)
#define MSG_ROW       1
#define LIST_ROW      3
#define STATUS_ROW    26

unsigned long plat_ticks(void) {
	return MARS_SYS_COMM12;
}

/* --- pad ------------------------------------------------------------ */

typedef struct {
	uint16_t pressed;   /* buttons that went down this step */
	uint16_t dir;       /* direction that fired this step, with auto-repeat */
} input_t;

static uint16_t prev_pad = 0xFFFF;   /* nothing counts as newly pressed at first */

static input_t poll(void (*frame)(void)) {
	static uint32_t dir_due, last_fire;
	static uint16_t last_dir;
	input_t in = { 0, 0 };

	frame();

	uint32_t now = MARS_SYS_COMM12;
	uint16_t pad = MARS_SYS_COMM8;
	uint16_t dir = pad & DIR_MASK;
	in.pressed = pad & ~prev_pad;
	prev_pad = pad;

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

static void wait_any(void (*frame)(void)) {
	for(;;) {
		if(poll(frame).pressed & (DIR_MASK | BUTTON_MASK)) return;
	}
}

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

/* Rogue's message line and a compact status line, over the dungeon. */
static void compose_hud(void) {
	const unsigned char *scr = rogue_screen();
	char status[96];

	text_begin();
	put_wrapped(HUD_X, MSG_ROW, HUD_W, scr, row_len(scr, MAP_W), 1);
	rogue_status(status);
	text_put(HUD_X, STATUS_ROW, status, HUD_W, TEXT_WHITE, 1);
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
			if(in.pressed & (SEGA_CTRL_A | SEGA_CTRL_C)) return sel;
			if(in.pressed & SEGA_CTRL_B) return -1;
			if(in.dir & SEGA_CTRL_UP)   { sel = (sel + n - 1) % n; break; }
			if(in.dir & SEGA_CTRL_DOWN) { sel = (sel + 1) % n; break; }
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

	if(contains(scr, len, "--More--")) {
		wait_any(game_frame);
		return ' ';
	}
	if(contains(scr, len, "for list")) return pick_item();

	/* A direction, or a yes or no: A is yes, C is no, B backs out. */
	for(;;) {
		input_t in = poll(game_frame);
		if(in.dir) return dir_key(in.dir);
		if(in.pressed & SEGA_CTRL_A) return 'y';
		if(in.pressed & SEGA_CTRL_C) return 'n';
		if(in.pressed & (SEGA_CTRL_B | SEGA_CTRL_START)) return ESCAPE;
	}
}

static int command_key(void) {
	for(;;) {
		input_t in = poll(game_frame);

		if(in.dir) return dir_key(in.dir);
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
		}
	}
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
		wait_any(game_blank);
		return ' ';
	}
	/* The tombstone is drawn on the main screen and wants a return. */
	if(contains(last, MAP_W, "[Press return")) {
		show_page(scr, MAP_H, MAP_W);
		wait_any(game_blank);
		return '\n';
	}

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

/* The game ended (death, victory or quitting). Starting over needs Rogue's
 * data reloaded from ROM, which is not written yet. */
void plat_exit(int status) {
	(void)status;
	text_begin();
	text_put(16, 12, "THE END", -1, TEXT_YELLOW, 0);
	text_put(10, 14, "RESET TO PLAY AGAIN", -1, TEXT_WHITE, 0);
	text_commit();
	for(;;) game_blank();
}
