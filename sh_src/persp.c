/* Only PERSP builds have anything in this file. */
typedef int persp_unit;

#ifdef PERSP
#include "mars.h"
#include "string.h"
#include "voxel.h"
#include "voxel_world.h"
#include "persp.h"
#include "sh2zam/sh2zam.h"
#include "baked3d.h"

/* A voxel-space renderer. The world is solid columns on a grid, so each
 * screen column is one ray across the ground: step from grid line to grid
 * line, and every column of voxels the ray enters adds its near side and its
 * top to the picture, nearest first, each drawn above what is already there.
 *
 * The camera looks level and the picture is slid down (a horizon far above
 * the screen), which is what makes it look down at the hero while vertical
 * things stay vertical. With the eye as high as it is far and the focal
 * length equal to both, a voxel at the hero is one fat pixel wide, tall and
 * deep, the same scale as the fixed view. The eye is always as high as it
 * is far back, so the horizon stays put when it pulls away to show more. */
#define FOCAL_NEAR  128
#define PULL_BACK   160                    /* /256 farther when pulled all the way back */

/* The focal length is not fixed. Lengthening it, with the eye going back
 * and up to match, takes the perspective out of the picture; by FOCAL_FLAT
 * a voxel is one fat pixel wherever it is, which is the fixed view's
 * projection. These are the focal lengths along the way, evenly spaced by
 * ratio. */
#define FOCAL_FLAT  3072
static const uint16_t focal_step[17] = { 128, 156, 190, 232, 283, 346, 421, 514, 627, 765, 933, 1138, 1388, 1693, 2065, 2519, 3072 };

/* The hero shows on this row, as in the fixed view. */
#define TARGET_ROW  (VIEW_H / 2 + 4)

/* Depths are Q16 along the view direction. The two tables below are
 * entered by depth past the nearest that is on screen, in quarter voxels. */
#define INV_STEP  14
#define INV_N     1024

#define FB_PIXELS 0x100                    /* after the line table */
#define MAX_BUMPS 32

#define NEVER     (INT32_MAX / 2)
#define DETAIL    0xFF

static uint32_t inv_t[INV_N];              /* focal length / depth, Q16 */
static uint32_t rise_t[INV_N];             /* depth / focal length, Q16: voxels per row */
/* Depth, in Q28, at which a level surface one voxel below the eye shows on
 * each row. One h voxels below shows at h times that. */
static uint32_t row_t[2 * VIEW_H];

/* One frame's camera, shared by both CPUs. The primary fills it in and
 * raises COMM6; the secondary draws the odd columns, the primary the even
 * ones, and the secondary lowers COMM6 when its share is done. */
static struct {
    uint16_t *fb;
    int32_t ex, ey;
    sh2z_fx_t s, c;
    int32_t focal, horizon;
    int32_t eye_h, t_near, t_far;
    int32_t t_focus;       /* depth of the ground on the row a focus ray starts from */
    int res;               /* PERSP_RES_x: how finely the picture is drawn */
    int cols, rows;
} job;

/* The framebuffer is 320 bytes a row whatever the resolution. A coarse
 * column is two pixels wide, written as one word; a coarse row is shown on
 * two scanlines by the line table. */
#define STRIDE    320
typedef uint16_t __attribute__((may_alias)) pair_t;

/* The 32X does not store a zero written to its framebuffer as a byte, so
 * that sprites can be drawn with holes in them. Nothing is CRAM index 1,
 * which the palette keeps as black as index 0. */
#define BLACK     0x0101

#define RUN_MAX   15

/* The colour on top of each column of a model, 64 bytes where the model
 * itself spreads them over a kilobyte. */
static uint8_t tops[VOX_MODELS_MAX][CELL * CELL];

/* All a ray needs to know about a cell, in one cache line. */
typedef struct {
    const vmodel_t *m;
    const uint8_t *top;
    uint8_t flat;          /* its one height, plinth and all, or DETAIL */
    uint8_t light;         /* top-face shade. For an empty cell, how many
                            * cells a ray can cross from here, whichever way
                            * it goes, before it needs to look again */
    uint8_t lift;
    uint8_t plinth;
    uint8_t pad[4];
} pcell_t;

typedef char pcell_is_a_line[sizeof(pcell_t) == 16 ? 1 : -1];
static pcell_t cells[MAP_H][MAP_W];

/* A cell with a creature in it gets a model of its own: the terrain's, with
 * the creature's voxel columns standing on it. */
static vmodel_t bump[MAX_BUMPS];
static struct { uint8_t cx, cy; } bumped[MAX_BUMPS];
static int n_bumped;
static uint8_t model_flat[VOX_MODELS_MAX];

static unsigned flatness(const vmodel_t *m);

void persp_init(void) {
    for (int m = 0; m < vox_model_count; m++) {
        model_flat[m] = (uint8_t)flatness(&vox_models[m]);
        for (int i = 0; i < CELL * CELL; i++) {
            const vcol_t *c = &vox_models[m].col[i >> 3][i & 7];
            tops[m][i] = c->h ? c->c[c->h - 1] : 0;
        }
    }

}

/* The height of a model if all its columns share one, otherwise DETAIL. */
static unsigned flatness(const vmodel_t *m) {
    unsigned h = m->col[0][0].h;
    for (int lx = 0; lx < CELL; lx++) {
        for (int ly = 0; ly < CELL; ly++) {
            if (m->col[lx][ly].h != h) return DETAIL;
        }
    }
    return h;
}

/* Make a cell what the terrain alone says it is. An empty cell's run is
 * left for persp_sync to work out. */
static void terrain(int cx, int cy) {
    pcell_t *pc = &cells[cy][cx];
    unsigned tile = vox_tile[cy][cx];
    unsigned h = model_flat[tile];

    pc->m = &vox_models[tile];
    pc->top = tops[tile];
    pc->flat = (uint8_t)(h == DETAIL ? DETAIL : h + vox_lift[cy][cx]);
    pc->lift = vox_lift[cy][cx];
    pc->plinth = vox_plinth[tile];
    if (pc->flat) pc->light = vox_light[cy][cx];
}

static unsigned run_at(int cx, int cy) {
    return cells[cy][cx].flat ? 0 : cells[cy][cx].light;
}

void persp_sync(void) {
    n_bumped = 0;
    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) terrain(cx, cy);
    }

    /* Distance, in cells and counting diagonals as one, from each empty
     * cell to the nearest that is not: two sweeps. The map's edge counts
     * as not empty, so a run never leaves the map. */
    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            if (cells[cy][cx].flat) continue;
            unsigned d = 0;
            if (cx > 0 && cy > 0 && cx < MAP_W - 1) {
                d = run_at(cx - 1, cy);
                if (run_at(cx - 1, cy - 1) < d) d = run_at(cx - 1, cy - 1);
                if (run_at(cx, cy - 1) < d) d = run_at(cx, cy - 1);
                if (run_at(cx + 1, cy - 1) < d) d = run_at(cx + 1, cy - 1);
            }
            if (d < RUN_MAX) d++;
            cells[cy][cx].light = (uint8_t)d;
        }
    }
    for (int cy = MAP_H - 1; cy >= 0; cy--) {
        for (int cx = MAP_W - 1; cx >= 0; cx--) {
            if (cells[cy][cx].flat) continue;
            unsigned d = cells[cy][cx].light;
            if (cy == MAP_H - 1 || cx == MAP_W - 1 || cx == 0) d = 1;
            else {
                if (run_at(cx + 1, cy) + 1 < d) d = run_at(cx + 1, cy) + 1;
                if (run_at(cx + 1, cy + 1) + 1 < d) d = run_at(cx + 1, cy + 1) + 1;
                if (run_at(cx, cy + 1) + 1 < d) d = run_at(cx, cy + 1) + 1;
                if (run_at(cx - 1, cy + 1) + 1 < d) d = run_at(cx - 1, cy + 1) + 1;
            }
            cells[cy][cx].light = (uint8_t)d;
        }
    }
    persp_creatures();
}

/* The cell's own copy of its model, to stand a creature on. 0 if the cell
 * is not somewhere a creature stands (nothing, or a wall), or there are no
 * copies left. */
static vmodel_t *bump_for(int cx, int cy) {
    if ((unsigned)cx >= MAP_W || (unsigned)cy >= MAP_H) return 0;
    for (int i = 0; i < n_bumped; i++) {
        if (bumped[i].cx == cx && bumped[i].cy == cy) return &bump[i];
    }
    unsigned h = model_flat[vox_tile[cy][cx]];
    if (cells[cy][cx].flat == 0 || (h != DETAIL && h > 3) || n_bumped == MAX_BUMPS) return 0;

    vmodel_t *m = &bump[n_bumped];
    bumped[n_bumped].cx = (uint8_t)cx;
    bumped[n_bumped].cy = (uint8_t)cy;
    n_bumped++;
    *m = vox_models[vox_tile[cy][cx]];
    cells[cy][cx].m = m;
    cells[cy][cx].flat = DETAIL;
    return m;
}

/* Stand every creature where it is this frame. A creature is 8x8 columns
 * of voxels, turned to face the way it last moved; walking between cells
 * it stands partly on each. */
void persp_creatures(void) {
    vox_creature_t c;

    while (n_bumped > 0) {
        n_bumped--;
        terrain(bumped[n_bumped].cx, bumped[n_bumped].cy);
    }
    for (int i = 0; vox_creature(i, &c); i++) {
        for (int fx = 0; fx < CELL; fx++) {
            for (int fy = 0; fy < CELL; fy++) {
                /* The footprint, turned: its front is its y = 7 edge. */
                const vcol_t *src;
                switch (c.face) {
                default: src = &ent_solid[c.kind][fx][fy]; break;
                case 1:  src = &ent_solid[c.kind][fy][CELL - 1 - fx]; break;
                case 2:  src = &ent_solid[c.kind][CELL - 1 - fx][CELL - 1 - fy]; break;
                case 3:  src = &ent_solid[c.kind][CELL - 1 - fy][fx]; break;
                }
                if (!src->h) continue;

                int wx = c.x + fx, wy = c.y + fy;
                vmodel_t *m = bump_for(wx >> 3, wy >> 3);
                if (!m) continue;
                vcol_t *dst = &m->col[wx & 7][wy & 7];
                /* A hop lifts it; what is under its feet reads as shadow. */
                for (int z = 0; z < c.z && dst->h < COL_MAXH; z++) {
                    dst->c[dst->h++] = src->c[0];
                }
                for (int z = 0; z < src->h && dst->h < COL_MAXH; z++) {
                    dst->c[dst->h++] = c.flash ? (uint8_t)(c.flash & ~7) : src->c[z];
                }
            }
        }
    }
}

/* One screen column. The ray crosses the map a cell at a time. A cell that
 * is the same height all over (nothing, floor, wall) is settled in one go:
 * the side the ray enters by, then the top, each row worked back to the
 * voxel it shows. Any other cell is walked voxel column by voxel column,
 * each adding its near side and its top. Everything is drawn nearest first
 * and bottom up. */
/* WIDE and YS are constants where this is used, so each resolution gets a
 * copy of its own with nothing left to decide per pixel: WIDE for columns
 * two pixels wide, YS 1 for rows one scanline tall. */
/* The focus window: the middle of the screen, where the hero and what is
 * next to him always are, drawn fine while the rest is drawn coarse. In
 * scanlines and screen pixels. A fine ray for it starts FOCUS_RUNUP rows
 * below, so that whatever stands that near and reaches up into the window
 * is in place before the first row that shows. */
#define FOCUS_X0     128
#define FOCUS_X1     192
#define FOCUS_Y0     56
#define FOCUS_Y1     150
#define FOCUS_RUNUP  32
#define FOCUS_START  (FOCUS_Y1 + FOCUS_RUNUP)

/* How a ray's pixels reach the framebuffer, all constants where used:
 *   WIDE    a pixel is two screen pixels wide, written as one word
 *   YS      1 when a row is one scanline, 0 when it is two
 *   TWICE   in the rows the focus window is on, the two scanlines of a row
 *           are two rows of the framebuffer and both are written; elsewhere,
 *           and always without TWICE, the line table shows one row twice
 *   CLIP    CLIP_HOLE leaves the focus window's rows alone, CLIP_FOCUS
 *           draws only them
 * y is the row the next pixel goes on, counted in this ray's own rows. */
enum { CLIP_NONE, CLIP_HOLE, CLIP_FOCUS };

#define PIXEL(w) do {                                                        \
        if (CLIP == CLIP_NONE ||                                             \
            (CLIP == CLIP_HOLE ? (unsigned)(y - FOCUS_Y0 / 2) >= (FOCUS_Y1 - FOCUS_Y0) / 2 \
                               : y < FOCUS_Y1)) {                            \
            if (WIDE) {                                                      \
                *(pair_t *)dst = (uint16_t)(w);                              \
                if (TWICE && (unsigned)(y - FOCUS_Y0 / 2) < (FOCUS_Y1 - FOCUS_Y0) / 2) \
                    *(pair_t *)(dst + STRIDE) = (uint16_t)(w);               \
            } else {                                                         \
                *dst = (uint8_t)(w);                                         \
            }                                                                \
        }                                                                    \
        dst -= TWICE ? 2 * STRIDE : STRIDE;                                  \
        y--;                                                                 \
    } while (0)

/* A run of rows that would all be clipped is stepped over, not worked out. */
#define HIDDEN(top) ((CLIP == CLIP_FOCUS && (top) >= FOCUS_Y1) ||                          \
                     (CLIP == CLIP_HOLE && ybuf <= FOCUS_Y1 / 2 && (top) >= FOCUS_Y0 / 2))
#define PASS(top) do {                                                 \
        dst -= (ybuf - (top)) * (TWICE ? 2 * STRIDE : STRIDE);         \
        y -= ybuf - (top);                                             \
    } while (0)

static inline __attribute__((always_inline))
void ray(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy,
         int32_t tdx, int32_t tdy, const int WIDE, const int YS,
         const int TWICE, const int CLIP) {
    const int32_t EYE_H = job.eye_h, T_NEAR = job.t_near, T_FAR = job.t_far;
    const int32_t HORIZON = job.horizon << YS;
    /* A focus ray draws rows YEND up to FOCUS_START and starts as deep as
     * the ground is on the row it starts from; any other draws them all. */
    const int YEND = CLIP == CLIP_FOCUS ? FOCUS_Y0 : 0;
    const int32_t T_BEGIN = CLIP == CLIP_FOCUS ? job.t_focus : T_NEAR;
#define INV(t)  inv_t[((t) - T_NEAR) >> INV_STEP]
#define RISE(t) rise_t[((t) - T_NEAR) >> INV_STEP]
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int32_t px = ex + (int32_t)(((int64_t)dx * T_BEGIN) >> 16);
    int32_t py = ey + (int32_t)(((int64_t)dy * T_BEGIN) >> 16);
    int cx = px >> 19, cy = py >> 19;

    /* Depth at the next cell line on each axis, and between cell lines. */
    int32_t fx = px & 0x7FFFF, fy = py & 0x7FFFF;
    int32_t tcx = tdx >= NEVER ? NEVER
        : T_BEGIN + (int32_t)(((int64_t)(dx < 0 ? fx : 0x80000 - fx) * tdx) >> 16);
    int32_t tcy = tdy >= NEVER ? NEVER
        : T_BEGIN + (int32_t)(((int64_t)(dy < 0 ? fy : 0x80000 - fy) * tdy) >> 16);
    int32_t tdcx = tdx >= NEVER / 8 ? NEVER : tdx * 8;
    int32_t tdcy = tdy >= NEVER / 8 ? NEVER : tdy * 8;

    /* Where the ray is at a depth: the eye plus direction times depth, as
     * Q20 times Q12 so the voxel number is simply the product's high word. */
    int32_t dx20 = dx << 4, dy20 = dy << 4;
    int64_t ex48 = (int64_t)ex << 16, ey48 = (int64_t)ey << 16;
#define AT_X(t12) ((int)(((int64_t)dx20 * (int32_t)(t12) + ex48) >> 32))
#define AT_Y(t12) ((int)(((int64_t)dy20 * (int32_t)(t12) + ey48) >> 32))

    int32_t t_in = T_BEGIN;
    int ybuf = CLIP == CLIP_FOCUS ? FOCUS_START : VIEW_H << YS;   /* rows from here down are drawn */
    int y = ybuf - 1;
    int prev_h = 0, x_side = 0;

    for (;;) {
        int x_next = tcx < tcy;
        int32_t t_out = x_next ? tcx : tcy;
        if (t_out > T_FAR) t_out = T_FAR;

        unsigned kind = 0, st = 0, plinth = 0;
        int lift = 0;                      /* the cell's model stands this high */
        const vmodel_t *m = 0;
        const uint8_t *tex = 0;
        if ((unsigned)cx < MAP_W && (unsigned)cy < MAP_H) {
            const pcell_t *pc = &cells[cy][cx];
            kind = pc->flat;
            if (kind == 0) {
                /* Nothing here, nor for some way: cross it all at once. */
                for (int n = pc->light - 1; n > 0 && t_out < T_FAR; n--) {
                    if (x_next) { cx += sx; tcx += tdcx; }
                    else        { cy += sy; tcy += tdcy; }
                    x_next = tcx < tcy;
                    t_out = x_next ? tcx : tcy;
                }
                if (t_out > T_FAR) t_out = T_FAR;
            } else {
                m = pc->m;
                tex = pc->top;
                st = pc->light;
                lift = pc->lift;
                plinth = pc->plinth;
            }
        }

        if (kind == 0) {
            uint32_t s = INV(t_out);
            int top = HORIZON + (int)(((uint32_t)EYE_H * s) >> (16 - YS));
            if (top < YEND) top = YEND;
            if (top < ybuf) {
                if (HIDDEN(top)) PASS(top);
                else for (int n = ybuf - top; n > 0; n--) {
                    PIXEL(BLACK);
                }
                ybuf = top;
                if (ybuf == YEND) return;
            }
            prev_h = 0;
        } else if (kind != DETAIL) {
            int h = (int)kind;

            /* The side the ray comes in by: one voxel column of it. */
            if (h > prev_h) {
                uint32_t s = INV(t_in);
                int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> (16 - YS));
                if (top < YEND) top = YEND;
                if (top < ybuf) {
                    int32_t tq = (t_in >> 4) + 1;
                    const uint8_t *c = m->col[AT_X(tq) & 7][AT_Y(tq) & 7].c;
                    unsigned sf = vox_front_shade[st];
                    if (x_side && sf) sf--;
                    int32_t zstep = (int32_t)RISE(t_in) >> YS;
                    int32_t z = (EYE_H << 16) - (ybuf - 1 - HORIZON) * zstep;
                    if (HIDDEN(top)) PASS(top);
                    else for (int n = ybuf - top; n > 0; n--) {
                        int zi = z >> 16;
                        if (zi < 0) zi = 0;
                        if (zi >= h) zi = h - 1;
                        PIXEL(vox_wtab[(zi < lift ? plinth : c[zi - lift]) + sf]);
                        z += zstep;
                    }
                    ybuf = top;
                    if (ybuf == YEND) return;
                }
            }

            /* The top, each row worked back to the voxel under it. */
            uint32_t s = INV(t_out);
            int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> (16 - YS));
            if (top < YEND) top = YEND;
            if (top < ybuf) {
                const uint32_t *pt = &row_t[(ybuf - 1) << (1 - YS)];
                int32_t below = EYE_H - h;
                if (HIDDEN(top)) PASS(top);
                else for (int n = ybuf - top; n > 0; n--) {
                    int32_t t = (int32_t)(((uint64_t)*pt * (uint32_t)below) >> 16);
                    pt -= 2 >> YS;
                    PIXEL(vox_wtab[tex[(AT_X(t) & 7) * CELL + (AT_Y(t) & 7)] + st]);
                }
                ybuf = top;
                if (ybuf == YEND) return;
            }
            prev_h = h;
        } else {
            /* Walk the voxel columns of this cell. Where the ray is as it
             * comes in, nudged inside so rounding cannot put it next door. */
            int32_t tq = (t_in >> 4) + 1;
            int ix = AT_X(tq), iy = AT_Y(tq);
            if (ix >> 3 != cx) ix = cx * 8 + (ix >> 3 < cx ? 0 : 7);
            if (iy >> 3 != cy) iy = cy * 8 + (iy >> 3 < cy ? 0 : 7);

            /* Depth at this voxel's far line on each axis: the cell's own
             * far line, less the voxel lines still to cross before it. */
            int32_t tvx = tdx >= NEVER ? NEVER : tcx - tdx * (dx < 0 ? ix & 7 : 7 - (ix & 7));
            int32_t tvy = tdy >= NEVER ? NEVER : tcy - tdy * (dy < 0 ? iy & 7 : 7 - (iy & 7));
            int32_t tv_in = t_in;
            unsigned sf0 = vox_front_shade[st];

            for (;;) {
                const vcol_t *col = &m->col[ix & 7][iy & 7];
                int h = col->h + lift;

                /* The side that faces the eye, where the ray came in. */
                if (h > prev_h) {
                    uint32_t s = INV(tv_in);
                    int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> (16 - YS));
                    if (top < YEND) top = YEND;
                    if (top < ybuf) {
                        unsigned sf = (x_side && sf0) ? sf0 - 1 : sf0;
                        int32_t zstep = (int32_t)RISE(tv_in) >> YS;
                        int32_t z = (EYE_H << 16) - (ybuf - 1 - HORIZON) * zstep;
                        if (HIDDEN(top)) PASS(top);
                        else for (int n = ybuf - top; n > 0; n--) {
                            int zi = z >> 16;
                            if (zi < 0) zi = 0;
                            if (zi >= h) zi = h - 1;
                            PIXEL(vox_wtab[(zi < lift ? plinth : col->c[zi - lift]) + sf]);
                            z += zstep;
                        }
                        ybuf = top;
                        if (ybuf == YEND) return;
                    }
                }

                /* Its top, from where the ray came in to where it goes out. */
                int vx_next = tvx < tvy;
                int32_t tv_out = vx_next ? tvx : tvy;
                if (tv_out > t_out) tv_out = t_out;
                {
                    uint32_t s = INV(tv_out);
                    int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> (16 - YS));
                    if (top < YEND) top = YEND;
                    if (top < ybuf) {
                        uint16_t w = col->h ? vox_wtab[col->c[col->h - 1] + st]
                                            : (h ? vox_wtab[plinth + st] : BLACK);
                        if (HIDDEN(top)) PASS(top);
                        else for (int n = ybuf - top; n > 0; n--) {
                            PIXEL(w);
                        }
                        ybuf = top;
                        if (ybuf == YEND) return;
                    }
                }
                prev_h = h;
                if (tv_out >= t_out) break;

                if (vx_next) { ix += sx; tvx += tdx; }
                else         { iy += sy; tvy += tdy; }
                if (ix >> 3 != cx || iy >> 3 != cy) break;
                x_side = vx_next;
                tv_in = tv_out;
            }
        }
        if (t_out >= T_FAR) break;

        if (x_next) { cx += sx; tcx += tdcx; }
        else        { cy += sy; tcy += tdcy; }
        x_side = x_next;
        t_in = t_out;
    }
    while (ybuf-- > YEND) {
        PIXEL(BLACK);
    }
}


#define RAY(name, wide, ys, twice, clip)                                           \
    __attribute__((section(".ramtext"), noinline))                                 \
    static void name(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy, \
                     int32_t tdx, int32_t tdy) {                                   \
        ray(dst, ex, ey, dx, dy, tdx, tdy, wide, ys, twice, clip);                 \
    }
RAY(ray_coarse, 1, 0, 0, CLIP_NONE)
RAY(ray_wide, 0, 0, 0, CLIP_NONE)
RAY(ray_fine, 0, 1, 0, CLIP_NONE)
RAY(ray_around, 1, 0, 1, CLIP_NONE)        /* coarse, beside the focus window */
RAY(ray_hole, 1, 0, 1, CLIP_HOLE)          /* coarse, above and below it */
RAY(ray_focus, 0, 1, 0, CLIP_FOCUS)        /* fine, in it */

#define SECONDARY_GO  1

typedef void ray_fn(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy,
                    int32_t tdx, int32_t tdy);

/* Send the ray for column col of `cols` across the screen. */
static void cast(ray_fn *fn, uint8_t *dst, int col, int cols) {
    sh2z_fx_t s = job.s, c = job.c;
    sh2z_fx_t k = ((2 * col - (cols - 1)) << 16) / (2 * job.focal * (cols / VIEW_W));
    sh2z_fx_t dx = c - sh2z_mul(s, k);
    sh2z_fx_t dy = s + sh2z_mul(c, k);

    /* Depth between voxel lines on each axis. A ray that runs along an
     * axis, or nearly, never meets the other axis's lines on screen. */
    sh2z_fx_t tdx = dx ? sh2z_recip(sh2z_abs(dx)) : NEVER;
    sh2z_fx_t tdy = dy ? sh2z_recip(sh2z_abs(dy)) : NEVER;
    if (tdx > 0x1000000) tdx = NEVER;
    if (tdy > 0x1000000) tdy = NEVER;

    fn(dst, job.ex, job.ey, dx, dy, tdx, tdy);
}

/* This CPU's share of the frame: every other column, starting at `first`. */
static void columns(int first) {
    uint8_t *pixels = (uint8_t *)(job.fb + FB_PIXELS);

    switch (job.res) {
    case PERSP_RES_COARSE:
        for (int col = first; col < VIEW_W; col += 2) {
            cast(ray_coarse, pixels + (VIEW_H - 1) * STRIDE + col * 2, col, VIEW_W);
        }
        break;
    case PERSP_RES_WIDE:
        for (int col = first; col < 2 * VIEW_W; col += 2) {
            cast(ray_wide, pixels + (VIEW_H - 1) * STRIDE + col, col, 2 * VIEW_W);
        }
        break;
    case PERSP_RES_FINE:
        for (int col = first; col < 2 * VIEW_W; col += 2) {
            cast(ray_fine, pixels + (2 * VIEW_H - 1) * STRIDE + col, col, 2 * VIEW_W);
        }
        break;
    default:
        /* Coarse columns, each two framebuffer rows to a row. One that
         * crosses the focus window leaves a hole for its two fine columns. */
        for (int col = first; col < VIEW_W; col += 2) {
            uint8_t *at = pixels + (2 * VIEW_H - 2) * STRIDE + col * 2;
            if (col < FOCUS_X0 / 2 || col >= FOCUS_X1 / 2) {
                cast(ray_around, at, col, VIEW_W);
                continue;
            }
            cast(ray_hole, at, col, VIEW_W);
            for (int fine = 2 * col; fine < 2 * col + 2; fine++) {
                cast(ray_focus, pixels + (FOCUS_START - 1) * STRIDE + fine, fine, 2 * VIEW_W);
            }
        }
        break;
    }
}

/* Each SH-2 has a cache of its own, and this one has not seen what the
 * primary wrote to the world since the last frame. Forget everything. */
void persp_secondary(void) {
    while (MARS_SYS_COMM6 != SECONDARY_GO);
    CacheControl(SH2_CCTL_CP | SH2_CCTL_CE);
    columns(1);
    MARS_SYS_COMM6 = 0;
}

/* Loose voxels are too few and too brief to give the rays something to
 * hit. Each is put where the camera sees it, over the finished picture; one
 * that should be behind a wall shows through it for the moment it lasts. */
static void pieces(void) {
    int px, py, pz;
    unsigned colour;

    for (int i = 0; vox_piece(i, &px, &py, &pz, &colour); i++) {
        sh2z_fx_t rx = (px << 12) - job.ex, ry = (py << 12) - job.ey;
        sh2z_fx_t t = sh2z_mul(rx, job.c) + sh2z_mul(ry, job.s);
        sh2z_fx_t u = sh2z_mul(ry, job.c) - sh2z_mul(rx, job.s);
        if (t < job.t_near || t > job.t_far) continue;

        int32_t scale = (int32_t)(inv_t[(t - job.t_near) >> INV_STEP] >> 4);   /* focal / depth, Q12 */
        int fine_x = job.cols > VIEW_W, fine_y = job.rows > VIEW_H;
        int col = job.cols / 2 + (((u >> 8) * scale) >> (20 - fine_x));
        int row = (job.horizon << fine_y) + (((job.eye_h * 16 - pz) * scale) >> (16 - fine_y));
        if ((unsigned)col >= (unsigned)job.cols || (unsigned)row >= (unsigned)job.rows) continue;
        /* Outside the focus window's rows only even framebuffer rows show. */
        if (job.res == PERSP_RES_FOCUS && (row < FOCUS_Y0 || row >= FOCUS_Y1)) row &= ~1;

        /* A voxel's worth of pixels, whatever a pixel is. */
        uint8_t *at = (uint8_t *)(job.fb + FB_PIXELS) + row * STRIDE + (fine_x ? col & ~1 : col * 2);
        *(pair_t *)at = vox_wtab[colour];
        if (fine_y && row + 1 < job.rows) *(pair_t *)(at + STRIDE) = vox_wtab[colour];
    }
}

/* Set the lens: flat is 0 for full perspective to 256 for none, pull 0
 * close in to 256 pulled back. The eye is always as high as it is far
 * back, which keeps the hero on the same row and the ground as deep on
 * screen as it is wide. The tables follow, and are only worked out again
 * when the lens changes. */
static void lens(int flat, int pull, int res) {
    static int had_flat = -1, had_pull = -1, had_res = -1;
    if (flat == had_flat && pull == had_pull && res == had_res) return;
    had_flat = flat;
    had_pull = pull;
    had_res = res;

    /* The picture's size in framebuffer pixels and rows. */
    job.res = res;
    job.cols = res == PERSP_RES_COARSE ? VIEW_W : 2 * VIEW_W;
    job.rows = (res == PERSP_RES_FINE || res == PERSP_RES_FOCUS) ? 2 * VIEW_H : VIEW_H;

    int i = flat >> 4, f = flat & 15;
    int32_t focal = focal_step[i] + (i < 16 ? (focal_step[i + 1] - focal_step[i]) * f / 16 : 0);
    int32_t eye = focal + ((focal * PULL_BACK * pull) >> 16);
    int32_t horizon = TARGET_ROW - focal;      /* in coarse rows; a fine ray doubles it */

    /* The ground reaches the bottom row at t_near and the top row at t_far. */
    job.focal = focal;
    job.horizon = horizon;
    job.eye_h = eye;
    job.t_near = (int32_t)((((uint64_t)eye * focal) << 16) / (uint32_t)(VIEW_H - horizon));
    job.t_far = (int32_t)((((uint64_t)eye * focal) << 16) / (uint32_t)(0 - horizon));
    job.t_focus = (int32_t)((((uint64_t)eye * focal) << 17) / (uint32_t)(FOCUS_START - 2 * horizon));
    if (job.t_far - job.t_near > (int32_t)((INV_N - 2) << INV_STEP)) {
        job.t_far = job.t_near + (int32_t)((INV_N - 2) << INV_STEP);   /* as far as the tables go */
    }

    for (int n = 0; n < INV_N; n++) {
        uint32_t t = (uint32_t)job.t_near + ((uint32_t)n << INV_STEP);
        inv_t[n] = ((uint32_t)focal << 19) / (t >> 13);
        rise_t[n] = t / (uint32_t)focal;
    }
    /* By scanline; a coarse ray reads every other entry. */
    for (int y = 0; y < 2 * VIEW_H; y++) {
        row_t[y] = (uint32_t)((((uint64_t)focal) << 29) / (uint32_t)(y - 2 * horizon));
    }
}

void persp_render(uint16_t *fb, int x, int y, uint16_t yaw, int pull, int flat, int res) {
    sh2z_sincos(yaw, &job.s, &job.c);
    lens(flat, pull, res);

    /* Forward is (c, s) on the ground, right is (-s, c). The eye is as far
     * back as it is high. */
    job.ex = (x << 16) - job.c * job.eye_h;
    job.ey = (y << 16) - job.s * job.eye_h;
    job.fb = fb;

    MARS_SYS_COMM6 = SECONDARY_GO;
    columns(0);
    while (MARS_SYS_COMM6 == SECONDARY_GO);
    pieces();

    if (job.res == PERSP_RES_FOCUS) {
        /* A framebuffer row to a scanline where the focus window is, and
         * one to two scanlines above and below it. */
        for (int k = 0; k < 2 * VIEW_H; k++) {
            int row = (k >= FOCUS_Y0 && k < FOCUS_Y1) ? k : k & ~1;
            fb[k] = (uint16_t)(FB_PIXELS + row * (STRIDE / 2));
        }
    } else if (job.rows > VIEW_H) {
        for (int k = 0; k < job.rows; k++) fb[k] = (uint16_t)(FB_PIXELS + k * (STRIDE / 2));
    } else {
        for (int k = 0; k < VIEW_H; k++) {
            fb[2 * k] = fb[2 * k + 1] = (uint16_t)(FB_PIXELS + k * (STRIDE / 2));
        }
    }
}
#endif
