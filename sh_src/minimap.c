#include "mars.h"
#include "text.h"
#include "voxel.h"
#include "minimap.h"

/* A tile is 4 by 4 cells, each cell 2 by 2 pixels. */
#define TILES_W   (MAP_W / 4)              /* 20 */
#define TILES_H   (MAP_H / 4)              /* 6 */
#define N_TILES   (TILES_W * TILES_H)
#define TILE0     256                      /* first pattern, after font and HUD */
#define AT_X      (TEXT_W - 1 - TILES_W)   /* flush with the HUD's right edge */
#define AT_Y      0

/* Tiles looked at and tiles sent in one call. A tile sent is 17 words
 * through the 68000. */
#define LOOK      24
#define SEND      3

/* What a cell shows as. Colours are the HUD palette's. */
enum { NOTHING, FLOOR, WALL, STAIRS, HERO, MONSTER };
#define DIM       0x3                      /* frame */
#define GREEN     0x9
#define IVORY     0x1
#define RED       0x5                      /* the heart's */

/* Rogue's top line is messages and its bottom line is the status. */
static unsigned kind_of(const unsigned char *screen, int x, int y) {
    if (y == 0 || y == MAP_H - 1) return NOTHING;
    switch (screen[y * MAP_W + x] & 0x7F) {
    case ' ': return NOTHING;
    case '@': return HERO;
    case '%': return STAIRS;
    case '|': case '-': case '+': case '#': return WALL;
    default:
        /* A letter is a monster, and Rogue only draws those the hero can
         * see: the ones in the room with him, or beside him in the dark. */
        if ((screen[y * MAP_W + x] & 0x7F) >= 'A' && (screen[y * MAP_W + x] & 0x7F) <= 'Z') return MONSTER;
        return FLOOR;                      /* floor, and whatever lies on it */
    }
}

/* A tile's sixteen cells, four bits each and a row of cells to sixteen
 * bits, to tell when it has changed. No tile is all ones. The SH-2 shifts
 * by a variable amount with a library call, so every shift here is fixed. */
typedef struct { uint16_t row[4]; } cells_t;
static cells_t up[N_TILES];
static int started, primed, next, to_look;

void minimap_changed(void) {
    to_look = N_TILES;
}

/* A cell's two pixels on its upper line and on its lower one. Floor is
 * every other pixel, so that the picture behind shows through it. */
static const uint8_t upper[8] = { 0, DIM << 4, DIM << 4 | DIM, GREEN << 4 | GREEN, IVORY << 4 | IVORY, RED << 4 | RED };
static const uint8_t lower[8] = { 0, DIM,      DIM << 4 | DIM, GREEN << 4 | GREEN, IVORY << 4 | IVORY, RED << 4 | RED };

static void send(int tile, const cells_t *cells) {
    HwMdSetOffset((TILE0 + tile) * 32);
    for (int cy = 0; cy < 4; cy++) {
        unsigned row = cells->row[cy];
        unsigned a = (row >> 12) & 7, b = (row >> 8) & 7, c = (row >> 4) & 7, d = row & 7;
        HwMdSetVram((uint16_t)(upper[a] << 8 | upper[b]));
        HwMdSetVram((uint16_t)(upper[c] << 8 | upper[d]));
        HwMdSetVram((uint16_t)(lower[a] << 8 | lower[b]));
        HwMdSetVram((uint16_t)(lower[c] << 8 | lower[d]));
    }
}

void minimap_update(const unsigned char *screen) {
    if (!started) {
        started = 1;
        for (int i = 0; i < N_TILES; i++)
            for (int cy = 0; cy < 4; cy++) up[i].row[cy] = 0xFFFF;
        to_look = N_TILES;
    }
    int sent = 0;
    for (int looked = 0; to_look > 0 && looked < LOOK && sent < SEND; looked++) {
        int tx = next % TILES_W, ty = next / TILES_W;
        cells_t cells;
        int changed = 0;
        for (int cy = 0; cy < 4; cy++) {
            unsigned row = 0;
            for (int cx = 0; cx < 4; cx++)
                row = row << 4 | kind_of(screen, tx * 4 + cx, ty * 4 + cy);
            cells.row[cy] = (uint16_t)row;
            changed |= row != up[next].row[cy];
        }
        if (changed) {
            send(next, &cells);
            up[next] = cells;
            sent++;
        }
        next = (next + 1) % N_TILES;
        to_look--;
    }
    /* Every pattern has been sent once: nothing of what video memory held
     * before can show now. */
    if (!to_look) primed = 1;
}

void minimap_place(void) {
    if (!primed) return;
    for (int i = 0; i < N_TILES; i++)
        text_tile(AT_X + i % TILES_W, AT_Y + i / TILES_W, TEXT_HUD | (TILE0 + i));
}
