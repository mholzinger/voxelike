/* make POLY=1: the hero's lit room as flat polygons.
 *
 * The rays work every pixel out; this paints surfaces. A cell that is one
 * height all over is a box: a top and the sides that stand above their
 * neighbours, each one colour, floors in two shades cell and cell about.
 * Anything else (a door, stairs, a thing on
 * the floor) and every creature is drawn as its voxel columns, each column
 * a small box of whole words. Nothing is textured.
 *
 * Nothing is sorted either. Ground goes down first, since ground hides
 * nothing that stands. Then what stands is drawn cell by cell, the cells
 * farthest from the eye first along each axis, which is back to front for
 * boxes on a grid whatever the perspective.
 *
 * The primary SH-2 projects the corners of the cells and lists the
 * creatures while the secondary clears the picture with the fill hardware.
 * Then each draws its half of the screen in that same order. The
 * framebuffer takes no write while a fill runs, so spans are written by
 * the CPUs themselves. Rows are 512 bytes apart: a row is one 256-word
 * page, which a fill cannot cross.
 *
 * POLY=2 gives floors the bitmap the rays give them, eight by eight to a
 * cell, and no two shades. The camera does not roll, so level ground is
 * one depth along a row of the screen and a step across the row is one
 * step over the ground: a span is two adds and a look a sample, a sample
 * to a word, which is the two pixels of a column. It is a quad to a cell
 * still: the floor is two tiles turn and turn about, so no two cells side
 * by side share a bitmap. The tops of walls are done the same.
 *
 * It draws 320 by 112 whatever resolution is asked for. */
#ifdef POLY
#include "mars.h"
#include "string.h"
#include "persp.h"
#include "persp_poly.h"

#define FB_W      320
#define FB_H      VIEW_H
/* The primary draws left of a column and the secondary right of it. The
 * frame is done when the slower is, so the column goes toward whichever
 * took longer over the last frame: a room is seldom the same on both
 * sides of the middle. */
#define SPLIT_MIN   64
#define SPLIT_MAX   (FB_W - 64)
#define SPLIT_STEP  16                     /* the most it moves in a frame */
#define PAGE      256
#define FIRST     0x100                    /* words: after the line table */
#define BLACK     0x0101

/* The cells of a frame, and one more each way for their far corners. A
 * room of Rogue's is at most 26 by 8 with its walls, and the view has two
 * cells round that. */
#define VIEW_W_MAX 33
#define VIEW_H_MAX 14
#define FLOOR_MAX  1                       /* a slab no higher is ground: a wall is two */
#define MAX_SEEN   16                      /* creatures drawn in a frame */

#define DIVU_DVSR  (*(volatile uint32_t *)0xFFFFFF00)
#define DIVU_DVDNT (*(volatile uint32_t *)0xFFFFFF04)
#define SHARED(x)  (*(volatile __typeof__(x) *)((uintptr_t)&(x) | 0x20000000u))
#define FILL_WAIT() do { } while (MARS_VDP_FBCTL & MARS_VDP_FEN)

typedef struct { int16_t x, y; } point_t;

/* A corner of a cell on screen: its column, and focal / depth in Q17, by
 * which a height becomes a row. Zero for one too near to be shown. */
typedef struct { int32_t x; uint32_t scale; } corner_t;
static corner_t corner_grid[(VIEW_H_MAX + 1) * (VIEW_W_MAX + 1)];
/* The grid is as many corners as a room's cells have, but it is not kept
 * to a room's shape: a passage is drawn as far ahead of the eye as there
 * are corners for, whichever way the eye looks. */
#define corners_row(gy) (corner_grid + (gy) * frame.stride)

/* The frame, as the primary leaves it for both to draw from. */
static struct {
    int x0, y0, w, h;                      /* the cells in view */
    int stride;                            /* corners to a row of them */
    int eye_x, eye_y;                      /* the eye, in voxels */
    int32_t eye_h, horizon;
    int up, down;                          /* shifts that make scale Q17 */
    int32_t near;                          /* depth under which nothing is shown */
    int split;                             /* the column the two CPUs meet at */
    int pits;                              /* some cell in view is one: see is_pit() */
    int n_seen;
} frame;
static struct {
    vox_creature_t c;
    const vcol_t (*model)[CELL];
    uint8_t next;                          /* the next in the same cell, and one */
} seen[MAX_SEEN];
static uint8_t standing[VIEW_H_MAX * VIEW_W_MAX];   /* the first creature in a cell, and one */
static volatile uint16_t cleared, listed;
static volatile uint16_t secondary_ticks;  /* how long its half took it, by its own timer */
static int16_t split = FB_W / 2;

/* make POLY_OFF=n leaves a kind of thing undrawn, to find what it costs:
 * 1 the ground, 2 walls, 4 things drawn a column at a time, 8 all of it,
 * 16 the ledges outside walls.
 *
 * make POLY_PROF=1 keeps the time the primary CPU spends on each kind of
 * thing, in poly_prof, which a SNAP build keeps by station and
 * tools/snapbench.py prints. The clock is read twice for each thing drawn,
 * so the frame is a little slower than it is without. */
#ifndef POLY_OFF
#define POLY_OFF 0
#endif
#define OFF(n) (POLY_OFF & (n))
enum { PROF_FLOOR, PROF_GROUND, PROF_LEDGE, PROF_WALL, PROF_TERRAIN, PROF_CREATURE, PROF_DRAW, PROF_FRAME,
       PROF_OTHER_CPU, PROF_KINDS };
#ifdef POLY_PROF
__attribute__((used)) uint32_t poly_prof[PROF_KINDS];
#define PROF(kind, work) do { if (lo == 0) { uint16_t t_ = ticks(); work; poly_prof[kind] += (uint16_t)(ticks() - t_); } \
                              else { work; } } while (0)
#else
#define PROF(kind, work) do { work; } while (0)
#endif

static inline uint16_t ticks(void);
static inline uint16_t ticks(void) {
    uint8_t hi = SH2_FRT_FRCH, lo = SH2_FRT_FRCL;
    return (uint16_t)(hi << 8 | lo);
}

static uint16_t recip[256];                /* 65536 / n, from 2 */
/* By model: its height if it has one, the least height of its columns, and
 * the colours of its top and its side. */
static uint8_t model_flat[VOX_MODELS_MAX], model_base[VOX_MODELS_MAX];
static uint8_t model_top[VOX_MODELS_MAX], model_side[VOX_MODELS_MAX];
/* A model that is a box on part of its cell and nothing on the rest, as a
 * wall is: which quarters of the cell the box is on, a bit each (west then
 * east of the north, then of the south), and in the high four bits how
 * high it is. Zero for a model that is not one. */
static uint8_t model_part[VOX_MODELS_MAX];
/* How many of a model's voxels from the bottom are what its plinth is
 * made of: a wall is a course of the plinth's brick under its cap. The
 * side of a box is in the plinth's colour that much farther up, which is
 * all the pattern a side two voxels high has. */
static uint8_t model_foot[VOX_MODELS_MAX];

#if POLY >= 2
#define TEXTURED 1
#else
#define TEXTURED 0
#endif
#define WALL_TOPS TEXTURED

/* What one CPU has ready for bitmaps: the height it last drew one at, and
 * a few bitmaps as words in the light they were asked for in. A bitmap
 * here is two cells wide, the one over cells on even columns of the map
 * and the one over odd: a room's floor is two tiles turn and turn about,
 * and held so a row of it is one span. One tile is itself twice. A few,
 * because the rows of a floor are two by turns and so are a wall's tops.
 * One of these to a CPU: each is only ever seen through its own cache. */
#define TEX_HELD 4
typedef struct {
    int z;                                 /* -1 when the frame is new */
    uint32_t q;                            /* (eye - z) / focal, Q16 */
    const uint16_t *words;                 /* the bitmap in use */
    uint8_t next;                          /* which of those held goes next */
    struct { const uint8_t *even, *odd; uint32_t light; uint16_t words[2 * CELL * CELL]; } held[TEX_HELD];
} tex_t;
#if TEXTURED
static tex_t tex_of[2];

/* a * b in Q16: the middle of the 64-bit product, which the compiler
 * shifts out with a library call. */
static inline int32_t mul16(int32_t a, int32_t b) {
    int32_t hi, lo;
    __asm__("dmuls.l %2,%3\n\tsts mach,%0\n\tsts macl,%1\n\txtrct %0,%1"
            : "=&r"(hi), "=&r"(lo) : "r"(a), "r"(b) : "mach", "macl");
    return lo;
}

/* a * b >> 31, unsigned. */
static inline uint32_t mul31(uint32_t a, uint32_t b) {
    uint32_t hi, lo;
    __asm__("dmulu.l %2,%3\n\tsts mach,%0\n\tsts macl,%1\n\tshll %1\n\trotcl %0"
            : "=&r"(hi), "=&r"(lo) : "r"(a), "r"(b) : "mach", "macl", "t");
    return hi;
}

/* Where ground is at a pixel is in voxels, Q16, times sixteen one way and
 * twice the other, so that the high word of each holds its bits of the
 * bitmap, four across two cells and three down one, where the two can be
 * put together as they are. It is kept
 * unsigned: only those bits are wanted, and they come round right. */
#define TEXEL(u, v) (*(const uint16_t *)(const void *)((const uint8_t *)texels + \
                     ((((uint32_t)(u) >> 16) & 0xF0) | (((uint32_t)(v) >> 16) & 0x0E))))

static inline int32_t divide(int32_t n, int32_t d);

/* Make ready to draw a bitmap at a height in a light. */
static void tex_for(tex_t *tex, const uint8_t *even, const uint8_t *odd, unsigned light, int z) {
    if (z != tex->z) {
        tex->z = z;
        tex->q = (uint32_t)divide((frame.eye_h - z) << 16, persp_job.focal);
    }
    int n = 0;
    while (n < TEX_HELD && (tex->held[n].even != even || tex->held[n].odd != odd || tex->held[n].light != light)) n++;
    if (n == TEX_HELD) {
        n = tex->next;
        tex->next = (uint8_t)((n + 1) % TEX_HELD);
        tex->held[n].even = even;
        tex->held[n].odd = odd;
        tex->held[n].light = light;
        uint16_t *words = tex->held[n].words;
        for (int half = 0; half < 2; half++) {
            const uint8_t *top = half ? odd : even;
            /* A wall's bitmap is empty where the wall is not, and a top's
             * edge can fall a pixel into that: there it takes what is
             * across the cell from it. */
            for (int i = 0; i < CELL * CELL; i++) {
                unsigned v = top[i];
                if (!v && !(v = top[i ^ 4]) && !(v = top[i ^ 32])) v = top[i ^ 36];
                *words++ = (uint16_t)((v + light) * 0x0101u);
            }
        }
    }
    tex->words = tex->held[n].words;
}
#endif

void poly_init(void) {
    for (int n = 2; n < 256; n++) recip[n] = (uint16_t)(65536u / (unsigned)n);
    for (int m = 0; m < vox_model_count; m++) {
        const vmodel_t *model = &vox_models[m];
        unsigned base = COL_MAXH, flat = model->col[0][0].h;
        const vcol_t *lowest = &model->col[0][0];
        for (int x = 0; x < CELL; x++) {
            for (int y = 0; y < CELL; y++) {
                const vcol_t *col = &model->col[x][y];
                if (col->h != flat) flat = DETAIL;
                if (col->h < base) { base = col->h; lowest = col; }
            }
        }
        /* A box on a half or a quarter of the cell: every quarter all one
         * height or all empty, and those that stand side by side. */
        unsigned quarters = 0, high = 0, boxed = flat == DETAIL;
        const vcol_t *standing_at = 0;
        for (int x = 0; x < CELL && boxed; x++) {
            for (int y = 0; y < CELL; y++) {
                const vcol_t *col = &model->col[x][y];
                if (col->h > high) { high = col->h; standing_at = col; }
            }
        }
        for (int x = 0; x < CELL && boxed; x++) {
            for (int y = 0; y < CELL; y++) {
                unsigned h = model->col[x][y].h, bit = 1u << ((y >= CELL / 2) * 2 + (x >= CELL / 2));
                unsigned first = model->col[x & 4][y & 4].h;
                if ((h && h != high) || h != first) boxed = 0;
                if (h) quarters |= bit;
            }
        }
        if (quarters != 1 && quarters != 2 && quarters != 4 && quarters != 8 &&
            quarters != 3 && quarters != 12 && quarters != 5 && quarters != 10) boxed = 0;
        model_part[m] = boxed ? (uint8_t)(high << 4 | quarters) : 0;

        const vcol_t *middle = boxed ? standing_at : flat == DETAIL ? lowest : &model->col[3][3];
        model_flat[m] = (uint8_t)flat;
        model_base[m] = (uint8_t)base;
        model_side[m] = middle->h ? middle->c[middle->h - 1] : 0;
        model_foot[m] = 0;
        while (model_foot[m] < middle->h && middle->c[model_foot[m]] == vox_plinth[m]) model_foot[m]++;
        /* A top is patterned, and takes the colour most of it is: the two
         * floors are one pattern turned round, and must come out alike. */
        uint8_t count[256];
        unsigned most = 0;
        memset(count, 0, sizeof count);
        model_top[m] = 0;
        unsigned top_at = boxed ? high : base;
        for (int x = 0; x < CELL && top_at; x++) {
            for (int y = 0; y < CELL; y++) {
                const vcol_t *col = &model->col[x][y];
                if (col->h != top_at) continue;
                unsigned colour = col->c[top_at - 1];
                if (++count[colour] > most) { most = count[colour]; model_top[m] = (uint8_t)colour; }
            }
        }
    }
}

static inline int32_t divide(int32_t n, int32_t d) {
    DIVU_DVSR = (uint32_t)d;
    DIVU_DVDNT = (uint32_t)n;
    return (int32_t)DIVU_DVDNT;
}

/* focal / depth in Q17, for a depth in Q16 that is not under frame.near. */
static inline uint32_t scale_at(int32_t t) {
    return (uint32_t)divide((int32_t)persp_job.focal << frame.up, (int32_t)((uint32_t)t >> frame.down));
}

/* The row a height shows on at a corner. */
static inline int row_at(const corner_t *at, int z) {
    return (int)frame.horizon + (int)(((uint32_t)(frame.eye_h - z) * at->scale) >> 17);
}

/* One divide a corner. Depth and offset are level along a row of corners,
 * so they are stepped, not multiplied. */
static void project(void) {
    const sh2z_fx_t s = persp_job.s, c = persp_job.c;
    for (int gy = 0; gy <= frame.h; gy++) {
        int32_t rx = ((frame.x0 * CELL) << 16) - persp_job.ex;
        int32_t ry = (((frame.y0 + gy) * CELL) << 16) - persp_job.ey;
        int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
        int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
        corner_t *at = corners_row(gy);
        for (int gx = 0; gx <= frame.w; gx++, at++, t += CELL * c, u -= CELL * s) {
            if (t < frame.near) {
                at->scale = 0;
                continue;
            }
            at->scale = scale_at(t);
            /* A column is two pixels: twice u * focal / depth. */
            at->x = FB_W / 2 + (int32_t)(((int64_t)u * (int32_t)at->scale) >> 32);
        }
    }
}

/* The creatures, each hung on the cell its middle is over. */
static void list_creatures(void) {
    vox_creature_t c;
    memset(standing, 0, sizeof standing);
    frame.n_seen = 0;
    /* The stairs are in a pit, which takes more care to draw. Most rooms
     * have none, and pay nothing for it. */
    frame.pits = 0;
    for (int cy = frame.y0; cy < frame.y0 + frame.h && !frame.pits; cy++) {
        for (int cx = frame.x0; cx < frame.x0 + frame.w; cx++) {
            if (persp_cells[cy][cx].ground != DETAIL || model_part[vox_tile[cy][cx]]) continue;
            unsigned lift = vox_lift[cy][cx];
            if ((cx > 0 && vox_lift[cy][cx - 1] > lift) || (cx + 1 < MAP_W && vox_lift[cy][cx + 1] > lift) ||
                (cy > 0 && vox_lift[cy - 1][cx] > lift) || (cy + 1 < MAP_H && vox_lift[cy + 1][cx] > lift)) {
                frame.pits = 1;
                break;
            }
        }
    }
    if (persp_actors_hidden()) return;
    for (int i = 0; vox_creature(i, &c) && frame.n_seen < MAX_SEEN; i++) {
        int cx = ((c.x + CELL / 2) >> 3) - frame.x0, cy = ((c.y + CELL / 2) >> 3) - frame.y0;
        if ((unsigned)cx >= (unsigned)frame.w || (unsigned)cy >= (unsigned)frame.h) continue;
        int n = frame.n_seen++;
        seen[n].c = c;
        seen[n].model = persp_creature_model(&c);
        seen[n].next = standing[cy * frame.w + cx];
        standing[cy * frame.w + cx] = (uint8_t)(n + 1);
    }
}

static inline int32_t slope(int dx, int dy) {
    return dy == 1 ? dx << 16 : dy < 256 ? dx * (int32_t)recip[dy] : (dx << 16) / dy;
}

/* A convex quad, clockwise: from the top point the right side goes on
 * through the points and the left side goes back through them. A span is
 * the pixels from left up to, not including, right, kept to the columns
 * from lo up to hi. clip is known where this is expanded, so a quad that
 * is all inside pays for no clipping. */
static inline __attribute__((always_inline)) void fill_as(const point_t *p, unsigned colour, int clip, int lo, int hi,
                                                          const tex_t *tex) {
    (void)tex;
    int top = 0, y_end = p[0].y;
    for (int i = 1; i < 4; i++) {
        if (p[i].y < p[top].y) top = i;
        if (p[i].y > y_end) y_end = p[i].y;
    }
    int y = p[top].y;
    if (clip) {
        if (y >= FB_H || y_end <= 0) return;
        if (y_end > FB_H) y_end = FB_H;
    }

    int li = top, ri = top, lh = 0, rh = 0;
    int32_t xl = 0, xr = 0, dl = 0, dr = 0;
#if TEXTURED
    /* Held here: a byte written to the picture could be any of them. */
    const uint16_t *const texels = tex ? tex->words : 0;
    const uint32_t *const rows = persp_rows + 1;
    const uint32_t q = tex ? tex->q : 0;
    const int32_t cos = persp_job.c, sin = persp_job.s;
    const uint32_t far_u = 32 * (uint32_t)persp_job.focal, far_v = 4 * (uint32_t)persp_job.focal;
    const uint32_t eye_u = (uint32_t)persp_job.ex * 16, eye_v = (uint32_t)persp_job.ey * 2;
#endif
    while (y < y_end) {
        while (lh == 0) {
            int from = li;
            li = (li - 1) & 3;
            lh = p[li].y - p[from].y;
            xl = p[from].x << 16;
            if (lh > 0) dl = slope(p[li].x - p[from].x, lh);
            else if (lh < 0) return;
        }
        while (rh == 0) {
            int from = ri;
            ri = (ri + 1) & 3;
            rh = p[ri].y - p[from].y;
            xr = p[from].x << 16;
            if (rh > 0) dr = slope(p[ri].x - p[from].x, rh);
            else if (rh < 0) return;
        }
        int n = lh < rh ? lh : rh;
        lh -= n; rh -= n;
        if (clip) {
            if (y + n > y_end) n = y_end - y;
            if (y < 0) {
                int skip = -y < n ? -y : n;
                xl += dl * skip; xr += dr * skip;
                y += skip; n -= skip;
            }
        }
        uint8_t *line = (uint8_t *)persp_job.fb + (FIRST + y * PAGE) * 2;
#if TEXTURED
        int row = y;
#endif
        y += n;
        for (; n > 0; n--, line += PAGE * 2) {
            int left = xl >> 16, right = xr >> 16;
            xl += dl; xr += dr;
            if (clip) {
                if (left < lo) left = lo;
                if (right > hi) right = hi;
            }
#if TEXTURED
            int at_row = row++;
#endif
            if (left >= right) continue;
#if TEXTURED
            if (tex) {
                /* Forward is (c, s) and right is (-s, c). A pixel to the
                 * right is k of right, k being the depth over twice the
                 * focal length. So with f that step of forward, pixel p,
                 * whose middle is 319 - 2p half pixels left of the
                 * picture's, is at
                 *   u = 16 (eye_x + 2 focal fx) + 8 fy (319 - 2p)
                 *   v =  2 (eye_y + 2 focal fy) -   fx (319 - 2p)
                 * The depth is the middle of the row's. One place on the
                 * row is multiplied for, the even pixel at the span's right
                 * end, and the rest are steps from it: a pixel to the left
                 * is two more half pixels, so two of du and of dv. */
                uint32_t scratch;
                const uint32_t across = 0xF0;
                uint32_t k = mul31(rows[2 * at_row], q);
                int32_t fx = mul16((int32_t)k, cos), fy = mul16((int32_t)k, sin);
                uint32_t du = (uint32_t)fy * 8, dv = 0u - (uint32_t)fx;
                int even = right & ~1;
                uint32_t m = (uint32_t)(FB_W - 1 - 2 * even);
                uint32_t u = eye_u + (uint32_t)fx * far_u + du * m, v = eye_v + (uint32_t)fy * far_v + dv * m;
                if (right & 1) line[even] = (uint8_t)TEXEL(u, v);
                int words = (even - left) >> 1;
                du *= 4; dv *= 4;
                u += du; v += dv;
                if (words > 0) {
                    uint16_t *at = (uint16_t *)(void *)(line + even);
                    /* Two words a turn of the loop, entered halfway for an
                     * odd count: the count and the branch are a fifth of
                     * a word's work. */
                    int odd = words & 1;
                    words = (words + 1) >> 1;
                    __asm__ volatile(
                        "\ttst\t%[odd],%[odd]\n"
                        "\tbf\t2f\n"
                        "1:\tswap.w\t%[u],%[row]\n"
                        "\tand\t%[across],%[row]\n"
                        "\tswap.w\t%[v],r0\n"
                        "\tand\t#14,r0\n"
                        "\tor\t%[row],r0\n"
                        "\tmov.w\t@(r0,%[tex]),r0\n"
                        "\tadd\t%[du],%[u]\n"
                        "\tmov.w\tr0,@-%[at]\n"
                        "\tadd\t%[dv],%[v]\n"
                        "2:\tswap.w\t%[u],%[row]\n"
                        "\tand\t%[across],%[row]\n"
                        "\tswap.w\t%[v],r0\n"
                        "\tand\t#14,r0\n"
                        "\tor\t%[row],r0\n"
                        "\tmov.w\t@(r0,%[tex]),r0\n"
                        "\tadd\t%[du],%[u]\n"
                        "\tmov.w\tr0,@-%[at]\n"
                        "\tdt\t%[n]\n"
                        "\tbf.s\t1b\n"
                        "\tadd\t%[dv],%[v]"
                        : [at] "+r"(at), [n] "+r"(words), [u] "+r"(u), [v] "+r"(v), [row] "=&r"(scratch)
                        : [du] "r"(du), [dv] "r"(dv), [tex] "r"(texels), [across] "r"(across), [odd] "r"(odd)
                        : "r0", "t", "memory");
                }
                /* u and v are now a word left of the last word written:
                 * the odd pixel is half a word back to the right. */
                if (left & 1) line[left] = (uint8_t)TEXEL(u - (du >> 1), v - (dv >> 1));
                continue;
            }
#endif
            if (left & 1) line[left++] = (uint8_t)colour;
            if (right & 1) line[--right] = (uint8_t)colour;
            uint16_t *at = (uint16_t *)(void *)(line + left);
            for (int words = (right - left) >> 1; words > 0; words--) *at++ = (uint16_t)colour;
        }
    }
}

static void fill_clipped(const point_t *p, unsigned colour, int lo, int hi) { fill_as(p, colour, 1, lo, hi, 0); }
static void fill_inside(const point_t *p, unsigned colour) { fill_as(p, colour, 0, 0, 0, 0); }
#if TEXTURED
static void tex_clipped(const point_t *p, int lo, int hi, const tex_t *tex) { fill_as(p, 0, 1, lo, hi, tex); }
static void tex_inside(const point_t *p, const tex_t *tex) { fill_as(p, 0, 0, 0, 0, tex); }
#endif

/* A quad between four corners at four heights, named clockwise as the eye
 * sees it, in the columns from lo up to hi: in a colour, or level and in
 * the bitmap tex has ready. */
static void quad_as(const corner_t *a, int za, const corner_t *b, int zb,
                    const corner_t *c, int zc, const corner_t *d, int zd,
                    unsigned colour, int lo, int hi, const tex_t *tex) {
    if (!a->scale || !b->scale || !c->scale || !d->scale) return;
    int left = a->x, right = a->x;
    if (b->x < left) left = b->x; else if (b->x > right) right = b->x;
    if (c->x < left) left = c->x; else if (c->x > right) right = c->x;
    if (d->x < left) left = d->x; else if (d->x > right) right = d->x;
    if (right <= lo || left >= hi) return;

    point_t p[4];
    p[0].x = (int16_t)a->x; p[0].y = (int16_t)row_at(a, za);
    p[1].x = (int16_t)b->x; p[1].y = (int16_t)row_at(b, zb);
    p[2].x = (int16_t)c->x; p[2].y = (int16_t)row_at(c, zc);
    p[3].x = (int16_t)d->x; p[3].y = (int16_t)row_at(d, zd);
    int up = p[0].y, down = up;
    for (int k = 1; k < 4; k++) {
        if (p[k].y < up) up = p[k].y;
        if (p[k].y > down) down = p[k].y;
    }
#if TEXTURED
    if (tex) {
        if (left >= lo && right <= hi && up >= 0 && down <= FB_H) tex_inside(p, tex);
        else tex_clipped(p, lo, hi, tex);
        return;
    }
#else
    (void)tex;
#endif
    colour *= 0x0101u;
    if (left >= lo && right <= hi && up >= 0 && down <= FB_H) fill_inside(p, colour);
    else fill_clipped(p, colour, lo, hi);
}

static void quad(const corner_t *a, int za, const corner_t *b, int zb,
                 const corner_t *c, int zc, const corner_t *d, int zd,
                 unsigned colour, int lo, int hi) {
    quad_as(a, za, b, zb, c, zc, d, zd, colour, lo, hi, 0);
}

/* A cell's terrain: the height it has all over, or DETAIL. A creature
 * standing on it has not changed this. */
static inline unsigned ground_at(int cx, int cy) {
    return persp_cells[cy][cx].ground;
}

/* How high a quarter of a cell stands, by its place among all the
 * quarters of the map: what stands against a side of a box, for what of
 * the side it hides. A wall is on some quarters of its cell only. */
static unsigned quarter(int qx, int qy) {
    int cx = qx >> 1, cy = qy >> 1;
    if (cx < frame.x0 || cy < frame.y0 || cx >= frame.x0 + frame.w || cy >= frame.y0 + frame.h) return 0;
    unsigned g = ground_at(cx, cy);
    if (g != DETAIL) return g;
    unsigned tile = vox_tile[cy][cx], part = model_part[tile];
    if (part) return vox_lift[cy][cx] + ((part >> ((qy & 1) * 2 + (qx & 1))) & 1 ? part >> 4 : 0);
    return vox_lift[cy][cx] + model_base[tile];
}

/* The lowest of the quarters from one to another, which are in a line. */
static unsigned beside(int qx0, int qy0, int qx1, int qy1) {
    unsigned low = quarter(qx0, qy0);
    int dx = qx1 > qx0, dy = qy1 > qy0;
    while (low && (qx0 != qx1 || qy0 != qy1)) {
        qx0 += dx; qy0 += dy;
        unsigned other = quarter(qx0, qy0);
        if (other < low) low = other;
    }
    return low;
}

/* The top of a cell at one height. Floors are in two shades, cell and
 * cell about. */
static void top(int gx, int gy, unsigned height, unsigned colour, int lo, int hi) {
    const corner_t *at = &corners_row(gy)[gx], *under = at + frame.stride;
    quad(at, (int)height, at + 1, (int)height, under + 1, (int)height, under, (int)height, colour, lo, hi);
}

/* A side of a box from one corner to the next, clockwise as the eye sees
 * it, from h down to low. What of it is under lift is the plinth, in the
 * plinth's colour, as the rays have it. */
static void side(const corner_t *a, const corner_t *b, int h, int low, int lift,
                 unsigned colour, unsigned plinth, int lo, int hi) {
    if (lift > h) lift = h;
    if (lift <= low) {
        quad(a, h, b, h, b, low, a, low, colour, lo, hi);
        return;
    }
    if (lift < h) quad(a, h, b, h, b, lift, a, lift, colour, lo, hi);
    quad(a, lift, b, lift, b, low, a, low, plinth, lo, hi);
}

/* Of a box's sides that face the eye, what stands above the cell beside. */
static void sides(int gx, int gy, unsigned height, int lo, int hi) {
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned tile = vox_tile[cy][cx], light = vox_light[cy][cx];
    unsigned shade = vox_front_shade[light], dim = shade ? shade - 1 : 0;
    unsigned front = model_side[tile] + shade, flank = model_side[tile] + dim;
    unsigned plinth = vox_plinth[tile];
    const corner_t *at = &corners_row(gy)[gx], *under = at + frame.stride;
    int h = (int)height, lift = vox_lift[cy][cx] + model_foot[tile], low;

    int qx = 2 * cx, qy = 2 * cy;
    if (frame.eye_y > (cy + 1) * CELL && (low = (int)beside(qx, qy + 2, qx + 1, qy + 2)) < h)
        side(under, under + 1, h, low, lift, front, plinth + shade, lo, hi);
    if (frame.eye_y < cy * CELL && (low = (int)beside(qx, qy - 1, qx + 1, qy - 1)) < h)
        side(at + 1, at, h, low, lift, front, plinth + shade, lo, hi);
    if (frame.eye_x > (cx + 1) * CELL && (low = (int)beside(qx + 2, qy, qx + 2, qy + 1)) < h)
        side(under + 1, at + 1, h, low, lift, flank, plinth + dim, lo, hi);
    if (frame.eye_x < cx * CELL && (low = (int)beside(qx - 1, qy, qx - 1, qy + 1)) < h)
        side(at, under, h, low, lift, flank, plinth + dim, lo, hi);
}

#if TEXTURED
static void tex_for(tex_t *tex, const uint8_t *even, const uint8_t *odd, unsigned light, int z);
#endif

#define BETWEEN(to, a, b) do { (to).x = ((a).x + (b).x) >> 1; (to).scale = ((a).scale + (b).scale) >> 1; } while (0)

/* The long side of a row of cells, the south or the north: one piece for
 * each stretch of it that has the same height beside it. across is the
 * edge of the cells it is on, 0 their north, 1 their middle and 2 their
 * south, and qy the quarters that are beside it. */
static void long_side(int gx, int last, int gy, int across, int qy, int south, int h, int lift,
                      unsigned colour, unsigned plinth, int lo, int hi) {
    int from = gx, low = -1;
    for (int k = gx; k <= last + 1; k++) {
        int qx = 2 * (frame.x0 + k);
        int here = k <= last ? (int)beside(qx, qy, qx + 1, qy) : -2;
        if (here == low) continue;
        if (low >= 0 && low < h) {
            corner_t west, east;
            const corner_t *w = &corners_row(gy)[from], *e = &corners_row(gy)[k];
            if (across == 1) {
                BETWEEN(west, w[0], w[frame.stride]);
                BETWEEN(east, e[0], e[frame.stride]);
            } else {
                west = w[across ? frame.stride : 0];
                east = e[across ? frame.stride : 0];
            }
            if (south) side(&west, &east, h, low, lift, colour, plinth, lo, hi);
            else side(&east, &west, h, low, lift, colour, plinth, lo, hi);
        }
        from = k;
        low = here;
    }
}

/* A box on some quarters of a cell, or of a row of cells that are alike
 * from one to another: a wall, or the plinth it stands on. The cells of a
 * row are one box since a box costs by its sides and not its size.
 *
 * Its corners that are not a cell's are halfway between two that are on
 * the screen, which is not quite halfway over the ground, but a cell is
 * small beside its distance and the walls either side of an edge agree on
 * where its middle is. A box of more than one cell is as wide as they
 * are: only its depth is by halves.
 *
 * Its top is in a colour, or with tex in the first cell's bitmap. */
static void long_box(int gx, int last, int gy, unsigned on, int h, unsigned colour, tex_t *tex, int lo, int hi) {
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned tile = vox_tile[cy][cx], light = vox_light[cy][cx];
    const corner_t *at = &corners_row(gy)[gx], *end = &corners_row(gy)[last + 1];
    const corner_t *under = at + frame.stride, *under_end = end + frame.stride;
    if (!at->scale || !end->scale || !under->scale || !under_end->scale) return;

    corner_t n[3][3];
    n[0][0] = *at; n[0][2] = *end; n[2][0] = *under; n[2][2] = *under_end;
    BETWEEN(n[0][1], n[0][0], n[0][2]);
    BETWEEN(n[2][1], n[2][0], n[2][2]);
    BETWEEN(n[1][0], n[0][0], n[2][0]);
    BETWEEN(n[1][2], n[0][2], n[2][2]);
    BETWEEN(n[1][1], n[1][0], n[1][2]);
    int x0 = on & 5 ? 0 : 1, x1 = on & 10 ? 2 : 1, y0 = on & 3 ? 0 : 1, y1 = on & 12 ? 2 : 1;
    const corner_t *a = &n[y0][x0], *b = &n[y0][x1], *c = &n[y1][x1], *d = &n[y1][x0];
    int lift = vox_lift[cy][cx] + model_foot[tile], low;

#if TEXTURED
    if (tex) {
        /* The first cell's bitmap and the next's, by their columns. A
         * wall's is the one every wall has a part of, so that all the
         * walls of a room are one bitmap to hold. */
        const uint8_t *mine = vox_is_wall(tile) ? vox_wall_cap : persp_cells[cy][cx].top;
        const uint8_t *next = last > gx && !vox_is_wall(tile) ? persp_cells[cy][cx + 1].top : mine;
        tex_for(tex, cx & 1 ? next : mine, cx & 1 ? mine : next, light, h);
        quad_as(a, h, b, h, c, h, d, h, 0, lo, hi, tex);
    } else
#else
    (void)tex;
#endif
    quad(a, h, b, h, c, h, d, h, colour, lo, hi);

    unsigned shade = vox_front_shade[light], dim = shade ? shade - 1 : 0;
    unsigned front = model_side[tile] + shade, flank = model_side[tile] + dim;
    unsigned plinth = vox_plinth[tile];
    /* In quarters of a cell, from the first the box is on to the last. */
    int qx0 = 2 * cx + x0, qx1 = 2 * (frame.x0 + last) + x1 - 1, qy0 = 2 * cy + y0, qy1 = 2 * cy + y1 - 1;
    if (last > gx && frame.pits) {
        /* What stands beside a row of cells is not one height all along
         * it where there is a pit for the stairs: there the side goes
         * farther down, and only there. */
        if (frame.eye_y > (qy1 + 1) * (CELL / 2))
            long_side(gx, last, gy, y1, qy1 + 1, 1, h, lift, front, plinth + shade, lo, hi);
        if (frame.eye_y < qy0 * (CELL / 2))
            long_side(gx, last, gy, y0, qy0 - 1, 0, h, lift, front, plinth + shade, lo, hi);
    } else {
        if (frame.eye_y > (qy1 + 1) * (CELL / 2) && (low = (int)beside(qx0, qy1 + 1, qx1, qy1 + 1)) < h)
            side(d, c, h, low, lift, front, plinth + shade, lo, hi);
        if (frame.eye_y < qy0 * (CELL / 2) && (low = (int)beside(qx0, qy0 - 1, qx1, qy0 - 1)) < h)
            side(b, a, h, low, lift, front, plinth + shade, lo, hi);
    }
    if (frame.eye_x > (qx1 + 1) * (CELL / 2) && (low = (int)beside(qx1 + 1, qy0, qx1 + 1, qy1)) < h)
        side(c, b, h, low, lift, flank, plinth + dim, lo, hi);
    if (frame.eye_x < qx0 * (CELL / 2) && (low = (int)beside(qx0 - 1, qy0, qx0 - 1, qy1)) < h)
        side(a, d, h, low, lift, flank, plinth + dim, lo, hi);
}

/* A box: its top and its sides. */
static void box(int gx, int gy, unsigned height, unsigned lit, int lo, int hi) {
    top(gx, gy, height, model_top[vox_tile[frame.y0 + gy][frame.x0 + gx]] + lit, lo, hi);
    sides(gx, gy, height, lo, hi);
}

/* The walls from one cell to another, and the plinth under them. */
static void wall_run(int gx, int last, int gy, tex_t *tex, int lo, int hi) {
    if (OFF(2)) return;
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned part = model_part[vox_tile[cy][cx]];
    long_box(gx, last, gy, part & 15, vox_lift[cy][cx] + (int)(part >> 4),
             model_top[vox_tile[cy][cx]] + vox_light[cy][cx], WALL_TOPS ? tex : 0, lo, hi);
}

/* The plinth shows on the half of the cell the wall is not on, and is
 * only drawn there: under a wall it is paint the wall goes over. Round
 * the wall of a corner it is three quarters, which is no box, so there
 * it is the whole cell. */
/* Ground from one cell to another, all of a height and in one bitmap. */
static void ground_run(int gx, int last, int gy, tex_t *tex, int lo, int hi) {
    if (OFF(1)) return;
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    long_box(gx, last, gy, 15, (int)ground_at(cx, cy), 0, tex, lo, hi);
}

static void ledge_run(int gx, int last, int gy, int lo, int hi) {
    if (OFF(16)) return;
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned tile = vox_tile[cy][cx], off = 15 ^ (model_part[tile] & 15);
    if (off != 3 && off != 12 && off != 5 && off != 10) off = 15;
    long_box(gx, last, gy, off, vox_lift[cy][cx], vox_plinth[tile] + vox_light[cy][cx], 0, lo, hi);
}

#if TEXTURED
/* The top of a cell at one height, in its bitmap. */
static void top_bitmap(int gx, int gy, unsigned height, tex_t *tex, int lo, int hi) {
    int cx = frame.x0 + gx, cy = frame.y0 + gy, h = (int)height;
    const corner_t *at = &corners_row(gy)[gx], *under = at + frame.stride;
    tex_for(tex, persp_cells[cy][cx].top, persp_cells[cy][cx].top, vox_light[cy][cx], h);
    quad_as(at, h, at + 1, h, under + 1, h, under, h, 0, lo, hi, tex);
}
#endif

/* Eight by eight voxel columns, a column at a time from the far corner to
 * the near one. They are small beside their distance, so one point is
 * projected and the columns are placed by steps from it. A column is the
 * box round where its footprint falls, as wide as whole words: its top in
 * the colour of its highest voxel, and under that its side, a row at a
 * time in the colour of the voxel at that height, as far down as the
 * columns in front leave it showing.
 *
 * (x, y) is the voxel of the model's corner and feet the height it stands
 * at. Columns no higher than base are left out, and hide what is under
 * them of the others' sides: base is the ground already painted. */
static void columns(const vcol_t model[CELL][CELL], int face, int x, int y, int feet, int base,
                    unsigned lit, unsigned flash, int lo, int hi) {
    if (OFF(4)) return;
    const sh2z_fx_t s = persp_job.s, c = persp_job.c;
    int32_t rx = ((x + CELL / 2) << 16) - persp_job.ex, ry = ((y + CELL / 2) << 16) - persp_job.ey;
    int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
    int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
    if (t < frame.near) return;
    int32_t scale = (int32_t)scale_at(t);
    /* No such thing is 48 pixels from its middle to its edge. */
    int middle = FB_W / 2 + (int)(((int64_t)u * scale) >> 32);
    if (middle + 48 < lo || middle - 48 >= hi) return;

    /* All Q16, pixels across and rows down. A voxel nearer moves a point
     * out from the middle column by across * u / t and down by
     * eye * focal / t / t. */
    int32_t across = scale;
    int32_t aside = -sh2z_mul(across, divide(u, t >> 16));
    int32_t down = -divide((int32_t)(((uint32_t)frame.eye_h * (uint32_t)scale) >> 1), t >> 16);
    int32_t xx = sh2z_mul(aside, c) - sh2z_mul(across, s), xy = sh2z_mul(aside, s) + sh2z_mul(across, c);
    int32_t yx = sh2z_mul(down, c), yy = sh2z_mul(down, s);
    int32_t rise = scale >> 1;                         /* rows a voxel is tall */
    int32_t voxels = divide(0x7FFFFFFF, scale >> 2);   /* voxels a row is tall */
    int32_t x0 = (middle << 16) - (CELL / 2) * (xx + xy);
    int32_t y0 = frame.horizon * 65536 + (int32_t)(((uint32_t)(frame.eye_h - feet) * (uint32_t)scale) >> 1)
               - (CELL / 2) * (yx + yy);
    int32_t left_by = (xx < 0 ? xx : 0) + (xy < 0 ? xy : 0), right_by = (xx > 0 ? xx : 0) + (xy > 0 ? xy : 0);
    int32_t far_by = (yx < 0 ? yx : 0) + (yy < 0 ? yy : 0), near_by = (yx > 0 ? yx : 0) + (yy > 0 ? yy : 0);
    int first_x = c > 0 ? CELL - 1 : 0, step_x = c > 0 ? -1 : 1;
    int first_y = s > 0 ? CELL - 1 : 0, step_y = s > 0 ? -1 : 1;
    unsigned side = vox_front_shade[lit];
    lo >>= 1; hi >>= 1;

    for (int i = 0, mx = first_x; i < CELL; i++, mx += step_x) {
        for (int j = 0, my = first_y; j < CELL; j++, my += step_y) {
            const vcol_t *col = vox_model_column(model, face, mx, my);
            if (col->h <= base) continue;
            int32_t px = x0 + mx * xx + my * xy, py = y0 + mx * yx + my * yy;
            int left = (px + left_by) >> 17, right = (px + right_by + 0x1FFFF) >> 17;
            int foot = (py + near_by) >> 16, back = (py + far_by) >> 16;
            int tall = (col->h * rise) >> 16;
            int upper = back - tall, lip = foot - tall;
            if (left < lo) left = lo;
            if (right > hi) right = hi;
            if (left >= right || upper >= FB_H || foot <= 0) continue;
            if (lip <= upper) lip = upper + 1;
            int width = right - left;
            uint16_t *at = (uint16_t *)(void *)((uint8_t *)persp_job.fb + (FIRST + upper * PAGE) * 2) + left;
            unsigned colour = ((flash ? flash & ~7u : col->c[col->h - 1]) + lit) * 0x0101u;
            int row = upper;
            for (; row < lip; row++, at += PAGE) {
                if ((unsigned)row >= FB_H) continue;
                for (int w = 0; w < width; w++) at[w] = (uint16_t)colour;
            }
            /* The side shows down to the lower of the two columns in
             * front of this one; they hide the rest of it. */
            int nx = mx + step_x, ny = my + step_y;
            int front_x = (unsigned)nx < CELL ? vox_model_column(model, face, nx, my)->h : 0;
            int front_y = (unsigned)ny < CELL ? vox_model_column(model, face, mx, ny)->h : 0;
            int hidden = front_x < front_y ? front_x : front_y;
            if (hidden < base) hidden = base;
            if (hidden >= col->h) continue;
            foot -= (hidden * rise) >> 16;
            int32_t z = (int32_t)col->h << 16;
            for (; row < foot && row < FB_H; row++, at += PAGE) {
                z -= voxels;
                if (row < 0) continue;
                colour = ((flash ? flash & ~7u : col->c[z > 0 ? z >> 16 : 0]) + side) * 0x0101u;
                for (int w = 0; w < width; w++) at[w] = (uint16_t)colour;
            }
        }
    }
}

/* Floor is ground however far its plinth has lifted it: what makes it
 * ground is that it is a low slab, on which things stand. */
static inline int is_ground(unsigned tile) {
    return model_flat[tile] != DETAIL && model_flat[tile] <= FLOOR_MAX;
}

/* A cell's ground: a floor, in two shades cell and cell about, or the
 * slab that a thing which is not one height stands on. */
static void ground(int gx, int gy, tex_t *tex, int lo, int hi) {
    (void)tex;
    if (OFF(1)) return;
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned g = ground_at(cx, cy), tile = vox_tile[cy][cx], light = vox_light[cy][cx];
    if (g == DETAIL) {
        /* With its sides: a door in an outer wall stands over nothing. */
        if (model_base[tile]) box(gx, gy, vox_lift[cy][cx] + model_base[tile], light, lo, hi);
        /* Or on its plinth alone, whose top shows where the thing leaves
         * the cell empty: the ledge outside a wall. */
        else if (vox_lift[cy][cx]) ledge_run(gx, gx, gy, lo, hi);
    } else if (g && is_ground(tile)) {
#if TEXTURED
        top_bitmap(gx, gy, g, tex, lo, hi);
        sides(gx, gy, g, lo, hi);
#else
        if ((cx + cy) & 1) light = light > 2 ? light - 2 : 0;
        box(gx, gy, g, light, lo, hi);
#endif
    }
}

/* What stands in a cell: a box, or the columns of a thing that is not one
 * height, and then whoever is on it. */
/* A cell sunk under those round it, with a thing in it: the stairs. */
static int is_pit(int cx, int cy, unsigned lift) {
    if (cx < frame.x0 || cy < frame.y0 || cx >= frame.x0 + frame.w || cy >= frame.y0 + frame.h) return 0;
    return vox_lift[cy][cx] < lift && ground_at(cx, cy) == DETAIL && !model_part[vox_tile[cy][cx]];
}

static void ground(int gx, int gy, tex_t *tex, int lo, int hi);

static void standing_in(int gx, int gy, tex_t *tex, int lo, int hi) {
    (void)tex;
    int cx = frame.x0 + gx, cy = frame.y0 + gy;
    unsigned g = ground_at(cx, cy), tile = vox_tile[cy][cx];
    /* What is in a pit is drawn down to the pit's bottom, and what of
     * that the rim nearest the eye should hide goes over the floor this
     * side of it. So floor with a pit behind it is painted again: it is
     * come to after the pit, and before anything stands on it. */
    if (frame.pits && g && g != DETAIL && is_ground(tile)) {
        unsigned lift = vox_lift[cy][cx];
        int bx = frame.eye_x >= (cx + 1) * CELL ? -1 : frame.eye_x < cx * CELL ? 1 : 0;
        int by = frame.eye_y >= (cy + 1) * CELL ? -1 : frame.eye_y < cy * CELL ? 1 : 0;
        if ((bx && is_pit(cx + bx, cy, lift)) || (by && is_pit(cx, cy + by, lift)) ||
            (bx && by && is_pit(cx + bx, cy + by, lift)))
            PROF(PROF_GROUND, ground(gx, gy, tex, lo, hi));
    }
    if (g == DETAIL) {
        const vmodel_t *partial = vox_assembly_model(cx, cy);
        const vmodel_t *model = partial ? partial : &vox_models[tile];
        if (!partial && model_part[tile]) PROF(PROF_WALL, wall_run(gx, gx, gy, tex, lo, hi));
        else PROF(PROF_TERRAIN, columns(model->col, 0, cx * CELL, cy * CELL, vox_lift[cy][cx],
                                        partial ? 0 : model_base[tile], vox_light[cy][cx], 0, lo, hi));
    } else if (g && !is_ground(tile)) {
#if WALL_TOPS
        PROF(PROF_WALL, top_bitmap(gx, gy, g, tex, lo, hi); sides(gx, gy, g, lo, hi));
#else
        box(gx, gy, g, vox_light[cy][cx], lo, hi);
#endif
    }
    for (int n = standing[gy * frame.w + gx]; n; n = seen[n - 1].next) {
        const vox_creature_t *c = &seen[n - 1].c;
        int x = c->x + CELL / 2, y = c->y + CELL / 2;
        int feet = g == DETAIL ? vox_lift[cy][cx] + vox_models[tile].col[x & 7][y & 7].h : (int)g;
        PROF(PROF_CREATURE, columns(seen[n - 1].model, c->face, c->x, c->y, feet + c->z, 0, vox_light[cy][cx], c->flash, lo, hi));
    }
}

/* One CPU's half of the picture: the ground, then what stands on it.
 * Along each axis the cells on the far side of the eye's own come first,
 * working toward it, and then those on the near side, working back to
 * it. */
static void draw(int lo, int hi) {
    tex_t *tex = 0;
    if (OFF(8)) return;
#if TEXTURED
    tex = &tex_of[lo != 0];
    tex->z = -1;
#endif
    int eye_gx = (frame.eye_x >> 3) - frame.x0, eye_gy = (frame.eye_y >> 3) - frame.y0;
    if (frame.eye_x < 0) eye_gx = -1;
    if (frame.eye_y < 0) eye_gy = -1;
    for (int pass = 0; pass < 2; pass++) {
        for (int side_y = 0; side_y < 2; side_y++) {
            int gy = side_y ? frame.h - 1 : 0, end_y = side_y ? (eye_gy > 0 ? eye_gy : 0) - 1 : (eye_gy < frame.h ? eye_gy : frame.h);
            for (; side_y ? gy > end_y : gy < end_y; gy += side_y ? -1 : 1) {
                for (int side_x = 0; side_x < 2; side_x++) {
                    int gx = side_x ? frame.w - 1 : 0, end_x = side_x ? (eye_gx > 0 ? eye_gx : 0) - 1 : (eye_gx < frame.w ? eye_gx : frame.w);
                    /* Walls side by side along the row that are alike are
                     * one box, and so are the plinths under them: first
                     * is where such a run began, or -1. Nothing stands on
                     * a wall, and a row is all as far from the eye this
                     * way, so the order is kept. */
                    int first = -1, before = 0;
                    unsigned run_tile = 0, run_light = 0, run_lift = 0, run_ground = 0;
                    const uint8_t *run_top[2] = { 0, 0 };
#define END_RUN() do { if (first >= 0) { \
                        int from = first < before ? first : before, to = first < before ? before : first; \
                        if (pass) PROF(PROF_WALL, wall_run(from, to, gy, tex, lo, hi)); \
                        else if (!run_ground) PROF(PROF_LEDGE, ledge_run(from, to, gy, lo, hi)); \
                        else if (from == to) PROF(PROF_FLOOR, ground(from, gy, tex, lo, hi)); \
                        else PROF(PROF_FLOOR, ground_run(from, to, gy, tex, lo, hi)); \
                        first = -1; } } while (0)
                    for (; side_x ? gx > end_x : gx < end_x; gx += side_x ? -1 : 1) {
                        int cx = frame.x0 + gx, cy = frame.y0 + gy;
                        unsigned tile = vox_tile[cy][cx], part = model_part[tile] & 15;
                        /* Most of what is round a passage is nothing. */
                        if (!tile && !(pass && standing[gy * frame.w + gx])) { END_RUN(); continue; }
                        const corner_t *at = &corners_row(gy)[gx], *under = at + frame.stride;
                        unsigned g = ground_at(cx, cy);
                        /* Ground side by side is one box too, if it is
                         * all one tile or two turn and turn about, as a
                         * room's floor is: what is over the even columns
                         * of the map is alike, and what is over the odd. */
                        int joins = part ? g == DETAIL && vox_lift[cy][cx] &&
                                           (!pass || ((part == 3 || part == 12) && !vox_assembly_model(cx, cy)))
                                         : TEXTURED && !pass && g && g != DETAIL && is_ground(tile);
                        if (joins && at[0].scale && at[1].scale && under[0].scale && under[1].scale) {
                            unsigned light = vox_light[cy][cx], lift = vox_lift[cy][cx];
                            const uint8_t *cell_top = persp_cells[cy][cx].top;
                            if (first >= 0 && (light != run_light || lift != run_lift || (!part) != run_ground ||
                                               (part ? tile != run_tile
                                                     : run_top[cx & 1] && run_top[cx & 1] != cell_top))) END_RUN();
                            if (first < 0) {
                                first = gx; run_tile = tile; run_light = light; run_lift = lift; run_ground = !part;
                                run_top[0] = run_top[1] = 0;
                            }
                            run_top[cx & 1] = cell_top;
                            before = gx;
                            continue;
                        }
                        END_RUN();
                        if (pass) standing_in(gx, gy, tex, lo, hi);
                        else PROF(PROF_GROUND, ground(gx, gy, tex, lo, hi));
                    }
                    END_RUN();
                }
            }
        }
    }
}

void poly_secondary(void) {
    for (int row = 0; row < FB_H; row++) {
        FILL_WAIT();
        MARS_VDP_FILLEN = FB_W / 2 - 1;
        MARS_VDP_FILADR = (uint16_t)(FIRST + row * PAGE);
        MARS_VDP_FILDAT = BLACK;
    }
    FILL_WAIT();
    SHARED(cleared) = 1;
    /* The primary's work is read only once it is whole: this CPU's cache
     * was emptied before the frame and has taken none of it in. */
    while (!SHARED(listed));
    /* This CPU's timer is its own, and nothing else sets it to count as
     * the primary's does. */
    SH2_FRT_TCR = 0x02;
    uint16_t began = ticks();
    draw(frame.split, FB_W);
    SHARED(secondary_ticks) = (uint16_t)(ticks() - began);
}

/* Loose voxels go over the finished picture, each projected by itself.
 * One behind a wall shows through it for the moment it lasts. */
static void pieces(void) {
    const sh2z_fx_t s = persp_job.s, c = persp_job.c;
    int px, py, pz, piece;
    unsigned colour;
    for (int i = 0; (piece = vox_piece(i, &px, &py, &pz, &colour)); i++) {
        int32_t rx = (px << 12) - persp_job.ex, ry = (py << 12) - persp_job.ey;
        int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
        int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
        if (t < frame.near) continue;
        uint32_t scale = scale_at(t);
        int word = (FB_W / 2 + (int)(((int64_t)u * (int32_t)scale) >> 32)) >> 1;
        int row = (int)frame.horizon + (int)(((uint32_t)(frame.eye_h * 16 - pz) * scale) >> 21);
        if ((unsigned)word >= FB_W / 2 - 1 || (unsigned)row >= FB_H - 1) continue;
        uint16_t *at = (uint16_t *)(void *)((uint8_t *)persp_job.fb + (FIRST + row * PAGE) * 2) + word;
        if (piece == 2) {
            /* Larger pieces keep a lit top and a darker side as they fall. */
            unsigned material = colour & ~7u;
            at[0] = (uint16_t)((material + 7) * 0x0101u);
            at[1] = (uint16_t)((material + 6) * 0x0101u);
            at[PAGE] = (uint16_t)((material + 6) * 0x0101u);
            at[PAGE + 1] = (uint16_t)((material + 4) * 0x0101u);
        } else {
            at[0] = (uint16_t)(colour * 0x0101u);
        }
    }
}

int poly_frame(void) {
#ifdef POLY_PROF
    uint16_t frame_began = ticks();
#endif
    frame.x0 = persp_job.view_left;
    frame.y0 = persp_job.view_top;
    frame.w = persp_job.view_right - persp_job.view_left;
    frame.h = persp_job.view_bottom - persp_job.view_top;
    frame.stride = frame.w + 1;
    if (frame.w < 1 || frame.h < 1 ||
        (frame.w + 1) * (frame.h + 1) > (VIEW_H_MAX + 1) * (VIEW_W_MAX + 1) ||
        frame.w * frame.h > VIEW_H_MAX * VIEW_W_MAX) return 0;
    frame.eye_x = persp_job.ex >> 16;
    frame.eye_y = persp_job.ey >> 16;
    frame.eye_h = persp_job.eye_h;
    frame.horizon = persp_job.horizon;
    frame.split = split;
    /* The divider takes a signed dividend: the focal length goes as far
     * up as leaves it one, and the depth comes down to match. */
    frame.up = 23;
    while (persp_job.focal >> (31 - frame.up)) frame.up--;
    frame.down = 33 - frame.up;
    frame.near = persp_job.t_near - (persp_job.t_near >> 2);

    /* Every row is shown on two scanlines. The line table is in the
     * framebuffer, so it is written before the secondary starts to clear:
     * a write during a fill is lost. */
    uint16_t *lines = persp_job.fb;
    for (int k = 0; k < 2 * FB_H; k++) lines[k] = (uint16_t)(FIRST + (k / 2) * PAGE);

    SHARED(cleared) = 0;
    SHARED(listed) = 0;
    MARS_SYS_COMM6 = POLY_GO;
    project();
    list_creatures();
    SHARED(listed) = 1;
    while (!SHARED(cleared));
    uint16_t began = ticks();
    draw(0, frame.split);
    int mine = (uint16_t)(ticks() - began);
    while (MARS_SYS_COMM6 == POLY_GO);
#ifdef POLY_PROF
    poly_prof[PROF_DRAW] += (uint32_t)mine;
    poly_prof[PROF_OTHER_CPU] += SHARED(secondary_ticks);
    poly_prof[PROF_FRAME] += (uint16_t)(ticks() - frame_began);
#endif
    /* Were the work spread evenly over the columns, this would even it
     * out in one go; it is not, so go half way and no more than a step. */
    int its = SHARED(secondary_ticks);
    int move = (its - mine) * (FB_W / 4) / (its + mine + 1);
    if (move > SPLIT_STEP) move = SPLIT_STEP;
    if (move < -SPLIT_STEP) move = -SPLIT_STEP;
    move += split;
    if (move < SPLIT_MIN) move = SPLIT_MIN;
    if (move > SPLIT_MAX) move = SPLIT_MAX;
    split = (int16_t)(move & ~1);
    if (!persp_actors_hidden()) pieces();
    return 1;
}
#else
typedef int poly_unused;
#endif
