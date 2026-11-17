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

/*
 * rogue_screen:
 *	The cells of the main screen, for the renderer.
 */
const unsigned char *
rogue_screen(void)
{
    return stdscr->_cells;
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

/*
 * rogue_status:
 *	The stats of status(), packed to fit a 40 column screen.
 */
void
rogue_status(char *buf)
{
    static char *state_name[] =
    {
	"", "Hungry", "Weak", "Faint"
    };

    sprintf(buf, "Lv%d $%d Hp%d/%d St%d Ac%d Xp%d/%ld %s",
	level, purse, pstats.s_hpt, max_hp, pstats.s_str,
	10 - (cur_armor != NULL ? cur_armor->o_arm : pstats.s_arm),
	pstats.s_lvl, (long) pstats.s_exp, state_name[hungry_state]);
}

/*
 * rogue_pack:
 *	Describe the index'th thing in the pack as "a) name", at most
 *	max characters. Returns its pack letter, or 0 past the end.
 */
int
rogue_pack(int index, char *buf, int max)
{
    THING *obj;

    for (obj = pack; obj != NULL && index > 0; obj = next(obj))
	index--;
    if (obj == NULL)
	return 0;
    sprintf(buf, "%c) %.*s", obj->o_packch, max - 3, inv_name(obj, FALSE));
    return obj->o_packch;
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

    if (rp == NULL || (rp->r_flags & (ISGONE | ISDARK)) || on(player, ISBLIND))
	return FALSE;
    r[0] = rp->r_pos.x;
    r[1] = rp->r_pos.y;
    r[2] = rp->r_pos.x + rp->r_max.x - 1;
    r[3] = rp->r_pos.y + rp->r_max.y - 1;
    return TRUE;
}
