/* Curses for Rogue on the 32X: every window is a character grid in RAM. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <curses.h>
#include "plat.h"

#define SCR_LINES 24
#define SCR_COLS  80

int LINES = SCR_LINES, COLS = SCR_COLS;
int curses_changed;

static unsigned char stdscr_cells[SCR_LINES * SCR_COLS];
static WINDOW stdscr_win;

/* Rogue only uses curscr to read and restore the cursor position, so it
 * can be the same window as stdscr. */
WINDOW *stdscr, *curscr;

static unsigned char *cell(WINDOW *w, int y, int x) {
    return &w->_cells[y * w->_maxx + x];
}

static void touched(WINDOW *w) {
    if (w == stdscr) curses_changed++;
}

WINDOW *initscr(void) {
    stdscr_win._maxy = SCR_LINES;
    stdscr_win._maxx = SCR_COLS;
    stdscr_win._cells = stdscr_cells;
    stdscr = curscr = &stdscr_win;
    wclear(stdscr);
    return stdscr;
}

int endwin(void) { return OK; }

WINDOW *newwin(int lines, int cols, int begy, int begx) {
    if (lines <= 0) lines = SCR_LINES - begy;
    if (cols <= 0) cols = SCR_COLS - begx;

    WINDOW *w = calloc(1, sizeof(WINDOW));
    if (!w) return NULL;
    w->_cells = malloc((size_t)(lines * cols));
    if (!w->_cells) { free(w); return NULL; }
    w->_maxy = (short)lines;
    w->_maxx = (short)cols;
    w->_begy = (short)begy;
    w->_begx = (short)begx;
    werase(w);
    return w;
}

/* Rogue's one subwin call (the inventory overlay) never relies on sharing
 * cells with its parent, so a subwindow is an independent window. */
WINDOW *subwin(WINDOW *parent, int lines, int cols, int begy, int begx) {
    (void)parent;
    return newwin(lines, cols, begy, begx);
}

int delwin(WINDOW *w) {
    if (!w || w == stdscr) return ERR;
    free(w->_cells);
    free(w);
    return OK;
}

int mvwin(WINDOW *w, int y, int x) {
    w->_begy = (short)y;
    w->_begx = (short)x;
    return OK;
}

int wmove(WINDOW *w, int y, int x) {
    if (y < 0 || y >= w->_maxy || x < 0 || x >= w->_maxx) return ERR;
    w->_cury = (short)y;
    w->_curx = (short)x;
    return OK;
}

int wclrtoeol(WINDOW *w) {
    for (int x = w->_curx; x < w->_maxx; x++) *cell(w, w->_cury, x) = ' ';
    touched(w);
    return OK;
}

int werase(WINDOW *w) {
    memset(w->_cells, ' ', (size_t)(w->_maxy * w->_maxx));
    w->_cury = w->_curx = 0;
    touched(w);
    return OK;
}

int wclear(WINDOW *w) { return werase(w); }

static void newline(WINDOW *w) {
    w->_curx = 0;
    if (w->_cury < w->_maxy - 1) w->_cury++;
}

int waddch(WINDOW *w, chtype ch) {
    unsigned char c = (unsigned char)(ch & A_CHARTEXT);

    switch (c) {
    case '\n':
        wclrtoeol(w);
        newline(w);
        return OK;
    case '\r':
        w->_curx = 0;
        return OK;
    case '\b':
        if (w->_curx > 0) w->_curx--;
        return OK;
    case '\t':
        do waddch(w, ' '); while (w->_curx & 7);
        return OK;
    }

    *cell(w, w->_cury, w->_curx) = c | (unsigned char)((ch & A_STANDOUT) | w->_attr);
    if (++w->_curx >= w->_maxx) newline(w);
    touched(w);
    return OK;
}

int waddstr(WINDOW *w, const char *s) {
    while (*s) waddch(w, (unsigned char)*s++);
    return OK;
}

chtype winch(WINDOW *w) {
    return *cell(w, w->_cury, w->_curx);
}

static int vwprintw(WINDOW *w, const char *fmt, va_list ap) {
    char buf[512];
    vsprintf(buf, fmt, ap);
    return waddstr(w, buf);
}

int wprintw(WINDOW *w, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vwprintw(w, fmt, ap);
    va_end(ap);
    return r;
}

int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...) {
    va_list ap;
    if (wmove(w, y, x) == ERR) return ERR;
    va_start(ap, fmt);
    int r = vwprintw(w, fmt, ap);
    va_end(ap);
    return r;
}

int printw(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vwprintw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

int mvprintw(int y, int x, const char *fmt, ...) {
    va_list ap;
    if (wmove(stdscr, y, x) == ERR) return ERR;
    va_start(ap, fmt);
    int r = vwprintw(stdscr, fmt, ap);
    va_end(ap);
    return r;
}

/* The platform decides how a window reaches the display. */
int wrefresh(WINDOW *w) {
    plat_show(w->_cells, w->_maxy, w->_maxx, w == stdscr);
    return OK;
}

int touchwin(WINDOW *w) { (void)w; return OK; }
int clearok(WINDOW *w, bool flag) { (void)w; (void)flag; return OK; }
int leaveok(WINDOW *w, bool flag) { (void)w; (void)flag; return OK; }
int idlok(WINDOW *w, bool flag) { (void)w; (void)flag; return OK; }

int wstandout(WINDOW *w) { w->_attr = A_STANDOUT; return OK; }
int wstandend(WINDOW *w) { w->_attr = 0; return OK; }

/* Line input needs a keyboard. Until there is an on-screen one, a prompt
 * for text (naming an item, "press return" under the tombstone) waits for
 * one press and gets an empty answer. */
int wgetnstr(WINDOW *w, char *buf, int n) {
    (void)w; (void)n;
    plat_getkey();
    buf[0] = 0;
    return OK;
}

int baudrate(void) { return 9600; }

char *unctrl(chtype ch) {
    static char buf[3];
    unsigned char c = (unsigned char)(ch & A_CHARTEXT);
    if (c < ' ') { buf[0] = '^'; buf[1] = (char)(c + '@'); buf[2] = 0; }
    else if (c == 127) { buf[0] = '^'; buf[1] = '?'; buf[2] = 0; }
    else { buf[0] = (char)c; buf[1] = 0; }
    return buf;
}

char killchar(void) { return 21; }    /* ^U */
char erasechar(void) { return 8; }    /* ^H */
