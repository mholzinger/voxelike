/* Only PERSP builds have anything in this file. */
typedef int persp_unit;

#ifdef PERSP
#include "mars.h"
#include "string.h"
#include "voxel.h"
#include "voxel_world.h"
#include "persp.h"
#include "sh2zam/sh2zam.h"

/* A voxel-space renderer. The world is solid columns on a grid, so each
 * screen column is one ray across the ground: step from grid line to grid
 * line, and every column of voxels the ray enters adds its near side and its
 * top to the picture, nearest first, each drawn above what is already there.
 *
 * The camera looks level and the picture is slid down (a horizon far above
 * the screen), which is what makes it look down at the hero while vertical
 * things stay vertical. With the eye as high as it is far and the focal
 * length equal to both, a voxel at the hero is one fat pixel wide, tall and
 * deep, the same scale as the fixed view. */
#define FOCAL     128
#define EYE_H     128
#define EYE_BACK  128
#define HORIZON   (VIEW_H / 2 - EYE_H * FOCAL / EYE_BACK)

/* Depths are Q16 along the view direction. The ground reaches the bottom
 * row at T_NEAR and the top row at T_FAR; nothing else is on screen. */
#define T_NEAR    ((EYE_H * FOCAL << 8) / (VIEW_H - HORIZON) << 8)
#define T_FAR     ((EYE_H * FOCAL << 8) / (0 - HORIZON) << 8)
#define INV_STEP  14                       /* table entry per 1/4 voxel */
#define INV_N     1024

typedef char far_fits[(T_FAR >> INV_STEP) < INV_N ? 1 : -1];

#define FB_PIXELS 0x100                    /* after the line table */
#define MAX_BUMPS 16

#define NEVER     (INT32_MAX / 2)
#define DETAIL    0xFF

static uint32_t inv_t[INV_N];              /* FOCAL / depth, Q16 */
/* Depth, in Q12, at which a level surface h voxels up shows on each row. */
static uint32_t plane_t[COL_MAXH + 1][VIEW_H];

/* For an empty cell, how many more empty cells lie beyond it whichever way
 * a ray leaves: the ray may cross that many without looking. */
static uint8_t clear_run[MAP_H][MAP_W];
#define RUN_MAX 15
static const vmodel_t *grid[MAP_H][MAP_W];
static uint8_t flat[MAP_H][MAP_W];         /* a cell's one height, plinth and all, or DETAIL */
static vmodel_t bump[MAX_BUMPS];           /* cells with a creature standing in them */

void persp_init(void) {
    for (int i = 1; i < INV_N; i++) inv_t[i] = ((uint32_t)FOCAL << (16 + 16 - INV_STEP)) / (uint32_t)i;
    inv_t[0] = inv_t[1];
    for (int h = 0; h <= COL_MAXH; h++) {
        for (int y = 0; y < VIEW_H; y++) {
            plane_t[h][y] = (uint32_t)((EYE_H - h) * FOCAL * 4096 / (y - HORIZON));
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

/* Until creatures are real 3D models, one is a block of its main colour
 * standing in the middle of its cell. */
void persp_sync(void) {
    int n = 0;

    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            unsigned h = flatness(&vox_models[vox_tile[cy][cx]]);
            grid[cy][cx] = &vox_models[vox_tile[cy][cx]];
            flat[cy][cx] = (uint8_t)(h == DETAIL ? DETAIL : h + vox_lift[cy][cx]);
        }
    }
    for (int i = 0; n < MAX_BUMPS; i++) {
        int cx, cy, kind;
        if (!vox_creature(i, &cx, &cy, &kind)) break;

        vmodel_t *m = &bump[n++];
        *m = *grid[cy][cx];
        for (int lx = 2; lx < 6; lx++) {
            for (int ly = 2; ly < 6; ly++) {
                vcol_t *c = &m->col[lx][ly];
                for (int z = 0; z < 10 && c->h < COL_MAXH; z++) {
                    c->c[c->h++] = vox_creature_colour(kind, z);
                }
            }
        }
        grid[cy][cx] = m;
        flat[cy][cx] = DETAIL;
    }

    /* Distance, in cells and counting diagonals as one, from each empty
     * cell to the nearest that is not: two sweeps. Off the map counts as
     * not empty, so a run never leaves it. */
    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            unsigned d = 0;
            if (flat[cy][cx] == 0 && cx > 0 && cy > 0 && cx < MAP_W - 1) {
                d = clear_run[cy][cx - 1];
                if (clear_run[cy - 1][cx - 1] < d) d = clear_run[cy - 1][cx - 1];
                if (clear_run[cy - 1][cx] < d) d = clear_run[cy - 1][cx];
                if (clear_run[cy - 1][cx + 1] < d) d = clear_run[cy - 1][cx + 1];
                if (d < RUN_MAX) d++;
            }
            clear_run[cy][cx] = (uint8_t)d;
        }
    }
    for (int cy = MAP_H - 1; cy >= 0; cy--) {
        for (int cx = MAP_W - 1; cx >= 0; cx--) {
            unsigned d = clear_run[cy][cx];
            if (d == 0) continue;
            if (cy == MAP_H - 1 || cx == MAP_W - 1) { clear_run[cy][cx] = 1; continue; }
            if (clear_run[cy][cx + 1] + 1u < d) d = clear_run[cy][cx + 1] + 1;
            if (clear_run[cy + 1][cx + 1] + 1u < d) d = clear_run[cy + 1][cx + 1] + 1;
            if (clear_run[cy + 1][cx] + 1u < d) d = clear_run[cy + 1][cx] + 1;
            if (clear_run[cy + 1][cx - 1] + 1u < d) d = clear_run[cy + 1][cx - 1] + 1;
            clear_run[cy][cx] = (uint8_t)d;
        }
    }
}

/* One screen column. The ray crosses the map a cell at a time. A cell that
 * is the same height all over (nothing, floor, wall) is settled in one go:
 * the side the ray enters by, then the top, each row worked back to the
 * voxel it shows. Any other cell is walked voxel column by voxel column,
 * each adding its near side and its top. Everything is drawn nearest first
 * and bottom up. */
__attribute__((section(".ramtext"), noinline))
static void ray(uint16_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy,
                int32_t tdx, int32_t tdy) {
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int32_t px = ex + (int32_t)(((int64_t)dx * T_NEAR) >> 16);
    int32_t py = ey + (int32_t)(((int64_t)dy * T_NEAR) >> 16);
    int cx = px >> 19, cy = py >> 19;

    /* Depth at the next cell line on each axis, and between cell lines. */
    int32_t fx = px & 0x7FFFF, fy = py & 0x7FFFF;
    int32_t tcx = tdx >= NEVER ? NEVER
        : T_NEAR + (int32_t)(((int64_t)(dx < 0 ? fx : 0x80000 - fx) * tdx) >> 16);
    int32_t tcy = tdy >= NEVER ? NEVER
        : T_NEAR + (int32_t)(((int64_t)(dy < 0 ? fy : 0x80000 - fy) * tdy) >> 16);
    int32_t tdcx = tdx >= NEVER / 8 ? NEVER : tdx * 8;
    int32_t tdcy = tdy >= NEVER / 8 ? NEVER : tdy * 8;

    /* Where the ray is at a depth: the eye plus direction times depth, as
     * Q20 times Q12 so the voxel number is simply the product's high word. */
    int32_t dx20 = dx << 4, dy20 = dy << 4;
    int64_t ex48 = (int64_t)ex << 16, ey48 = (int64_t)ey << 16;
#define AT_X(t12) ((int)(((int64_t)dx20 * (int32_t)(t12) + ex48) >> 32))
#define AT_Y(t12) ((int)(((int64_t)dy20 * (int32_t)(t12) + ey48) >> 32))

    int32_t t_in = T_NEAR;
    int ybuf = VIEW_H;                     /* rows at and below this are drawn */
    int prev_h = 0, x_side = 0;

    for (;;) {
        int x_next = tcx < tcy;
        int32_t t_out = x_next ? tcx : tcy;
        if (t_out > T_FAR) t_out = T_FAR;

        unsigned kind = 0, st = 0, plinth = 0;
        int lift = 0;                      /* the cell's model stands this high */
        const vmodel_t *m = 0;
        if ((unsigned)cx < MAP_W && (unsigned)cy < MAP_H) {
            kind = flat[cy][cx];
            if (kind == 0) {
                /* Nothing here, nor for some way: cross it all at once. */
                for (int n = clear_run[cy][cx] - 1; n > 0 && t_out < T_FAR; n--) {
                    if (x_next) { cx += sx; tcx += tdcx; }
                    else        { cy += sy; tcy += tdcy; }
                    x_next = tcx < tcy;
                    t_out = x_next ? tcx : tcy;
                }
                if (t_out > T_FAR) t_out = T_FAR;
            } else {
                m = grid[cy][cx];
                st = vox_light[cy][cx];
                lift = vox_lift[cy][cx];
                plinth = vox_plinth[vox_tile[cy][cx]];
            }
        }

        if (kind == 0) {
            uint32_t s = inv_t[t_out >> INV_STEP];
            int top = HORIZON + (int)(((uint32_t)EYE_H * s) >> 16);
            if (top < 0) top = 0;
            if (top < ybuf) {
                for (int n = ybuf - top; n > 0; n--) {
                    *dst = 0;
                    dst -= VIEW_W;
                }
                ybuf = top;
                if (ybuf == 0) return;
            }
            prev_h = 0;
        } else if (kind != DETAIL) {
            int h = (int)kind;

            /* The side the ray comes in by: one voxel column of it. */
            if (h > prev_h) {
                uint32_t s = inv_t[t_in >> INV_STEP];
                int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> 16);
                if (top < 0) top = 0;
                if (top < ybuf) {
                    int32_t tq = (t_in >> 4) + 1;
                    const uint8_t *c = m->col[AT_X(tq) & 7][AT_Y(tq) & 7].c;
                    unsigned sf = vox_front_shade[st];
                    if (x_side && sf) sf--;
                    int32_t zstep = (int32_t)((uint32_t)t_in / FOCAL);   /* voxels per row, Q16 */
                    int32_t z = (EYE_H << 16) - (ybuf - 1 - HORIZON) * zstep;
                    for (int n = ybuf - top; n > 0; n--) {
                        int zi = z >> 16;
                        if (zi < 0) zi = 0;
                        if (zi >= h) zi = h - 1;
                        *dst = vox_wtab[(zi < lift ? plinth : c[zi - lift]) + sf];
                        dst -= VIEW_W;
                        z += zstep;
                    }
                    ybuf = top;
                    if (ybuf == 0) return;
                }
            }

            /* The top, each row worked back to the voxel under it. */
            uint32_t s = inv_t[t_out >> INV_STEP];
            int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> 16);
            if (top < 0) top = 0;
            if (top < ybuf) {
                const uint32_t *pt = &plane_t[h][ybuf - 1];
                int layer = h - lift - 1;
                for (int n = ybuf - top; n > 0; n--) {
                    int32_t t = (int32_t)*pt--;
                    *dst = vox_wtab[m->col[AT_X(t) & 7][AT_Y(t) & 7].c[layer] + st];
                    dst -= VIEW_W;
                }
                ybuf = top;
                if (ybuf == 0) return;
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
                    uint32_t s = inv_t[tv_in >> INV_STEP];
                    int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> 16);
                    if (top < 0) top = 0;
                    if (top < ybuf) {
                        unsigned sf = (x_side && sf0) ? sf0 - 1 : sf0;
                        int32_t zstep = (int32_t)((uint32_t)tv_in / FOCAL);   /* voxels per row, Q16 */
                        int32_t z = (EYE_H << 16) - (ybuf - 1 - HORIZON) * zstep;
                        for (int n = ybuf - top; n > 0; n--) {
                            int zi = z >> 16;
                            if (zi < 0) zi = 0;
                            if (zi >= h) zi = h - 1;
                            *dst = vox_wtab[(zi < lift ? plinth : col->c[zi - lift]) + sf];
                            dst -= VIEW_W;
                            z += zstep;
                        }
                        ybuf = top;
                        if (ybuf == 0) return;
                    }
                }

                /* Its top, from where the ray came in to where it goes out. */
                int vx_next = tvx < tvy;
                int32_t tv_out = vx_next ? tvx : tvy;
                if (tv_out > t_out) tv_out = t_out;
                {
                    uint32_t s = inv_t[tv_out >> INV_STEP];
                    int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> 16);
                    if (top < 0) top = 0;
                    if (top < ybuf) {
                        uint16_t w = col->h ? vox_wtab[col->c[col->h - 1] + st]
                                            : (h ? vox_wtab[plinth + st] : 0);
                        for (int n = ybuf - top; n > 0; n--) {
                            *dst = w;
                            dst -= VIEW_W;
                        }
                        ybuf = top;
                        if (ybuf == 0) return;
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
    while (ybuf-- > 0) {
        *dst = 0;
        dst -= VIEW_W;
    }
}

/* One frame's camera, shared by both CPUs. The primary fills it in and
 * raises COMM6; the secondary draws the odd columns, the primary the even
 * ones, and the secondary lowers COMM6 when its share is done. */
static struct {
    uint16_t *fb;
    int32_t ex, ey;
    sh2z_fx_t s, c;
} job;

#define SECONDARY_GO  1

static void columns(int first) {
    uint16_t *fb = job.fb;
    int32_t ex = job.ex, ey = job.ey;
    sh2z_fx_t s = job.s, c = job.c;

    for (int col = first; col < VIEW_W; col += 2) {
        sh2z_fx_t k = ((2 * col - (VIEW_W - 1)) << 16) / (2 * FOCAL);
        sh2z_fx_t dx = c - sh2z_mul(s, k);
        sh2z_fx_t dy = s + sh2z_mul(c, k);

        /* Depth between voxel lines on each axis. A ray that runs along an
         * axis, or nearly, never meets the other axis's lines on screen. */
        sh2z_fx_t tdx = dx ? sh2z_recip(sh2z_abs(dx)) : NEVER;
        sh2z_fx_t tdy = dy ? sh2z_recip(sh2z_abs(dy)) : NEVER;
        if (tdx > 0x1000000) tdx = NEVER;
        if (tdy > 0x1000000) tdy = NEVER;

        ray(fb + FB_PIXELS + (VIEW_H - 1) * VIEW_W + col, ex, ey, dx, dy, tdx, tdy);
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

void persp_render(uint16_t *fb, int x, int y, uint16_t yaw) {
    sh2z_sincos(yaw, &job.s, &job.c);

    /* Forward is (c, s) on the ground, right is (-s, c). */
    job.ex = (x << 16) - job.c * EYE_BACK;
    job.ey = (y << 16) - job.s * EYE_BACK;
    job.fb = fb;

    MARS_SYS_COMM6 = SECONDARY_GO;
    columns(0);
    while (MARS_SYS_COMM6 == SECONDARY_GO);

    for (int k = 0; k < VIEW_H; k++) {
        fb[2 * k] = fb[2 * k + 1] = (uint16_t)(FB_PIXELS + k * VIEW_W);
    }
}
#endif
