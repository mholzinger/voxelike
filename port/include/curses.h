#ifndef PORT_CURSES_H
#define PORT_CURSES_H

/* A curses small enough for Rogue: windows are plain character grids in
 * RAM and nothing is ever sent to a terminal. The voxel renderer and the
 * text layer read stdscr directly. */
#include <stdio.h>

typedef char bool;
typedef unsigned short chtype;

#define TRUE  1
#define FALSE 0
#define OK    0
#define ERR   (-1)

#define A_CHARTEXT 0x007F
#define A_STANDOUT 0x0080

typedef struct _win {
    short _cury, _curx;
    short _maxy, _maxx;
    short _begy, _begx;
    unsigned char _attr;      /* A_STANDOUT or 0, applied to new characters */
    unsigned char _sub;       /* cells belong to a parent window */
    unsigned char *_cells;    /* _maxy * _maxx, bit 7 is standout */
} WINDOW;

extern WINDOW *stdscr, *curscr;
extern int LINES, COLS;
extern int curses_changed;    /* bumped on every stdscr write */

WINDOW *initscr(void);
int     endwin(void);
WINDOW *newwin(int lines, int cols, int begy, int begx);
WINDOW *subwin(WINDOW *parent, int lines, int cols, int begy, int begx);
int     delwin(WINDOW *win);
int     mvwin(WINDOW *win, int y, int x);

int     wmove(WINDOW *win, int y, int x);
int     waddch(WINDOW *win, chtype ch);
int     waddstr(WINDOW *win, const char *s);
int     wprintw(WINDOW *win, const char *fmt, ...);
int     mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...);
int     printw(const char *fmt, ...);
int     mvprintw(int y, int x, const char *fmt, ...);
chtype  winch(WINDOW *win);
int     wclear(WINDOW *win);
int     werase(WINDOW *win);
int     wclrtoeol(WINDOW *win);
int     wrefresh(WINDOW *win);
int     touchwin(WINDOW *win);
int     clearok(WINDOW *win, bool flag);
int     leaveok(WINDOW *win, bool flag);
int     idlok(WINDOW *win, bool flag);
int     wstandout(WINDOW *win);
int     wstandend(WINDOW *win);
int     wgetnstr(WINDOW *win, char *buf, int n);
int     baudrate(void);
char   *unctrl(chtype ch);
char    killchar(void);
char    erasechar(void);

#define getyx(win, y, x)    ((y) = (win)->_cury, (x) = (win)->_curx)

#define addch(ch)           waddch(stdscr, ch)
#define addstr(s)           waddstr(stdscr, s)
#define move(y, x)          wmove(stdscr, y, x)
#define refresh()           wrefresh(stdscr)
#define clear()             wclear(stdscr)
#define erase()             werase(stdscr)
#define clrtoeol()          wclrtoeol(stdscr)
#define inch()              winch(stdscr)
#define standout()          wstandout(stdscr)
#define standend()          wstandend(stdscr)

#define mvwaddch(w, y, x, ch)   (wmove(w, y, x) == ERR ? ERR : waddch(w, ch))
#define mvwaddstr(w, y, x, s)   (wmove(w, y, x) == ERR ? ERR : waddstr(w, s))
#define mvwinch(w, y, x)        (wmove(w, y, x) == ERR ? (chtype)ERR : winch(w))
#define mvaddch(y, x, ch)       mvwaddch(stdscr, y, x, ch)
#define mvaddstr(y, x, s)       mvwaddstr(stdscr, y, x, s)
#define mvinch(y, x)            mvwinch(stdscr, y, x)

#endif
