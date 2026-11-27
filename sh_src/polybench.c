/* make POLYBENCH=1: can flat polygons draw a room faster than the rays do?
 *
 * One lit room of Rogue's largest size as flat-coloured quads, one to a
 * floor cell and up to three to a wall cell, seen through the game's own
 * camera at 320 by 112 and turned through a full circle. Quads are sorted
 * far to near by buckets and every span longer than two words is painted
 * by the 32X's fill hardware, so what the SH-2 pays for is a polygon's
 * height and not its area. No textures. Creatures are not polygons: each
 * is its voxel columns, far to near, as small boxes (ACTORS=n, 5 unless
 * given, 0 for none).
 *
 * The primary SH-2 projects and sorts; the secondary clears, scan-converts
 * and fills (POLYBENCH=1), or the primary does everything (POLYBENCH=2),
 * or each draws its side of the screen (POLYBENCH=3, see SHARE).
 * Times are FRT ticks, 6,000 to two vblanks, in polybench_result. */
#ifdef POLYBENCH
#include "mars.h"
#include "text.h"
#include "voxel.h"
#include "polybench.h"
#include <stddef.h>
void *malloc(size_t n);            /* port/libc.c */
#include "sh2zam/sh2zam.h"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "baked3d.h"

/* The room, in cells, walls included. */
#define ROOM_W    22
#define ROOM_H    9
#define WALL_Z    CELL                     /* a wall is a cell high */
#define GRID_W    (ROOM_W + 1)
#define GRID_H    (ROOM_H + 1)
#define N_GRID    (GRID_W * GRID_H)
#define MAX_QUADS (ROOM_W * ROOM_H + 4 * (2 * ROOM_W + 2 * ROOM_H - 4))

/* The game's near lens: persp.c's FOCAL_NEAR and TARGET_ROW. */
#define FOCAL     128
#define HORIZON   (VIEW_H / 2 + 4 - FOCAL)
#define EYE       POLYBENCH_EYE
#define FB_W      320
#define FB_H      VIEW_H
/* Nothing nearer than this can show: level ground reaches the bottom row
 * at eye * focal / (rows - horizon), which is farther. */
#define T_NEAR    (48 << 6)

/* A row is one 256-word page, so that no fill crosses a page. */
#define PAGE      256
#define FIRST     0x100                    /* words: after the line table */

#define DIVU_DVSR  (*(volatile uint32_t *)0xFFFFFF00)
#define DIVU_DVDNT (*(volatile uint32_t *)0xFFFFFF04)

enum { BACK = 1, FLOOR_A, FLOOR_B, WALL_TOP, WALL_SIDE_A, WALL_SIDE_B, FLOOR_GROUT, FLOOR_SPECK, N_COLOURS };

typedef struct { int16_t x, y; } point_t;
/* A quad is a cell's corner and which of the cell's faces it is; the four
 * points follow from those, which keeps the list small. */
enum { FACE_FLOOR, FACE_TOP, FACE_SOUTH, FACE_NORTH, FACE_EAST, FACE_WEST, N_FACES, FACE_ACTOR = N_FACES };
typedef struct { uint16_t base; int16_t next; uint8_t face, colour; } quad_t;

/* Projected grid points: level 0 is the floor, level 1 the wall tops. */
static point_t shown[2][N_GRID] __attribute__((aligned(4)));
static int32_t depth[N_GRID];              /* Q6 voxels; zero is behind */
static int any_near;                       /* some point is */
/* Creatures standing in the room. Each is one more entry after the quads,
 * sorted with them, whose base is its number here. */
#ifndef POLYBENCH_ACTORS
#define POLYBENCH_ACTORS 5
#endif
#define MAX_ACTORS 8
static const struct { uint8_t x, y, model; } actors[MAX_ACTORS] = {
    { 11, 4, 0 },                               /* the hero, where the camera looks */
    { 14, 3, 4 }, { 7, 5, 20 }, { 16, 6, 3 }, { 4, 2, 1 },
    { 18, 2, 8 }, { 9, 2, 15 }, { 12, 6, 26 },
};
/* An actor is small beside its distance, so its columns are placed by
 * steps from its near corner and not projected one by one. All Q16:
 * pixels for x, rows for y. */
typedef struct {
    int32_t x, y;                  /* the model's corner at column 0, 0 */
    int32_t xx, xy, yx, yy;        /* the step to the next column across and back */
    int32_t rise;                  /* rows a voxel is tall */
    int32_t voxels;                /* voxels a row is tall */
    int8_t first_x, step_x, first_y, step_y;   /* far to near */
} placed_t;
static placed_t placed[MAX_ACTORS];

/* A creature that has been struck down goes to pieces, as in the game:
 * some forty of its voxels fly up and out, fall, lie a moment and go. They
 * are in sixteenths of a voxel and move once a frame. SHATTER=0 leaves
 * this out. */
#ifndef POLYBENCH_SHATTER
#define POLYBENCH_SHATTER 1
#endif
#define MAX_PIECES   40
#define PIECE_LIFE   38
#define SHATTER_EVERY 48                   /* frames from one to the next */
typedef struct { int16_t x, y, z; int8_t vx, vy, vz; uint8_t life, colour; } piece_t;
static piece_t pieces[MAX_PIECES];
static int gone = -1;                      /* the actor that is in pieces */
static quad_t quads[MAX_QUADS + MAX_ACTORS];
static int n_quads;

#define N_BUCKETS 256
static int16_t bucket[N_BUCKETS];

/* The draw list the secondary reads: quads far to near, each as its
 * number and one. Zero ends it. No more than a floor's and three faces of
 * each wall cell can face the camera. */
#define MAX_DRAWS (ROOM_W * ROOM_H + 3 * (2 * ROOM_W + 2 * ROOM_H))
static uint16_t draws[MAX_DRAWS + MAX_ACTORS + 1];
#define SHARED(x) (*(volatile __typeof__(x) *)((uintptr_t)&(x) | 0x20000000u))
static volatile int16_t draws_ready;       /* how many the primary has finished */

static uint16_t recip[256];                /* 65536 / n, from 2 */

polybench_result_t polybench_result;
static uint16_t spans, drawn;

/* TEX=1: the floor takes a bitmap, eight by eight to a cell as the rays
 * have it, and not a colour. The camera does not roll, so the floor is one
 * depth along a row of the screen and a step across the row is one step
 * over the floor: a span is two adds and a look a sample. TEX=1 samples
 * once a word, which is the two pixels of a column; TEX=2 once a pixel.
 *
 * Where the floor is at a pixel is in voxels, Q16, times sixteen one way
 * and twice the other, so that the high word of each holds its three bits
 * of the bitmap where the two can be put together as they are. */
#ifdef POLYBENCH_TEX
#if POLYBENCH != 3
#error TEX=1 wants POLYBENCH=3: the fill hardware paints one colour
#endif
#define TEX POLYBENCH_TEX
/* The game's statics leave this ROM no room for tables: these are taken
 * from Rogue's heap, which the bench has no other use for. */
typedef struct {
    int32_t u, v;                  /* the eye, scaled as above */
    sh2z_fx_t s, c;
    int32_t step[FB_H];            /* voxels a pixel on each row, Q16 */
    uint16_t texels[CELL * CELL];
} floor_t;
static floor_t *the_floor;
#define TEXEL(u, v) (*(const uint16_t *)(const void *)((const uint8_t *)texels + \
                     ((((uint32_t)(u) >> 16) & 0x70) | (((uint32_t)(v) >> 16) & 0x0E))))

/* a * b in Q16: the middle of the 64-bit product, which the compiler
 * shifts out with a library call. */
static inline int32_t mul16(int32_t a, int32_t b) {
    int32_t hi, lo;
    __asm__("dmuls.l %2,%3\n\tsts mach,%0\n\tsts macl,%1\n\txtrct %0,%1"
            : "=&r"(hi), "=&r"(lo) : "r"(a), "r"(b) : "mach", "macl");
    return lo;
}

/* Words of floor from the right end of a span leftward: at is just past
 * the last, u and v are where that one is, and du and dv the step back to
 * the word before. Eleven instructions a word; the compiler's loop is
 * seventeen. */
static inline void floor_words(uint16_t *at, int words, int32_t u, int32_t v,
                               int32_t du, int32_t dv, const uint16_t *texels) {
    int32_t row, across = 0x70;
    __asm__ volatile(
        "1:\tswap.w\t%[u],%[row]\n"
        "\tand\t%[across],%[row]\n"
        "\tswap.w\t%[v],r0\n"
        "\tand\t#14,r0\n"
        "\tor\t%[row],r0\n"
        "\tmov.w\t@(r0,%[tex]),r0\n"
        "\tsub\t%[du],%[u]\n"
        "\tmov.w\tr0,@-%[at]\n"
        "\tdt\t%[n]\n"
        "\tbf.s\t1b\n"
        "\tsub\t%[dv],%[v]"
        : [at] "+r"(at), [n] "+r"(words), [u] "+r"(u), [v] "+r"(v), [row] "=&r"(row)
        : [du] "r"(du), [dv] "r"(dv), [tex] "r"(texels), [across] "r"(across)
        : "r0", "t", "memory");
}

/* A flagstone: mortar along two edges and a few chips. */
static void build_floor(void) {
    static const uint8_t chips[] = { 2 * 8 + 5, 3 * 8 + 5, 3 * 8 + 6, 5 * 8 + 2, 6 * 8 + 2, 6 * 8 + 3, 4 * 8 + 4 };
    the_floor = malloc(sizeof *the_floor);
    uint16_t *floor_tex = the_floor->texels;
    int32_t *row_step = the_floor->step;
    for (int i = 0; i < CELL * CELL; i++)
        floor_tex[i] = (uint16_t)((i >> 3 == 0 || (i & 7) == 0 ? FLOOR_GROUT : FLOOR_B) * 0x0101);
    for (unsigned i = 0; i < sizeof chips; i++) floor_tex[chips[i]] = FLOOR_SPECK * 0x0101;
    for (int y = 0; y < FB_H; y++) {
        /* The middle of the row: eye * focal / (y + 1/2 - horizon). */
        row_step[y] = (int32_t)((((uint64_t)(2 * EYE * FOCAL)) << 16) / (uint32_t)(2 * (y - HORIZON) + 1)) / (2 * FOCAL);
    }
}

/* Where the floor is on a row. Forward is (c, s) and right is (-s, c): a
 * pixel to the right is the row's step of right, and the depth is
 * 2 * FOCAL pixels' worth of forward. So with f the step forward, pixel p,
 * whose middle is 319 - 2p half pixels left of the picture's, is at
 *   u = 16 (eye_x + 256 fx) + 8 fy (319 - 2p)
 *   v =  2 (eye_y + 256 fy) -   fx (319 - 2p)
 * which is two multiplies a row and two more a place on it. No table of
 * rows is kept: a row of the screen is crossed by few spans now that a
 * span is a row of cells long. */
typedef struct { int32_t u, v, du, dv; } floor_row_t;   /* u and v where 319 - 2p is 0 */
static inline __attribute__((always_inline)) void floor_row(floor_row_t *row, const floor_t *f, int y) {
    int32_t k = f->step[y];
    /* k is small and s and c are no more than one: nothing to clamp. */
    int32_t fx = mul16(k, f->c), fy = mul16(k, f->s);
    row->u = f->u + fx * (16 * 2 * FOCAL); row->du = fy * 8;
    row->v = f->v + fy * (2 * 2 * FOCAL); row->dv = -fx;
}
#define FLOOR_U(row, p) ((row)->u + (row)->du * (FB_W - 1 - 2 * (p)))
#define FLOOR_V(row, p) ((row)->v + (row)->dv * (FB_W - 1 - 2 * (p)))
#else
#define TEX 0
#endif

static inline uint16_t clock(void) {
    uint8_t hi = SH2_FRT_FRCH, lo = SH2_FRT_FRCL;
    return (uint16_t)(hi << 8 | lo);
}

/* Points are named clockwise as the camera sees a face it is in front of. */
#define G(x, y)   ((y) * GRID_W + (x))
#define TOP(x, y) (N_GRID + G(x, y))
static const uint16_t corner[N_FACES][4] = {
    { G(0, 0), G(1, 0), G(1, 1), G(0, 1) },
    { TOP(0, 0), TOP(1, 0), TOP(1, 1), TOP(0, 1) },
    { TOP(0, 1), TOP(1, 1), G(1, 1), G(0, 1) },
    { TOP(1, 0), TOP(0, 0), G(0, 0), G(1, 0) },
    { TOP(1, 1), TOP(1, 0), G(1, 0), G(1, 1) },
    { TOP(0, 0), TOP(0, 1), G(0, 1), G(0, 0) },
};

static void add_quad(int x, int y, int face, int colour) {
    quad_t *q = &quads[n_quads++];
    q->base = (uint16_t)G(x, y);
    q->face = (uint8_t)face;
    q->colour = (uint8_t)colour;
}

static void build_room(void) {
    for (int y = 0; y < ROOM_H; y++) {
        for (int x = 0; x < ROOM_W; x++) {
            int wall = x == 0 || y == 0 || x == ROOM_W - 1 || y == ROOM_H - 1;
            if (!wall) {
                add_quad(x, y, FACE_FLOOR, (x + y) & 1 ? FLOOR_A : FLOOR_B);
                continue;
            }
            add_quad(x, y, FACE_TOP, WALL_TOP);
            /* The four sides. Those that face away are culled each frame. */
            add_quad(x, y, FACE_SOUTH, WALL_SIDE_A);
            add_quad(x, y, FACE_NORTH, WALL_SIDE_A);
            add_quad(x, y, FACE_EAST, WALL_SIDE_B);
            add_quad(x, y, FACE_WEST, WALL_SIDE_B);
        }
    }
}

/* One divide a grid point: both levels share its depth. Depth and offset
 * are level along a row of the grid, so they are stepped, not multiplied.
 * Every shift is of a value that is not negative: the SH-2 shifts a signed
 * one with a library call. */
static void project(int32_t ex, int32_t ey, sh2z_fx_t s, sh2z_fx_t c) {
    int i = 0;
    any_near = 0;
    for (int gy = 0; gy < GRID_H; gy++) {
        int32_t ry = ((gy * CELL) << 16) - ey, rx = -ex;
        int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
        int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
        for (int gx = 0; gx < GRID_W; gx++, i++, t += CELL * c, u -= CELL * s) {
            if (t < T_NEAR << 10) {
                depth[i] = 0;
                any_near = 1;
                continue;
            }
            uint32_t d = (uint32_t)t >> 10;
            depth[i] = (int32_t)d;
            DIVU_DVSR = d;
            /* focal / depth, Q17. The divider takes its dividend as signed. */
            DIVU_DVDNT = (uint32_t)FOCAL << 23;
            uint32_t scale = DIVU_DVDNT;
            /* A column is two pixels: twice u * focal / depth. */
            int16_t x = (int16_t)(FB_W / 2 + (int32_t)(((int64_t)u * (int32_t)scale) >> 32));
            shown[0][i].x = shown[1][i].x = x;
            shown[0][i].y = (int16_t)(HORIZON + (int)((EYE * scale) >> 17));
            shown[1][i].y = (int16_t)(HORIZON + (int)(((EYE - WALL_Z) * scale) >> 17));
        }
    }
}

/* Chain each quad that faces the eye into the bucket for its depth. A
 * floor or a wall top always faces it; a wall's side does when the eye is
 * on that side of it, which is the same for a whole row or column. */
static void sort(int32_t ex, int32_t ey) {
    for (int i = 0; i < N_BUCKETS; i++) bucket[i] = -1;
    int eye_x = ex >> 16, eye_y = ey >> 16, i = 0;
#if TEX
    /* A bitmap needs no quad to each cell: the floor cells side by side
     * along a row are one quad, kept in the first one's, whose colour is
     * how many. Floor hides nothing, so it all goes in the farthest
     * bucket, which nothing in this room is far enough to be in. */
    int run = -1;
#endif
    for (int y = 0; y < ROOM_H; y++) {
        int south = eye_y > (y + 1) * CELL, north = eye_y < y * CELL;
        for (int x = 0; x < ROOM_W; x++) {
            int wall = x == 0 || y == 0 || x == ROOM_W - 1 || y == ROOM_H - 1;
            int faces = wall ? 5 : 1;
            int g = G(x, y);
            unsigned key = (unsigned)(depth[g] + depth[g + GRID_W + 1]) >> 8;   /* two voxels a bucket */
            if (key >= N_BUCKETS) key = N_BUCKETS - 1;
            if (any_near && (!depth[g] || !depth[g + 1] || !depth[g + GRID_W] || !depth[g + GRID_W + 1])) {
                i += faces;
#if TEX
                run = -1;
#endif
                continue;
            }
#define CHAIN(n) do { quads[n].next = bucket[key]; bucket[key] = (int16_t)(n); } while (0)
#if TEX
            if (!wall) {
                if (run < 0) {
                    run = i;
                    quads[i].colour = 0;
                    key = N_BUCKETS - 1;
                    CHAIN(i);
                }
                quads[run].colour++;
                i++;
                continue;
            }
            run = -1;
#endif
            CHAIN(i);
            if (wall) {
                /* A side against another wall cell is inside the wall. It
                 * must not be drawn: cells along a wall are about as far
                 * away as each other, so the buckets cannot be trusted to
                 * paint the neighbour over it. */
#define IS_WALL(x, y) ((unsigned)(x) < ROOM_W && (unsigned)(y) < ROOM_H && \
                       ((x) == 0 || (y) == 0 || (x) == ROOM_W - 1 || (y) == ROOM_H - 1))
                if (south && !IS_WALL(x, y + 1)) CHAIN(i + 1);
                if (north && !IS_WALL(x, y - 1)) CHAIN(i + 2);
                if (eye_x > (x + 1) * CELL && !IS_WALL(x + 1, y)) CHAIN(i + 3);
                if (eye_x < x * CELL && !IS_WALL(x - 1, y)) CHAIN(i + 4);
            }
            i += faces;
        }
    }
}

static inline int32_t divide(int32_t n, int32_t d) {
    DIVU_DVSR = (uint32_t)d;
    DIVU_DVDNT = (uint32_t)n;
    return (int32_t)DIVU_DVDNT;
}

/* Where each actor's columns fall, and its place among the quads: just in
 * front of the floor it stands on, which is behind everything of it. */
static void place_actors(int32_t ex, int32_t ey, sh2z_fx_t s, sh2z_fx_t c) {
    for (int k = 0; k < POLYBENCH_ACTORS; k++) {
        if (k == gone) continue;
        quad_t *q = &quads[n_quads + k];
        int g = G(actors[k].x, actors[k].y);
        if (!depth[g] || !depth[g + 1] || !depth[g + GRID_W] || !depth[g + GRID_W + 1]) continue;
        unsigned key = (unsigned)(depth[g] + depth[g + GRID_W + 1]) >> 8;
        if (key >= N_BUCKETS) key = N_BUCKETS - 1;
        if (key) key--;
        q->next = bucket[key];
        bucket[key] = (int16_t)(n_quads + k);

        /* The middle of the cell stands for all of it. */
        int32_t rx = ((actors[k].x * CELL + CELL / 2) << 16) - ex;
        int32_t ry = ((actors[k].y * CELL + CELL / 2) << 16) - ey;
        int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
        int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
        int32_t scale = divide((int32_t)FOCAL << 23, t >> 10);   /* focal / depth, Q17 */
        int32_t across = scale;                               /* pixels a voxel of u, Q16 */
        /* A voxel nearer moves a point out from the middle column by
         * across * u / t and down by eye * focal / t / t. */
        int32_t aside = -sh2z_mul(across, divide(u, t >> 16));
        int32_t down = -divide((EYE * scale) >> 1, t >> 16);
        placed_t *a = &placed[k];
        a->xx = sh2z_mul(aside, c) - sh2z_mul(across, s);
        a->xy = sh2z_mul(aside, s) + sh2z_mul(across, c);
        a->yx = sh2z_mul(down, c);
        a->yy = sh2z_mul(down, s);
        a->x = ((FB_W / 2) << 16) + (int32_t)(((int64_t)u * scale) >> 16) - (CELL / 2) * (a->xx + a->xy);
        a->y = HORIZON * 65536 + ((EYE * scale) >> 1) - (CELL / 2) * (a->yx + a->yy);
        a->rise = scale >> 1;
        a->voxels = divide(0x7FFFFFFF, scale >> 2);
        a->first_x = (int8_t)(c > 0 ? CELL - 1 : 0); a->step_x = (int8_t)(c > 0 ? -1 : 1);
        a->first_y = (int8_t)(s > 0 ? CELL - 1 : 0); a->step_y = (int8_t)(s > 0 ? -1 : 1);
    }
}

/* Far to near into the draw list, telling the secondary as it grows. */
static void emit(void) {
    int n = 0;
    for (int k = N_BUCKETS - 1; k >= 0; k--) {
        for (int i = bucket[k]; i >= 0 && n < MAX_DRAWS + MAX_ACTORS; i = quads[i].next) {
            SHARED(draws[n]) = (uint16_t)(i + 1);
            SHARED(draws_ready) = (int16_t)++n;
        }
    }
    SHARED(draws[n]) = 0;
    SHARED(draws_ready) = (int16_t)(n + 1);
    drawn = (uint16_t)n;
}

#define FILL_WAIT() do { } while (MARS_VDP_FBCTL & MARS_VDP_FEN)

/* POLYBENCH=3 shares the drawing: the primary draws the columns left of
 * SPLIT and the secondary the rest, each in list order, so each half is
 * still painted far to near. The framebuffer takes no write while a fill
 * is running, so two CPUs cannot draw at once if either uses the fill
 * hardware: here spans are written by the CPU, a word at a time, and only
 * the clear is a fill. SOFT=1 does the same with one CPU drawing. */
#define SHARE (POLYBENCH == 3)
#if SHARE || defined POLYBENCH_SOFT
#define HW_FILL 0
#else
#define HW_FILL 1
#endif
#ifndef POLYBENCH_SPLIT
#define POLYBENCH_SPLIT 160
#endif
static volatile uint16_t cleared;

static void clear(void) {
    for (int row = 0; row < FB_H; row++) {
        FILL_WAIT();
        MARS_VDP_FILLEN = FB_W / 2 - 1;
        MARS_VDP_FILADR = (uint16_t)(FIRST + row * PAGE);
        MARS_VDP_FILDAT = BACK * 0x0101;
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
static inline __attribute__((always_inline)) void fill_quad_as(const point_t *p, unsigned colour, int clip, int lo, int hi, int tex) {
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

    volatile uint16_t *vdp = &MARS_VDP_DISPMODE;
#if TEX
    /* Held here: a write to the picture could be one to the pointer. */
    const floor_t *const floor = the_floor;
    const uint16_t *const texels = floor->texels;
#endif
    (void)tex;
    int li = top, ri = top, lh = 0, rh = 0;
    int32_t xl = 0, xr = 0, dl = 0, dr = 0;
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
        uint8_t *line = (uint8_t *)&MARS_FRAMEBUFFER + (FIRST + y * PAGE) * 2;
        unsigned page = (unsigned)(FIRST + y * PAGE);
#if TEX
        int row_y = y;
#endif
        y += n;
        for (; n > 0; n--, line += PAGE * 2, page += PAGE) {
            int left = xl >> 16, right = xr >> 16;
            xl += dl; xr += dr;
#ifdef POLYBENCH_COUNT
            spans++;
#endif
            if (clip) {
                if (left < lo) left = lo;
                if (right > hi) right = hi;
            }
#if TEX
            int at_y = row_y++;
#endif
            if (left >= right) continue;
#if TEX
            if (tex) {
                floor_row_t in;
                floor_row(&in, floor, at_y);
#if TEX == 2
                int32_t u = FLOOR_U(&in, left), v = FLOOR_V(&in, left), du = -2 * in.du, dv = -2 * in.dv;
                for (uint8_t *at = line + left, *end = line + right; at < end; at++, u += du, v += dv)
                    *at = (uint8_t)TEXEL(u, v);
#else
                if (left & 1) { line[left] = (uint8_t)TEXEL(FLOOR_U(&in, left), FLOOR_V(&in, left)); left++; }
                if (right & 1) { right--; line[right] = (uint8_t)TEXEL(FLOOR_U(&in, right), FLOOR_V(&in, right)); }
                int words = (right - left) >> 1;
                /* A word back is two pixels back: four of the row's steps. */
                if (words > 0)
                    floor_words((uint16_t *)(void *)(line + right), words, FLOOR_U(&in, right - 2),
                                FLOOR_V(&in, right - 2), -4 * in.du, -4 * in.dv, texels);
#endif
                continue;
            }
#endif
            if (HW_FILL) while (vdp[5] & MARS_VDP_FEN);
            if (left & 1) line[left++] = (uint8_t)colour;
            if (right & 1) line[--right] = (uint8_t)colour;
            int words = (right - left) >> 1;
            if (!HW_FILL) {
                uint16_t *at = (uint16_t *)(void *)(line + left);
                for (; words > 0; words--) *at++ = (uint16_t)colour;
            } else if (words > 2) {
                vdp[2] = (uint16_t)(words - 1);
                vdp[3] = (uint16_t)(page + (unsigned)(left >> 1));
                vdp[4] = (uint16_t)colour;
            } else if (words > 0) {
                uint16_t *at = (uint16_t *)(void *)(line + left);
                at[0] = (uint16_t)colour;
                if (words > 1) at[1] = (uint16_t)colour;
            }
        }
    }
}

/* A creature, a column at a time from the far corner to the near one. A
 * column is the box round where its footprint falls, as wide as whole
 * words: its top in the colour of its highest voxel, and under that its
 * side, a row at a time in the colour of the voxel at that height, as far
 * down as the columns in front leave it showing. Words
 * are written directly; none is long enough to be worth a fill. */
static void draw_actor(const placed_t *a, const vcol_t model[CELL][CELL], int lo, int hi) {
    int32_t left_by = (a->xx < 0 ? a->xx : 0) + (a->xy < 0 ? a->xy : 0);
    int32_t right_by = (a->xx > 0 ? a->xx : 0) + (a->xy > 0 ? a->xy : 0);
    int32_t far_by = (a->yx < 0 ? a->yx : 0) + (a->yy < 0 ? a->yy : 0);
    int32_t near_by = (a->yx > 0 ? a->yx : 0) + (a->yy > 0 ? a->yy : 0);
    /* No creature is 48 pixels from its middle to its edge. */
    int middle = (a->x + (CELL / 2) * (a->xx + a->xy)) >> 16;
    if (middle + 48 < lo || middle - 48 >= hi) return;
    lo >>= 1; hi >>= 1;
    if (HW_FILL) FILL_WAIT();
    for (int i = 0, mx = a->first_x; i < CELL; i++, mx += a->step_x) {
        for (int j = 0, my = a->first_y; j < CELL; j++, my += a->step_y) {
            const vcol_t *col = &model[mx][my];
            if (!col->h) continue;
            int32_t x = a->x + mx * a->xx + my * a->xy;
            int32_t y = a->y + mx * a->yx + my * a->yy;
            int left = (x + left_by) >> 17, right = (x + right_by + 0x1FFFF) >> 17;
            int base = (y + near_by) >> 16, back = (y + far_by) >> 16;
            int rise = (col->h * a->rise) >> 16;
            int top = back - rise, lip = base - rise;
            if (left < lo) left = lo;
            if (right > hi) right = hi;
            if (left >= right || top >= FB_H || base <= 0) continue;
            if (lip <= top) lip = top + 1;
            int width = right - left;
            uint16_t *at = (uint16_t *)(void *)((uint8_t *)&MARS_FRAMEBUFFER + (FIRST + top * PAGE) * 2) + left;
            unsigned colour = (unsigned)(col->c[col->h - 1] + 6) * 0x0101u;
            int row = top;
            for (; row < lip; row++, at += PAGE) {
                if ((unsigned)row >= FB_H) continue;
                for (int w = 0; w < width; w++) at[w] = (uint16_t)colour;
            }
            /* The side shows down to the lower of the two columns in
             * front of this one; they hide the rest of it. */
            int nx = mx + a->step_x, ny = my + a->step_y;
            int front_x = (unsigned)nx < CELL ? model[nx][my].h : 0;
            int front_y = (unsigned)ny < CELL ? model[mx][ny].h : 0;
            int hidden = front_x < front_y ? front_x : front_y;
            if (hidden >= col->h) continue;
            base -= (hidden * a->rise) >> 16;
            int32_t z = (int32_t)col->h << 16;
            for (; row < base && row < FB_H; row++, at += PAGE) {
                z -= a->voxels;
                if (row < 0) continue;
                colour = (unsigned)(col->c[z > 0 ? z >> 16 : 0] + 3) * 0x0101u;
                for (int w = 0; w < width; w++) at[w] = (uint16_t)colour;
            }
        }
    }
}

static void fill_quad_clipped(const point_t *p, unsigned colour, int lo, int hi) { fill_quad_as(p, colour, 1, lo, hi, 0); }
static void fill_quad_inside(const point_t *p, unsigned colour) { fill_quad_as(p, colour, 0, 0, 0, 0); }
#if TEX
static void floor_quad_clipped(const point_t *p, int lo, int hi) { fill_quad_as(p, 0, 1, lo, hi, 1); }
static void floor_quad_inside(const point_t *p) { fill_quad_as(p, 0, 0, 0, 0, 1); }
#endif

/* Draws what the primary has listed, waiting for more when it catches up,
 * in the columns from lo up to hi. */
static void draw_list(int lo, int hi) {
    int n = 0;
    for (;;) {
        int ready;
        while ((ready = SHARED(draws_ready)) <= n);
        for (; n < ready; n++) {
            /* The list is read round this CPU's cache, which could have
             * taken in a line of it that the primary had not finished. The
             * points were all written before the list was begun. */
            int i = SHARED(draws[n]);
            if (!i) return;
            const quad_t *q = &quads[i - 1];
            if (q->face == FACE_ACTOR) {
                draw_actor(&placed[q->base], ent_solid[actors[q->base].model], lo, hi);
                continue;
            }
            const point_t *all = &shown[0][0];
            const uint16_t *at = corner[q->face];
            int base = q->base;
#if TEX
            /* A run of floor: its corners are its length apart. */
            uint16_t ends[4];
            if (q->face == FACE_FLOOR) {
                ends[0] = G(0, 0); ends[1] = (uint16_t)G(q->colour, 0);
                ends[2] = (uint16_t)G(q->colour, 1); ends[3] = G(0, 1);
                at = ends;
            }
#endif
            /* A point is copied as one long: the compiler copies a
             * structure of two shorts a byte at a time. */
            typedef uint32_t __attribute__((may_alias)) both_t;
            const both_t *from = (const both_t *)(const void *)all;
            point_t p[4] __attribute__((aligned(4)));
            both_t *to = (both_t *)(void *)p;
            to[0] = from[base + at[0]]; to[1] = from[base + at[1]];
            to[2] = from[base + at[2]]; to[3] = from[base + at[3]];
            int left = p[0].x, right = left, up = p[0].y, down = up;
            for (int k = 1; k < 4; k++) {
                if (p[k].x < left) left = p[k].x;
                if (p[k].x > right) right = p[k].x;
                if (p[k].y < up) up = p[k].y;
                if (p[k].y > down) down = p[k].y;
            }
            if (right <= lo || left >= hi) continue;
#if TEX
            if (q->face == FACE_FLOOR) {
                if (left >= lo && right <= hi && up >= 0 && down <= FB_H) floor_quad_inside(p);
                else floor_quad_clipped(p, lo, hi);
                continue;
            }
#endif
            if (left >= lo && right <= hi && up >= 0 && down <= FB_H) fill_quad_inside(p, q->colour * 0x0101u);
            else fill_quad_clipped(p, q->colour * 0x0101u, lo, hi);
        }
    }
}

#define SECONDARY_DRAW 0x5042
void polybench_secondary(void) {
    while (MARS_SYS_COMM6 != SECONDARY_DRAW);
    CacheControl(SH2_CCTL_CP | SH2_CCTL_CE);
    clear();
    FILL_WAIT();
    SHARED(cleared) = 1;
    draw_list(SHARE ? POLYBENCH_SPLIT : 0, FB_W);
    FILL_WAIT();
    MARS_SYS_COMM6 = 0;
}

static unsigned chance(void) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

/* Every few voxels of the model becomes a piece, thrown outward from the
 * middle and up. */
static void shatter(int k) {
    const vcol_t (*model)[CELL] = ent_solid[actors[k].model];
    int total = 0, n = 0, made = 0;
    for (int x = 0; x < CELL; x++) for (int y = 0; y < CELL; y++) total += model[x][y].h;
    int skip = total / MAX_PIECES + 1;
    for (int x = 0; x < CELL; x++) {
        for (int y = 0; y < CELL; y++) {
            for (int z = 0; z < model[x][y].h; z++) {
                if (n++ % skip || made == MAX_PIECES) continue;
                piece_t *p = &pieces[made++];
                p->x = (int16_t)((actors[k].x * CELL + x) << 4);
                p->y = (int16_t)((actors[k].y * CELL + y) << 4);
                p->z = (int16_t)(z << 4);
                p->vx = (int8_t)((x * 2 - 7) * 2 + (int)(chance() & 7) - 4);
                p->vy = (int8_t)((y * 2 - 7) * 2 + (int)(chance() & 7) - 4);
                p->vz = (int8_t)(16 + (chance() & 15));
                p->life = PIECE_LIFE;
                p->colour = model[x][y].c[z];
            }
        }
    }
    gone = k;
}

static void move_pieces(void) {
    int alive = 0;
    for (int i = 0; i < MAX_PIECES; i++) {
        piece_t *p = &pieces[i];
        if (!p->life) continue;
        p->life--;
        alive = 1;
        if (p->z == 0 && p->vz <= 0) continue;         /* lying where it fell */
        p->x = (int16_t)(p->x + p->vx);
        p->y = (int16_t)(p->y + p->vy);
        p->z = (int16_t)(p->z + p->vz);
        p->vz = (int8_t)(p->vz - 3);
        if (p->z <= 0) { p->z = 0; p->vz = 0; }
    }
    if (!alive) gone = -1;
}

/* Pieces go over the finished picture, each projected by itself: a lit
 * top over a darker side, two words by two rows. One behind a wall shows
 * through it for the moment it lasts, as in the game. */
static void draw_pieces(int32_t ex, int32_t ey, sh2z_fx_t s, sh2z_fx_t c) {
    FILL_WAIT();
    for (int i = 0; i < MAX_PIECES; i++) {
        const piece_t *p = &pieces[i];
        if (!p->life) continue;
        int32_t rx = (p->x << 12) - ex, ry = (p->y << 12) - ey;
        int32_t t = sh2z_mul(rx, c) + sh2z_mul(ry, s);
        int32_t u = sh2z_mul(ry, c) - sh2z_mul(rx, s);
        if (t < T_NEAR << 10) continue;
        int32_t scale = divide((int32_t)FOCAL << 23, t >> 10);
        int word = (FB_W / 2 + (int)(((int64_t)u * scale) >> 32)) >> 1;
        int row = HORIZON + (int)((uint32_t)((EYE * 16 - p->z) * scale) >> 21);
        if ((unsigned)word >= FB_W / 2 - 1 || (unsigned)row >= FB_H - 1) continue;
        uint16_t *at = (uint16_t *)(void *)((uint8_t *)&MARS_FRAMEBUFFER + (FIRST + row * PAGE) * 2) + word;
        unsigned colour = p->colour;
        at[0] = (uint16_t)((colour + 7) * 0x0101u);
        at[1] = (uint16_t)((colour + 6) * 0x0101u);
        at[PAGE] = (uint16_t)((colour + 6) * 0x0101u);
        at[PAGE + 1] = (uint16_t)((colour + 4) * 0x0101u);
    }
}

static void flip(void) {
    static uint16_t fb;
    FILL_WAIT();
    MARS_VDP_FBCTL = fb ^ 1;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) == fb);
    fb ^= 1;
}

static void screen(void) {
    volatile uint16_t *cram = &MARS_CRAM;
    cram[BACK] = COLOR(1, 1, 3);
    cram[FLOOR_A] = COLOR(9, 8, 7);
    cram[FLOOR_B] = COLOR(11, 10, 9);
    cram[WALL_TOP] = COLOR(22, 20, 17);
    cram[WALL_SIDE_A] = COLOR(15, 13, 11);
    cram[WALL_SIDE_B] = COLOR(11, 9, 8);
    cram[FLOOR_GROUT] = COLOR(6, 5, 5);
    cram[FLOOR_SPECK] = COLOR(13, 12, 11);
    for (int both = 0; both < 2; both++) {
        volatile uint16_t *lines = &MARS_FRAMEBUFFER;
        for (int k = 0; k < 224; k++) lines[k] = (uint16_t)(FIRST + (k / 2) * PAGE);
        clear();
        flip();
    }
}

static char *number(char *p, const char *label, unsigned v) {
    char digits[6];
    int n = 0;
    while (*label) *p++ = *label++;
    do digits[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) *p++ = digits[--n];
    return p;
}

void polybench_run(void) {
    recip[0] = 0;
    for (int n = 2; n < 256; n++) recip[n] = (uint16_t)(65536u / (unsigned)n);
    build_room();
#if TEX
    build_floor();
#endif
    for (int k = 0; k < MAX_ACTORS; k++) {
        quads[n_quads + k].face = FACE_ACTOR;
        quads[n_quads + k].base = (uint16_t)k;
    }
    screen();

    uint16_t yaw = 0;
    uint32_t sums[4] = { 0, 0, 0, 0 };
    unsigned frames = 0, worst = 0;
    for (;;) {
        sh2z_fx_t s, c;
        sh2z_sincos(yaw, &s, &c);
        /* The hero stands in the middle; the eye is as far back as it is high. */
        int32_t ex = ((ROOM_W * CELL / 2) << 16) - c * EYE;
        int32_t ey = ((ROOM_H * CELL / 2) << 16) - s * EYE;

        if (POLYBENCH_SHATTER && POLYBENCH_ACTORS > 0) {
            static unsigned turn, next;
            if (turn++ % SHATTER_EVERY == 0) {
                /* Not the hero while there is anyone else. */
                next = POLYBENCH_ACTORS > 1 ? next % (POLYBENCH_ACTORS - 1) + 1 : 0;
                shatter((int)next);
            }
            move_pieces();
        }

        uint16_t t0 = clock();
        SHARED(draws_ready) = 0;
        SHARED(cleared) = 0;
#if POLYBENCH != 2
        MARS_SYS_COMM6 = SECONDARY_DRAW;
#else
        clear();
        FILL_WAIT();
#endif
        uint16_t t1 = clock();
        project(ex, ey, s, c);
#if TEX
        the_floor->u = ex * 16; the_floor->v = ey * 2;
        the_floor->s = s; the_floor->c = c;
#endif
        sort(ex, ey);
        place_actors(ex, ey, s, c);
        emit();
        uint16_t t2 = clock();
#if POLYBENCH == 3
        while (!SHARED(cleared));
        draw_list(0, POLYBENCH_SPLIT);
#endif
#if POLYBENCH != 2
        while (MARS_SYS_COMM6 == SECONDARY_DRAW);
#else
        draw_list(0, FB_W);
        FILL_WAIT();
#endif
        if (POLYBENCH_SHATTER && gone >= 0) draw_pieces(ex, ey, s, c);
        uint16_t t3 = clock();

        unsigned total = (uint16_t)(t3 - t0);
        sums[0] += total;
        sums[1] += (uint16_t)(t1 - t0);
        sums[2] += (uint16_t)(t2 - t1);
        sums[3] += (uint16_t)(t3 - t2);
        if (total > worst) worst = total;
        flip();
        yaw += 0x100;

        if (++frames == 256) {
            polybench_result.total = (uint16_t)(sums[0] >> 8);
            polybench_result.worst = (uint16_t)worst;
            polybench_result.clear = (uint16_t)(sums[1] >> 8);
            polybench_result.geometry = (uint16_t)(sums[2] >> 8);
            polybench_result.wait = (uint16_t)(sums[3] >> 8);
            polybench_result.quads = drawn;
            polybench_result.spans = spans;
            polybench_result.runs++;

            char line[TEXT_W + 1], *p = line;
            p = number(p, "TICKS ", polybench_result.total);
            p = number(p, " WORST ", polybench_result.worst);
            p = number(p, " QUADS ", drawn);
            *p = 0;
            text_begin();
            text_put(1, 26, line, -1, TEXT_YELLOW, 1);
            text_commit();
            frames = worst = 0;
            sums[0] = sums[1] = sums[2] = sums[3] = 0;
        }
        spans = 0;
    }
}
#else
typedef int polybench_unused;
#endif
