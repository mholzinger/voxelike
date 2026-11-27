#include "mars.h"
#include "sega.h"
#include "sega_logo.h"

/* The whole picture, a pixel a byte and a line a row: the logo's lines are
 * three pixels wide at its largest, and a character is four by six. */
enum { W = 320, H = 224, FB_PIXELS = 0x100, CELL_W = 4, CELL_H = 6 };
enum { BLACK, SHAPE, INK };
/* Where in the shape the gap between the E and the G is. */
enum { SEGA_GAP_X = 146 };

/* S, E, G and A, three pixels by five, the high bit to the left. */
static const uint8_t glyph[4][CELL_H] = {
    { 3, 4, 2, 1, 6, 0 },
    { 7, 4, 6, 4, 7, 0 },
    { 3, 4, 5, 5, 3, 0 },
    { 2, 5, 7, 5, 5, 0 },
};

void sega_light(int light) {
    volatile uint16_t *cram = &MARS_CRAM;
    if (light < 0) light = 0;
    if (light > 31) light = 31;
    cram[BLACK] = 0;
    cram[SHAPE] = (uint16_t)(light << 10);                                       /* the logo's blue */
    cram[INK] = (uint16_t)(light << 10 | (22 * light / 31) << 5 | 16 * light / 31);   /* its letters, paler */
}

static inline int inked(const uint8_t shape[SEGA_H][SEGA_W / 8], unsigned sx, unsigned sy) {
    return shape[sy][sx >> 3] & (0x80 >> (sx & 7));
}

static inline int letter_at(unsigned sx) {
    return (sx >= sega_split[0]) + (sx >= sega_split[1]) + (sx >= sega_split[2]);
}

void sega_draw(int width) {
    volatile uint16_t *fb = &MARS_FRAMEBUFFER;
    for (unsigned y = 0; y < H; y++) fb[y] = (uint16_t)(FB_PIXELS + W / 2 * y);
    uint32_t *clear = (uint32_t *)(void *)(fb + FB_PIXELS);
    for (unsigned i = 0; i < W * H / 4; i += 4) {
        clear[i] = 0; clear[i + 1] = 0; clear[i + 2] = 0; clear[i + 3] = 0;
    }

    if (width < SEGA_SMALL) width = SEGA_SMALL;
    if (width >= SEGA_GONE) return;
    /* A character's place is the same whatever the size: the four the
     * logo starts as are where the smallest logo is. */
    const int col0 = ((W - SEGA_SMALL) / 2) % CELL_W, row0 = ((H - 5) / 2) % CELL_H;
    /* Steps across the shape for one pixel of the picture, Q16. */
    uint32_t dx = ((uint32_t)SEGA_W << 16) / (uint32_t)width;
    uint8_t *pixels = (uint8_t *)(void *)(fb + FB_PIXELS);

    if (width < SEGA_WHOLE) {
        /* Small, it is whole characters, an even number across so that
         * they stay about the middle: four are the word, a character a
         * letter. A character is there if enough of its part of the shape
         * is, by sixteen places in it. */
        int across = (width + CELL_W) / (2 * CELL_W) * 2;
        int down = (across * CELL_W * SEGA_H + SEGA_W * CELL_H / 2) / (SEGA_W * CELL_H);
        if (down < 1) down = 1;
        int x0 = (W - SEGA_SMALL) / 2 - (across - 4) / 2 * CELL_W;
        int y0 = (H - 5) / 2 - (down - 1) / 2 * CELL_H;
        for (int j = 0; j < down; j++) {
            for (int i = 0; i < across; i++) {
                int in = 0;
                for (int k = 0; k < 16; k++) {
                    unsigned sx = (unsigned)((i * 8 + 1 + (k & 3) * 2) * SEGA_W) / (unsigned)(across * 8);
                    unsigned sy = (unsigned)((j * 8 + 1 + (k >> 2) * 2) * SEGA_H) / (unsigned)(down * 8);
                    if (inked(sega_solid, sx, sy)) in++;
                }
                if (in < 6) continue;
                const uint8_t *rows = glyph[letter_at((unsigned)((2 * i + 1) * SEGA_W) / (unsigned)(2 * across))];
                for (int gy = 0; gy < CELL_H; gy++) {
                    uint8_t *line = pixels + (y0 + j * CELL_H + gy) * W + x0 + i * CELL_W;
                    for (int gx = 0; gx < CELL_W; gx++) line[gx] = (rows[gy] << gx) & 4 ? INK : SHAPE;
                }
            }
        }
        return;
    }

    /* Larger, the shape is cut to the pixel and the characters are what
     * it is made of. Its lines are left filled in until they are wide
     * enough to be there in every row.
     *
     * Larger than the picture, it is the logo come past the eye, which
     * is making for the gap between the E and the G: the place in the
     * shape that is at the middle of the picture goes from the shape's
     * own middle to there as it passes full size. */
    const int lined = width >= SEGA_LINED;
    const uint16_t *rows = lined ? sega_shape_row : sega_filled_row;
    const sega_run_t *shape = lined ? sega_shape_run : sega_filled_run;
    int past = width <= SEGA_FULL ? 0 : width >= 2 * SEGA_FULL ? 256 : (width - SEGA_FULL) * 256 / SEGA_FULL;
    uint32_t aim_x = ((uint32_t)(SEGA_W / 2) << 16) - (uint32_t)(past * (((SEGA_W / 2 - SEGA_GAP_X) << 16) / 256));
    uint32_t aim_y = (uint32_t)(SEGA_H / 2) << 16;
    /* The rows and columns of the picture the shape is in. */
    int x_from = W / 2 - (int)(aim_x / dx), y_from = H / 2 - (int)(aim_y / dx);
    int x_to = W / 2 + (int)((((uint32_t)SEGA_W << 16) - aim_x) / dx), y_to = H / 2 + (int)((((uint32_t)SEGA_H << 16) - aim_y) / dx);
    if (x_from < 0) x_from = 0;
    if (y_from < 0) y_from = 0;
    if (x_to > W) x_to = W;
    if (y_to > H) y_to = H;
    /* A row of the shape is stretches of ink, which the header has ready,
     * and each is a stretch of the picture's columns: worked out once for
     * all the rows of the picture that show that row of the shape. A
     * stretch is filled a character's row of pixels at a time. */
    struct { int16_t from, to; uint8_t letter; } runs[32];
    int count = 0, held = -1;
    /* Columns of the picture to one of the shape, Q8: multiplied by, where
     * dividing by dx would be a call for every stretch. */
    const int32_t across = (int32_t)(((uint32_t)width << 8) / SEGA_W);
    const int32_t aim = (int32_t)(aim_x >> 8);
    for (int y = y_from; y < y_to; y++) {
        int32_t at = (int32_t)aim_y + (y - H / 2) * (int32_t)dx;
        if (at < 0 || at >= (SEGA_H << 16)) continue;
        int sy = (int)((uint32_t)at >> 16);
        if (sy != held) {
            held = sy;
            count = 0;
            for (int k = rows[sy]; k < rows[sy + 1] && count < 32; k++) {
                unsigned from = shape[k].from, to = shape[k].to;
                /* About the first column of the picture at or past a place in the shape. */
                int xa = W / 2 + (int16_t)(((uint32_t)(((int32_t)(from << 8) - aim) * across + 0xFFFF)) >> 16);
                int xb = W / 2 + (int16_t)(((uint32_t)(((int32_t)((to & 0xFFF) << 8) - aim) * across + 0xFFFF)) >> 16);
                if (xa < x_from) xa = x_from;
                if (xb > x_to) xb = x_to;
                if (xa >= xb) continue;
                runs[count].from = (int16_t)xa; runs[count].to = (int16_t)xb; runs[count].letter = (uint8_t)(to >> 12);
                count++;
            }
        }
        if (!count) continue;
        /* The row's pixels of each letter, by where in a character: four
         * bytes that are one word of the picture. */
        int gy = (y - row0 + CELL_H) % CELL_H;
        union { uint8_t byte[CELL_W]; uint32_t word; } ink[4];
        for (int l = 0; l < 4; l++) {
            unsigned row = glyph[l][gy];
            for (int gx = 0; gx < CELL_W; gx++) ink[l].byte[(gx + col0) & (CELL_W - 1)] = (row << gx) & 4 ? INK : SHAPE;
        }
        uint8_t *line = pixels + y * W;
        for (int k = 0; k < count; k++) {
            int x = runs[k].from, to = runs[k].to;
            const uint8_t *byte = ink[runs[k].letter].byte;
            uint32_t word = ink[runs[k].letter].word;
            for (; x < to && (x & 3); x++) line[x] = byte[x & 3];
            for (uint32_t *four = (uint32_t *)(void *)(line + x); x + 4 <= to; x += 4) *four++ = word;
            for (; x < to; x++) line[x] = byte[x & 3];
        }
    }
}
