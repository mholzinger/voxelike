/* A player that needs no pad, for test builds (make BOT=1). It walks to the
 * stairs and goes down, fights whatever stands next to it, tries the things
 * it picks up, and cannot die or starve, so one unattended run of the
 * emulator shows many levels, monsters and effects. It plays through
 * plat_getkey like a person would: Rogue cannot tell the difference. */
#ifdef BOT
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <curses.h>
#include "rogue.h"
#include "plat.h"

#define FAR	30000
#define USE_EVERY	25	/* turns between trying something from the pack */
#define LOOT_TURNS	150	/* turns on a level spent collecting things first */

#ifdef RENDER_TEST
static short (*steps_to)[NUMCOLS];
static uint16_t *queue;
#else
static short steps_to[NUMLINES][NUMCOLS];
static uint16_t queue[NUMLINES * NUMCOLS];
#endif
static char dirs[] = "hjklyubn";
static int ddx[] = { -1, 0, 0, 1, -1, 1, -1, 1 };
static int ddy[] = { 0, 1, -1, 0, -1, -1, 1, 1 };

static int turns, use_next, stuck;
static int on_level, level_turns;
static int bot_item;		/* pack letter to answer "which object?" with */
static coord last;

/*
 * walkable:
 *	0 for solid, 1 for open, 2 for a secret door or passage that has
 *	to be searched for first.
 */
static int
walkable(int y, int x)
{
    int ch;

    if (x < 0 || x >= NUMCOLS || y <= 0 || y >= NUMLINES - 1)
	return 0;
    ch = chat(y, x);
    if (ch == ' ' || ch == '|' || ch == '-')
	return (flat(y, x) & F_REAL) ? 0 : 2;
    return 1;
}

/*
 * flood:
 *	Steps from every cell to the nearest goal: the things lying on
 *	the level if loot is set, otherwise the stairs.
 */
static void
flood(int loot)
{
    int head = 0, tail = 0, i, x, y;
    coord c, n;
    THING *obj;

    for (y = 0; y < NUMLINES; y++)
	for (x = 0; x < NUMCOLS; x++)
	    steps_to[y][x] = FAR;
    if (loot)
    {
	for (obj = lvl_obj; obj != NULL; obj = next(obj))
	{
	    if (steps_to[obj->o_pos.y][obj->o_pos.x] == 0)
		continue;
	    steps_to[obj->o_pos.y][obj->o_pos.x] = 0;
	    queue[tail++] = obj->o_pos.y * NUMCOLS + obj->o_pos.x;
	}
    }
    else
    {
	steps_to[stairs.y][stairs.x] = 0;
	queue[tail++] = stairs.y * NUMCOLS + stairs.x;
    }
    while (head < tail)
    {
	int cell = queue[head++];
	c = (coord){ cell % NUMCOLS, cell / NUMCOLS };
	for (i = 0; i < 8; i++)
	{
	    n.x = c.x + ddx[i];
	    n.y = c.y + ddy[i];
	    if (!walkable(n.y, n.x) || steps_to[n.y][n.x] != FAR)
		continue;
	    if (i >= 4 && (walkable(n.y, c.x) != 1 || walkable(c.y, n.x) != 1))
		continue;
	    steps_to[n.y][n.x] = steps_to[c.y][c.x] + 1;
	    queue[tail++] = n.y * NUMCOLS + n.x;
	}
    }
}

/*
 * toward:
 *	Which of the eight ways leads to the goal last flooded from, or
 *	-1 if none does.
 */
static int
toward(void)
{
    int i, best = -1;
    coord n;

    for (i = 0; i < 8; i++)
    {
	n.x = hero.x + ddx[i];
	n.y = hero.y + ddy[i];
	if (!walkable(n.y, n.x) || !diag_ok(&hero, &n))
	    continue;
	if (i >= 4 && (walkable(n.y, hero.x) != 1 || walkable(hero.y, n.x) != 1))
	    continue;
	if (best < 0 || steps_to[n.y][n.x] < steps_to[hero.y + ddy[best]][hero.x + ddx[best]])
	    best = i;
    }
    if (best >= 0 && steps_to[hero.y + ddy[best]][hero.x + ddx[best]] == FAR)
	best = -1;
    return best;
}

/*
 * try_item:
 *	Pick the next thing in the pack worth trying and the command
 *	that tries it.
 */
static int
try_item(void)
{
    THING *obj;
    int i = 0, key = 0;

    for (obj = pack; obj != NULL; obj = next(obj), i++)
    {
	if (i < use_next)
	    continue;
	switch (obj->o_type)
	{
	    case POTION: key = 'q'; break;
	    case SCROLL: key = 'r'; break;
	    case STICK: key = 'z'; break;
	    case RING: key = 'P'; break;
	    case ARMOR: key = 'W'; break;
	}
	if (key)
	{
	    use_next = i + 1;
	    bot_item = obj->o_packch;
	    return key;
	}
    }
    use_next = 0;
    return 0;
}

/*
 * rogue_bot_key:
 *	The bot's next command. 0 means it would rather stand a while.
 */
int
rogue_bot_key(void)
{
    int i, best, key;
    coord n;

    pstats.s_hpt = max_hp;
    if (food_left < 500)
	food_left = 1500;
    turns++;
    if (level != on_level)
    {
	on_level = level;
	level_turns = 0;
    }
    level_turns++;

    stuck = (hero.x == last.x && hero.y == last.y) ? stuck + 1 : 0;
    last = hero;
    if (stuck > 6 && (stuck & 1))
	return 's';			/* whatever holds us, let time pass */
    if (turns % 40 == 0)
	return 0;
    if (turns % 97 == 0)
	return 'i';

    /* Held, only the flytrap that holds us can be hit. */
    for (i = 0; i < 8; i++)
    {
	n.x = hero.x + ddx[i];
	n.y = hero.y + ddy[i];
	if (walkable(n.y, n.x) != 1 || moat(n.y, n.x) == NULL || !diag_ok(&hero, &n))
	    continue;
	if (on(player, ISHELD) && moat(n.y, n.x)->t_type != 'F')
	    continue;
	return dirs[i];
    }
    if (turns % USE_EVERY == 0 && (key = try_item()) != 0)
	return key;

#ifdef RENDER_TEST
    /* Reuse Rogue's existing heap so the test bot does not consume the
     * renderer's stack reserve. Allocate only when a path is needed. */
    if (steps_to == NULL)
        steps_to = malloc(NUMLINES * sizeof(*steps_to));
    if (queue == NULL)
        queue = malloc(NUMLINES * NUMCOLS * sizeof(*queue));
    if (steps_to == NULL || queue == NULL)
        return 's';
#endif
    best = -1;
    if (level_turns < LOOT_TURNS && inpack < MAXPACK - 2)
    {
	flood(TRUE);
	best = toward();
    }
    if (best < 0)
    {
	if (hero.x == stairs.x && hero.y == stairs.y)
	    return '>';
	flood(FALSE);
	best = toward();
    }
    if (best < 0)
	return 's';
    if (walkable(hero.y + ddy[best], hero.x + ddx[best]) == 2)
	return 's';
    return dirs[best];
}

/*
 * rogue_bot_answer:
 *	The bot's answer to whatever Rogue asks on the message line.
 */
int
rogue_bot_answer(void)
{
    char line[NUMCOLS + 1];
    const unsigned char *scr = rogue_screen();
    int i;

    for (i = 0; i < NUMCOLS; i++)
	line[i] = scr[i] & 0x7F;
    line[NUMCOLS] = '\0';

    /* Rogue will not take no for an answer when identifying. */
    if (strstr(line, "for list") != NULL)
    {
	if (bot_item == 0 && strstr(line, "identify") != NULL && pack != NULL)
	{
	    static int which;
	    THING *obj = pack;

	    for (i = which++ % inpack; i > 0 && next(obj) != NULL; i--)
		obj = next(obj);
	    return obj->o_packch;
	}
	i = bot_item != 0 ? bot_item : 27;
	bot_item = 0;
	return i;
    }
    if (strstr(line, "irection") != NULL)
	return dirs[rnd(8)];
    if (strstr(line, "hand") != NULL)
	return 'l';
    return 27;
}
#endif
