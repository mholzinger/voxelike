#ifndef PLAT_H
#define PLAT_H

/* What the Rogue port needs from the machine. Implemented in sh_src. */
unsigned long plat_ticks(void);        /* vblanks since power on */
int           plat_getkey(void);       /* block until the player gives a command */
void          plat_exit(int status);   /* the game is over */
void          plat_descend(void);      /* a valid stair descent is about to generate a floor */
void          plat_record_kill(int type);

/* A curses window was refreshed. cells is lines * cols characters, bit 7
 * set on standout ones. main_screen is nonzero for Rogue's 80x24 screen. */
void plat_show(const unsigned char *cells, int lines, int cols, int main_screen);

/* What the machine calls. */
void rogue_start(void);
void rogue_play(void);
const unsigned char *rogue_screen(void);   /* the 80x24 screen's cells */
int  rogue_under(int x, int y);            /* level character at a screen cell */
unsigned long rogue_who(int x, int y);     /* the monster at a screen cell, or 0 */
int  rogue_alive(unsigned long who);       /* is that monster still in the level */
int  rogue_hp(unsigned long who, int *maximum);          /* its hit points; 1 asks for the player's */
int  rogue_combat_rank(unsigned long who);   /* Rogue's combat level, or 0 if gone */
int  rogue_depth(void);                    /* dungeon level */
const char *rogue_monster_name(int kind);  /* 1 through 26, A through Z */
int  rogue_room_id(void);                  /* which room the player is in, -1 in the passages */
void rogue_tally(int *gold, int *things, int *rank);   /* purse, pack count, experience level */
int  rogue_ground(int x, int y);           /* how high a cell's ground stands, in voxels */
unsigned long rogue_attacker(void);        /* next monster that struck at the player, or 0 */
#ifdef BOT
int  rogue_bot_key(void);                  /* test builds: the next command, 0 to stand */
int  rogue_bot_answer(void);               /* test builds: the answer to a prompt */
#endif
void rogue_gear(int *armor, int *weapon);  /* Rogue's numbers for what is worn and wielded, -1 for nothing */
void rogue_idle(int on);                   /* turns that pass by themselves cost no food */
#ifdef SNAP
int  rogue_snap_station(int n);          /* stand the hero at a station of the baked level */
const char *rogue_snap_name(int n);      /* what that station is called, 0 past the last */
#endif
int  rogue_room(int *bounds);              /* lit room the player is in, as x0 y0 x1 y1 */
int  rogue_cursor_row(void);               /* 0 while Rogue waits on a prompt */
typedef struct {
    int depth, gold, hp, hp_max, strength, armor, rank, experience, hunger;
} rogue_status_t;
void rogue_status(rogue_status_t *status);
int  rogue_pack(int index, char *buf, int max);   /* Name of the index'th matching item. Returns its pack letter or 0. */
int  rogue_pickup_command(int slot);       /* controller shortcut, or 0 */

#endif
