/* Everything Rogue 5.4.4 expects from main.c, mach_dep.c, mdport.c and
 * save.c, rewritten for a console. rnd, roll, playit, quit and the startup
 * sequence follow the originals in main.c:
 *
 * Rogue: Exploring the Dungeons of Doom
 * Copyright (C) 1980-1983, 1985, 1999 Michael Toy, Ken Arnold and Glenn Wichman
 * All rights reserved. See rogue/LICENSE.TXT.
 */
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <curses.h>
#include "rogue.h"
#include "score.h"
#include "plat.h"
#include "death_style.h"

unsigned int numscores = 10;
char *Numname = "Ten";
bool allscore = TRUE;

/*
 * rogue_start:
 *	main() up to the point where the first level exists.
 */
void
rogue_start(void)
{
    strcpy(whoami, "Rodney");
    dnum = (int) time(NULL);
    seed = dnum;

    initscr();				/* Start up cursor package */
    init_probs();			/* Set up prob tables for objects */
    init_player();			/* Set up initial player stats */
    init_names();			/* Set up names of scrolls */
    init_colors();			/* Set up colors of potions */
    init_stones();			/* Set up stone settings of rings */
    init_materials();			/* Set up materials of wands */

    /*
     * Lists go to a page of their own; the overlay style needs a wider
     * screen than the console has.
     */
    inv_type = INV_CLEAR;

    hw = newwin(LINES, COLS, 0, 0);
    new_level();			/* Draw current level */
    /*
     * Start up daemons and fuses
     */
    start_daemon(runners, 0, AFTER);
    start_daemon(doctor, 0, AFTER);
    fuse(swander, 0, WANDERTIME, AFTER);
    start_daemon(stomach, 0, AFTER);
}

/*
 * rogue_play:
 *	The rest of main(). Returns only through exit().
 */
void
rogue_play(void)
{
    playit();
}

/*
 * rnd:
 *	Pick a very random number.
 */
int
rnd(int range)
{
    return range == 0 ? 0 : abs((int) RN) % range;
}

/*
 * roll:
 *	Roll a number of dice
 */
int
roll(int number, int sides)
{
    int dtotal = 0;

    while (number--)
	dtotal += rnd(sides)+1;
    return dtotal;
}

/*
 * playit:
 *	The main loop of the program.  Loop until the game is over,
 *	refreshing things and looking at the proper times.
 */
void
playit()
{
    oldpos = hero;
    oldrp = roomin(&hero);
    while (playing)
	command();			/* Command execution */
    endit(0);
}

/*
 * quit:
 *	Have player make certain, then exit.
 */
void
quit(int sig)
{
    int oy, ox;

    NOOP(sig);

    if (!q_comm)
	mpos = 0;
    getyx(curscr, oy, ox);
    msg("really quit?");
    if (readchar() == 'y')
    {
	clear();
	mvprintw(LINES - 2, 0, "You quit with %d gold pieces", purse);
	move(LINES - 1, 0);
	refresh();
	score(purse, 1, 0);
	my_exit(0);
    }
    else
    {
	move(0, 0);
	clrtoeol();
	status();
	move(oy, ox);
	refresh();
	mpos = 0;
	count = 0;
	to_death = FALSE;
    }
}

/*
 * fatal:
 *	Exit the program, printing a message.
 */
void
fatal(char *s)
{
    clear();
    move(LINES - 2, 0);
    printw("%s", s);
    refresh();
    my_exit(0);
}

void
endit(int sig)
{
    NOOP(sig);
    fatal("Okay, bye bye!\n");
}

void
leave(int sig)
{
    NOOP(sig);
    my_exit(0);
}

void
my_exit(int st)
{
    exit(st);
}

void
shell()
{
    msg("there is no shell in this dungeon");
}

/*
 * No terminal, no users, no files.
 */
int	md_readchar()		{ return plat_getkey(); }
int	md_killchar()		{ return killchar(); }
int	md_erasechar()		{ return erasechar(); }
int	md_hasclreol()		{ return 0; }
int	md_getuid()		{ return 0; }
char	*md_getrealname(int uid) { NOOP(uid); return whoami; }
void	md_raw_standout()	{ }
void	md_raw_standend()	{ }
void	flush_type()		{ }
void	resetltchars(void)	{ }

/*
 * Scores and saves need battery RAM. Until then the score list is empty
 * every run and saving reports that it cannot be done.
 */
void	start_score()		{ }
bool	lock_sc()		{ return TRUE; }
void	unlock_sc()		{ }
void	rd_score(SCORE *top_ten) { NOOP(top_ten); }
void	wr_score(SCORE *top_ten) { NOOP(top_ten); }

void
save_game()
{
    mpos = 0;
    msg("this dungeon cannot be saved yet");
}

#ifdef SNAP
/* make SNAP=1: the level in port/snap_data.h, which tools/snap.py takes
 * out of a save state, is what the renderer is shown, whatever is played.
 * make SNAP=2: the level in port/snap_test.h, which tools/testlevel.py
 * makes, with every case the renderers have to draw. The hero is stood at
 * each of the level's stations in turn. See tools/snapbench.py. */
#if SNAP == 2
#include "snap_test.h"
#else
#include "snap_data.h"
#endif
static unsigned char snap_now[NUMLINES * NUMCOLS];
int snap_at = -1;

/*
 * rogue_snap_station:
 *	Stand the hero at a station of the level. TRUE if he was not
 *	there already.
 */
int
rogue_snap_station(int n)
{
    n %= SNAP_STATIONS;
    if (n == snap_at)
	return FALSE;
    snap_at = n;
    memcpy(snap_now, snap_screen, sizeof snap_now);
    snap_now[snap_stations[n][1] * NUMCOLS + snap_stations[n][0]] = '@';
    return TRUE;
}

/*
 * rogue_snap_name:
 *	What a station of the level is called, NULL past the last.
 */
const char *
rogue_snap_name(int n)
{
    return n < SNAP_STATIONS ? snap_names[n] : NULL;
}
#endif

/*
 * rogue_screen:
 *	The cells of the main screen, for the renderer.
 */
const unsigned char *
rogue_screen(void)
{
#ifdef SNAP
    if (snap_at < 0)
	rogue_snap_station(0);
    return snap_now;
#else
    return stdscr->_cells;
#endif
}

/*
 * rogue_under:
 *	What the level holds at a screen cell, under any monster or the
 *	player, whether or not it has been seen.
 */
int
rogue_under(int x, int y)
{
    if (x < 0 || x >= NUMCOLS || y < 1 || y >= NUMLINES - 1)
	return ' ';
#ifdef SNAP
    {
	/* The level is no more than is on the screen, which has what
	 * the hero stands on where he stands, and what else stands
	 * anywhere stands on floor. */
	int ch = snap_screen[y * NUMCOLS + x] & 0x7F;
	return (ch >= 'A' && ch <= 'Z') ? '.' : ch;
    }
#endif
    return chat(y, x);
}

/*
 * rogue_who:
 *	A number that stays with the monster at a screen cell for as long
 *	as it lives, so the renderer can tell a monster that moved from a
 *	different one. 0 if no monster is there.
 */
unsigned long
rogue_who(int x, int y)
{
    if (x < 0 || x >= NUMCOLS || y < 1 || y >= NUMLINES - 1)
	return 0;
    return (unsigned long) moat(y, x);
}

/*
 * rogue_alive:
 *	Is the monster rogue_who once named still in the level?
 */
int
rogue_alive(unsigned long who)
{
    THING *tp;

    for (tp = mlist; tp != NULL; tp = next(tp))
	if ((unsigned long) tp == who)
	    return TRUE;
    return FALSE;
}

/*
 * rogue_hp:
 *	Hit points of a monster that is alive, or of the player for 1.
 */
int
rogue_hp(unsigned long who, int *maximum)
{
    const struct stats *stats = who == 1 ? &pstats :
        rogue_alive(who) ? &((THING *) who)->t_stats : NULL;
    *maximum = stats ? stats->s_maxhp : 0;
    return stats ? stats->s_hpt : 0;
}

int
rogue_depth(void)
{
    return level;
}

int
rogue_combat_rank(unsigned long who)
{
    if (who == 1)
	return pstats.s_lvl;
    return rogue_alive(who) ? ((THING *)who)->t_stats.s_lvl : 0;
}

const char *
rogue_monster_name(int kind)
{
    return monsters[kind - 1].m_name;
}

/*
 * rogue_room_id:
 *	The number of the room the player is in. Passages, and the rooms
 *	that are only passages, are all -1.
 */
int
rogue_room_id(void)
{
    if (proom == NULL || proom < rooms || proom >= &rooms[MAXROOMS]
	|| (proom->r_flags & ISGONE))
	    return -1;
    return (int) (proom - rooms);
}

/*
 * rogue_tally:
 *	What the player has, for noticing when he gets more of it.
 */
void
rogue_tally(int *gold, int *things, int *rank)
{
    *gold = purse;
    *things = inpack;
    *rank = pstats.s_lvl;
}

/*
 * room_height:
 *	Rooms stand on plinths 2 to 4 voxels high, chosen so the rooms of
 *	a level differ and so do the levels. 0 if the cell is in no room.
 */
static int
room_height(int x, int y)
{
    struct room *rp;

    for (rp = rooms; rp < &rooms[MAXROOMS]; rp++)
    {
	if (rp->r_flags & (ISGONE | ISMAZE))
	    continue;
	if (x >= rp->r_pos.x && x < rp->r_pos.x + rp->r_max.x
	    && y >= rp->r_pos.y && y < rp->r_pos.y + rp->r_max.y)
		return 2 + ((int) (rp - rooms) * 5 + level) % 3;
    }
    return 0;
}

/*
 * rogue_ground:
 *	How high the ground of a cell stands. Passages run at ground level
 *	and step up halfway where they meet a room's door.
 */
int
rogue_ground(int x, int y)
{
    int h, n;

    if (x < 0 || x >= NUMCOLS || y < 1 || y >= NUMLINES - 1)
	return 0;
#ifdef SNAP
    return snap_lift[y * NUMCOLS + x];
#endif
    if ((h = room_height(x, y)) != 0)
	return h;
    h = room_height(x - 1, y);
    if ((n = room_height(x + 1, y)) > h) h = n;
    if ((n = room_height(x, y - 1)) > h) h = n;
    if ((n = room_height(x, y + 1)) > h) h = n;
    return h / 2;
}

/*
 * attack:
 *	The linker sends chase.c's calls to attack() here first, so the
 *	machine can show who struck at the player. Rogue's own attack()
 *	then runs untouched.
 */
#define MAXATTACKERS	8
static THING *attackers[MAXATTACKERS];
static int n_attackers;

extern int __real_attack(THING *mp);
static int attack_dx, attack_dy, bolt_fatal;
static death_blow_t fatal_bolt;

static int direction_sign(int delta)
{
    return (delta > 0) - (delta < 0);
}

void
rogue_fatal_bolt(const char *name, int dx, int dy)
{
    fatal_bolt.style = strcmp(name, "flame") == 0 ? DEATH_FLAME :
                       strcmp(name, "ice") == 0 ? DEATH_FROST : DEATH_SPARK;
    fatal_bolt.dx = direction_sign(dx);
    fatal_bolt.dy = direction_sign(dy);
    bolt_fatal = 1;
}

static death_style_t
death_style(char killer)
{
    /* Rogue gives monsters intrinsic attacks. These are animation choices. */
    switch (killer) {
        case 'O': return DEATH_CUT;
        case 'H': return DEATH_SLICES;
        case 'T': case 'Y': return DEATH_CRUSH;
        case 'F': case 'R': case 'S': case 'U':
        case 'a': case 'd': return DEATH_PIERCE;
        case 'W': case 'V': return DEATH_DRAIN;
        case 'h': case 's': return DEATH_COLLAPSE;
        default: return killer >= 'A' && killer <= 'Z' ? DEATH_CLAW : DEATH_COLLAPSE;
    }
}

int
__wrap_attack(THING *mp)
{
    if (n_attackers < MAXATTACKERS)
	attackers[n_attackers++] = mp;
    attack_dx = direction_sign(hero.x - mp->t_pos.x);
    attack_dy = direction_sign(hero.y - mp->t_pos.y);
    {
        int result = __real_attack(mp);
        attack_dx = attack_dy = 0;
        return result;
    }
}

void
rogue_note_kill(unsigned long id, int weapon, int launched)
{
    extern void vox_note_kill(unsigned long, death_style_t);
    death_style_t style = DEATH_CLAW;
    switch (weapon) {
        case SWORD: style = DEATH_CUT; break;
        case TWOSWORD: style = DEATH_SLICES; break;
        case MACE: style = DEATH_CRUSH; break;
        case BOW: style = DEATH_BOW_STRIKE; break;
        case ARROW: style = launched ? DEATH_ARROW_SHOT : DEATH_ARROW; break;
        case DAGGER: style = DEATH_DAGGER; break;
        case DART: style = DEATH_DART; break;
        case SHIRAKEN: style = DEATH_SHURIKEN; break;
        case SPEAR: style = DEATH_SPEAR; break;
    }
    vox_note_kill(id, style);
}

/* Play the fatal blow while Rogue's dungeon still occupies its screen. */
extern void __real_death(char monst);
extern void plat_death(death_blow_t blow);

void
__wrap_death(char monst)
{
    death_blow_t blow;
    blow.style = death_style(monst);
    blow.dx = attack_dx;
    blow.dy = attack_dy;
    if (bolt_fatal) blow = fatal_bolt;
    bolt_fatal = 0;
    plat_death(blow);
    __real_death(monst);
}

unsigned long
rogue_attacker(void)
{
    return n_attackers > 0 ? (unsigned long) attackers[--n_attackers] : 0;
}

/*
 * rogue_gear:
 *	Which armor is worn and which weapon wielded, as Rogue numbers
 *	them (LEATHER.., MACE..), or -1. Anything can be wielded; only a
 *	weapon counts as one here.
 */
void
rogue_gear(int *armor, int *weapon)
{
    *armor = cur_armor != NULL ? cur_armor->o_which : -1;
    *weapon = cur_weapon != NULL && cur_weapon->o_type == WEAPON
	? cur_weapon->o_which : -1;
}

/*
 * rogue_idle:
 *	The world moves on while the player stands and thinks. Those
 *	turns should not starve him, so the stomach is stopped for them.
 */
void
rogue_idle(int on)
{
    static int idle = FALSE;

    if (on && !idle)
	kill_daemon(stomach);
    else if (!on && idle)
	start_daemon(stomach, 0, AFTER);
    idle = on;
}

/*
 * rogue_cursor_row:
 *	Row the cursor was left on. Row 0 means Rogue is waiting for the
 *	answer to a prompt, anything else means it wants a command.
 */
int
rogue_cursor_row(void)
{
    return stdscr->_cury;
}

void
rogue_status(rogue_status_t *status)
{
    *status = (rogue_status_t){
	.depth = level, .gold = purse, .hp = pstats.s_hpt, .hp_max = max_hp,
	.strength = pstats.s_str,
	.armor = 10 - (cur_armor != NULL ? cur_armor->o_arm : pstats.s_arm),
	.rank = pstats.s_lvl, .experience = pstats.s_exp, .hunger = hungry_state,
    };
}

/*
 * get_item, watched:
 *	Rogue asks for something from the pack with a kind in mind, and
 *	its own list (the '*' answer) shows only that kind. The linker
 *	sends its calls here (-Wl,--wrap=get_item) so that the port's
 *	list can do the same. Anything can be wielded or thrown, so those
 *	two show everything.
 */
static int pick_type;

extern THING *__real_get_item(char *purpose, int type);

THING *
__wrap_get_item(char *purpose, int type)
{
    THING *obj;

    pick_type = strcmp(purpose, "wield") == 0 || strcmp(purpose, "throw") == 0
	? 0 : type;
    obj = __real_get_item(purpose, type);
    pick_type = 0;
    return obj;
}

/* The rule of Rogue's inventory(). */
static int
fits(THING *obj)
{
    if (pick_type == 0 || obj->o_type == pick_type)
	return TRUE;
    if (pick_type == CALLABLE)
	return obj->o_type != FOOD && obj->o_type != AMULET;
    if (pick_type == R_OR_S)
	return obj->o_type == RING || obj->o_type == STICK;
    return FALSE;
}

/*
 * rogue_pack:
 *	Describe the index'th thing in the pack that fits what Rogue is
 *	asking for, at most max characters. Returns its pack
 *	letter, or 0 past the end.
 */
int
rogue_pack(int index, char *buf, int max)
{
    THING *obj;

    for (obj = pack; obj != NULL; obj = next(obj))
	if (fits(obj) && index-- == 0)
	    break;
    if (obj == NULL)
	return 0;
    if (pick_type == SCROLL)
    {
	const struct obj_info *info = &scr_info[obj->o_which];
	const char *name = info->oi_know ? info->oi_name :
	    info->oi_guess ? info->oi_guess : s_names[obj->o_which];
	int prefix = 0;
	if (obj->o_count > 1)
	    prefix += sprintf(buf + prefix, "%dx ", obj->o_count);
	const char *label = info->oi_know ? "of " : info->oi_guess ? "called " : "";
	sprintf(buf + prefix, "%s%.*s", label, max - prefix - (int)strlen(label), name);
	return obj->o_packch;
    }
    sprintf(buf, "%.*s", max, inv_name(obj, FALSE));
    return obj->o_packch;
}

/* The controller shortcut for a freshly picked up pack item. */
int
rogue_pickup_command(int slot)
{
    THING *obj;

    for (obj = pack; obj != NULL; obj = next(obj))
    {
	if (obj->o_packch != slot)
	    continue;
	switch (obj->o_type)
	{
	    case FOOD: return 'e';
	    case POTION: return 'q';
	    case SCROLL: return 'r';
	    case WEAPON: return 'w';
	    case ARMOR: return 'W';
	    case RING: return 'P';
	    case STICK: return 'z';
	    default: return 0;
	}
    }
    return 0;
}

/*
 * rogue_room:
 *	Bounds of the lit room the player is in, walls included, as
 *	{ x0, y0, x1, y1 }. Returns FALSE in a passage or a dark room,
 *	where the player only sees what is next to him.
 */
int
rogue_room(int *r)
{
    struct room *rp = proom;

#ifdef SNAP
    {
	const unsigned char *at = snap_stations[snap_at < 0 ? 0 : snap_at];
	int i;

	if (!at[2])
	    return FALSE;
	for (i = 0; i < 4; i++)
	    r[i] = at[3 + i];
	return TRUE;
    }
#endif
    if (rp == NULL || (rp->r_flags & (ISGONE | ISDARK)) || on(player, ISBLIND))
	return FALSE;
    r[0] = rp->r_pos.x;
    r[1] = rp->r_pos.y;
    r[2] = rp->r_pos.x + rp->r_max.x - 1;
    r[3] = rp->r_pos.y + rp->r_max.y - 1;
    return TRUE;
}
