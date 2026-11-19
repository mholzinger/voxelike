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
/* Depth, in Q30, at which a level surface one voxel below the eye shows on
 * each row. One h voxels below shows at h times that. It is never as much
 * as 2: the eye is at least as high as the focal length is long. */
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
    int res;               /* PERSP_RES_x: how finely the picture is drawn */
    int cols, rows;
} job;

/* The framebuffer is 320 bytes a row whatever the resolution. A coarse
 * column is two pixels wide, written as one word; a coarse row is shown on
 * two scanlines by the line table. */
#define STRIDE    320
typedef uint16_t __attribute__((may_alias)) pair_t;
typedef uint32_t __attribute__((may_alias)) quad_t;

/* The 32X does not store a zero written to its framebuffer as a byte, so
 * that sprites can be drawn with holes in them. Nothing is CRAM index 1,
 * which the palette keeps as black as index 0. */
#define BLACK     0x0101

#define RUN_MAX   15

/* The colour on top of each column of a model, 64 bytes where the model
 * itself spreads them over a kilobyte. */
static uint8_t tops[VOX_MODELS_MAX][CELL * CELL];
static uint8_t stained_tops[VOX_MODELS_MAX][CELL * CELL];

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
    uint8_t stain;
    uint8_t pad[3];
} pcell_t;

typedef char pcell_is_a_line[sizeof(pcell_t) == 16 ? 1 : -1];
static pcell_t cells[MAP_H][MAP_W];

/* A cell with a creature in it gets a model of its own: the terrain's, with
 * the creature's voxel columns standing on it. */
static vmodel_t bump[MAX_BUMPS];
static struct { uint8_t cx, cy; } bumped[MAX_BUMPS];
static int n_bumped;
static int arrival_altitude;
static uint8_t model_flat[VOX_MODELS_MAX];

static unsigned flatness(const vmodel_t *m);

void persp_init(void) {
    for (int m = 0; m < vox_model_count; m++) {
        model_flat[m] = (uint8_t)flatness(&vox_models[m]);
        for (int i = 0; i < CELL * CELL; i++) {
            const vcol_t *c = &vox_models[m].col[i >> 3][i & 7];
            tops[m][i] = c->h ? c->c[c->h - 1] : 0;
            stained_tops[m][i] = c->h == 1 && vox_stain_pixel(i >> 3, i & 7)
                               ? vox_blood_material : tops[m][i];
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

    const vmodel_t *partial = vox_assembly_model(cx, cy);
    pc->m = partial ? partial : &vox_models[tile];
    pc->stain = (uint8_t)vox_stained(cx, cy);
    pc->top = pc->stain ? stained_tops[tile] : tops[tile];
    pc->flat = (uint8_t)(partial || h == DETAIL ? DETAIL : h + vox_lift[cy][cx]);
    pc->lift = vox_lift[cy][cx];
    pc->plinth = vox_plinth[tile];
    if (pc->flat) pc->light = vox_light[cy][cx];
}

static unsigned run_at(int cx, int cy) {
    return cells[cy][cx].flat ? 0 : cells[cy][cx].light;
}

/* Refresh the empty runs only when a cell changes between empty and
 * occupied. Light, height, items and creatures leave those runs valid. */
void persp_sync(int terrain_changed) {
    static int runs_ready;
    int rebuild_runs = !runs_ready;
    if (terrain_changed || rebuild_runs) {
        n_bumped = 0;
        for (int cy = 0; cy < MAP_H; cy++) {
            for (int cx = 0; cx < MAP_W; cx++) {
                pcell_t *pc = &cells[cy][cx];
                unsigned tile = vox_tile[cy][cx];
                unsigned lift = vox_lift[cy][cx];
                unsigned light = vox_light[cy][cx];
                if (pc->m == &vox_models[tile] && pc->lift == lift &&
                    pc->stain == vox_stained(cx, cy) && (!pc->flat || pc->light == light)) continue;
                int was_empty = pc->flat == 0;
                terrain(cx, cy);
                if (was_empty != (pc->flat == 0)) rebuild_runs = 1;
            }
        }
    }
    if (!rebuild_runs) {
        persp_creatures(0);
        return;
    }
    runs_ready = 1;

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
    persp_creatures(0);
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
    *m = *cells[cy][cx].m;
    cells[cy][cx].m = m;
    cells[cy][cx].flat = DETAIL;
    return m;
}

/* The hero as he is dressed and armed: the body his armour gives him, and
 * his weapon added wherever it stands clear of that. By Rogue's numbers. */
static const vcol_t (*hero_dressed(unsigned attack))[CELL] {
    static const uint8_t body_of[8] = {
        GEAR_BODY_LEATHER, GEAR_BODY_RING, GEAR_BODY_STUDDED, GEAR_BODY_SCALE,
        GEAR_BODY_CHAIN, GEAR_BODY_SPLINT, GEAR_BODY_BANDED, GEAR_BODY_PLATE,
    };
    static const uint8_t held_of[9] = {
        GEAR_MACE, GEAR_SWORD, GEAR_BOW, GEAR_ARROW, GEAR_DAGGER,
        GEAR_TWO_HANDED_SWORD, GEAR_DART, GEAR_SHURIKEN, GEAR_SPEAR,
    };
    static vcol_t made[CELL][CELL];
    static int armor = -2, weapon = -2;
    static const vox_attack_pose_t *last_pose;
    const vox_attack_pose_t *pose = vox_attack_pose(vox_hero_weapon, attack);

    if (armor == vox_hero_armor && weapon == vox_hero_weapon && pose == last_pose)
        return (const vcol_t (*)[CELL])made;
    armor = vox_hero_armor;
    weapon = vox_hero_weapon;
    last_pose = pose;
    const vcol_t (*body)[CELL] = (unsigned)armor < 8 ? gear_solid[body_of[armor]] : ent_solid[0];
    const vcol_t (*held)[CELL] = (unsigned)weapon < 9 ? gear_solid[held_of[weapon]] : 0;
    memcpy(made, body, sizeof made);
    if (held) {
        for (int x = 0; x < CELL; x++) {
            for (int y = 0; y < CELL; y++) {
                const vcol_t *weapon_col = &held[x][y];
                for (int z = 0; z < weapon_col->h; z++) {
                    int px = x, py = y, pz = z;
                    unsigned material = weapon_col->c[z];
                    if (pose) {
                        if (material == vox_shadow_material) continue;
                        int shaft = z - 5;
                        int tilt = pose->tilt < 0 ? -pose->tilt : pose->tilt;
                        px += pose->across;
                        py += pose->forward + shaft * pose->tilt / 4;
                        pz += pose->lift - shaft * tilt / 6;
                        if (px < 0 || px >= CELL || pz < 0 || pz >= COL_MAXH) continue;
                        py = py < 0 ? 0 : py >= CELL ? CELL - 1 : py;
                    }
                    if (pz < body[px][py].h) continue;
                    vcol_t *dst = &made[px][py];
                    while (dst->h <= pz) dst->c[dst->h++] = vox_shadow_material;
                    dst->c[pz] = (uint8_t)material;
                }
            }
        }
    }
    return (const vcol_t (*)[CELL])made;
}

void persp_hero_shatter(void) {
    vox_hero_shatter(hero_dressed(0));
}

int persp_hero_height(void) {
    const vcol_t (*model)[CELL] = hero_dressed(0);
    int height = 0;
    for (int x = 0; x < CELL; x++)
        for (int y = 0; y < CELL; y++)
            if (model[x][y].h > height) height = model[x][y].h;
    return height;
}

/* Stand every creature where it is this frame. A creature is 8x8 columns
 * of voxels, turned to face the way it last moved; walking between cells
 * it stands partly on each. */
static void place_creatures(int hero_buried, int stair, int offset, int hop) {
    vox_creature_t c;

    while (n_bumped > 0) {
        n_bumped--;
        terrain(bumped[n_bumped].cx, bumped[n_bumped].cy);
    }
    for (int i = 0, cx, cy; vox_assembly_cell(i, &cx, &cy); i++)
        terrain(cx, cy);
    for (int i = 0; vox_creature(i, &c); i++) {
        if (stair && !c.kind) {
            c.x += offset;
            c.z = hop;
            c.face = 3;   /* the stair treads descend along positive X */
        }
        const vcol_t (*model)[CELL] = c.kind ? ent_solid[c.kind] : hero_dressed(c.attack);
        for (int fx = 0; fx < CELL; fx++) {
            for (int fy = 0; fy < CELL; fy++) {
                const vcol_t *src = vox_model_column(model, c.face, fx, fy);
                int buried = c.kind ? 0 : hero_buried;
                if (src->h <= buried) continue;

                int wx = c.x + fx, wy = c.y + fy;
                vmodel_t *m = bump_for(wx >> 3, wy >> 3);
                if (!m) continue;
                vcol_t *dst = &m->col[wx & 7][wy & 7];
                /* A hop lifts it; what is under its feet reads as shadow. */
                for (int z = 0; z < c.z && dst->h < COL_MAXH; z++) {
                    dst->c[dst->h++] = src->c[0];
                }
                for (int z = buried; z < src->h && dst->h < COL_MAXH; z++) {
                    dst->c[dst->h++] = c.flash ? (uint8_t)(c.flash & ~7) : src->c[z];
                }
            }
        }
    }
}

void persp_creatures(int hero_buried) {
    place_creatures(hero_buried, 0, 0, 0);
}

void persp_stair_hop(int hero_buried, int offset, int hop) {
    place_creatures(hero_buried, 1, offset, hop);
}

void persp_arrival_hero(int altitude) {
    arrival_altitude = altitude > 0 ? altitude : 0;
    persp_creatures(altitude ? persp_hero_height() : 0);
}

void persp_death_pool(int cx, int cy, unsigned age) {
    vmodel_t *model = bump_for(cx, cy);
    if (!model) return;
    const vmodel_t *floor = &vox_models[vox_tile[cy][cx]];
    unsigned height = floor->col[CELL / 2][CELL / 2].h;
    if (!height || height > 3) return;

    /* Grow in voxel-sized steps, with a slightly uneven edge. Recolour
     * the floor itself so bodies, debris and walls keep their depth. */
    unsigned spread = 1 + age / 4;
    if (spread > 9) spread = 9;
    for (int x = 1; x < CELL - 1; x++) {
        for (int y = 1; y < CELL - 1; y++) {
            int dx = x - CELL / 2, dy = y - CELL / 2;
            unsigned distance = (unsigned)(dx * dx + dy * dy + ((x + 2 * y) & 1));
            if (distance <= spread && floor->col[x][y].h == height)
                model->col[x][y].c[height - 1] = vox_blood_material;
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
/* WIDE writes two screen pixels. YS selects one row per scanline. */

/* Draw the rows from ybuf up to `top`, which is above it, and leave ybuf
 * there. The statements work one pixel out into `p`. */
#define ROWS(top, ...) do {                                                  \
        int rows = ybuf - (top);                                             \
        ybuf = (top);                                                        \
        do {                                                                 \
            uint16_t p;                                                      \
            __VA_ARGS__;                                                     \
            if (WIDE) *(pair_t *)dst = p;                                    \
            else *dst = (uint8_t)p;                                          \
            dst -= STRIDE;                              \
        } while (--rows);                                                    \
    } while (0)

/* The tables, by depth. */
/* The shift is of an unsigned number because the SH-2 shifts one place at
 * a time, and the compiler calls a routine in ROM to shift a signed one. */
#define INV(t)  inv_t[(uint32_t)((t) - T_NEAR) >> INV_STEP]
#define RISE(t) rise_t[(uint32_t)((t) - T_NEAR) >> INV_STEP]
/* Where the ray is at a depth: the eye plus direction times depth, as
 * Q20 times Q12 so the voxel number is simply the product's high word. */
#define AT_X(t12) ((int)(((int64_t)dx20 * (int32_t)(t12) + ex48) >> 32))
#define AT_Y(t12) ((int)(((int64_t)dy20 * (int32_t)(t12) + ey48) >> 32))

/* What a ray hands over when it comes to a cell that is not one height all
 * over, and gets back when the cell has been drawn. */
typedef struct {
    uint8_t *dst;                          /* in and out: how far the column is drawn */
    int ybuf, prev_h, x_side;
    int cx, cy;                            /* the cell, and the ray's depths at it */
    int32_t t_in, t_out, tcx, tcy;
    int32_t dx, dy, tdx, tdy, ex, ey;      /* the ray */
} walk_t;

typedef int walk_fn(walk_t *w);

/* Walk the voxel columns of one cell, each adding its near side and its
 * top. Returns 1 when that has filled the screen column.
 *
 * This is apart from ray() for the cache's sake. The SH-2's is 4K, and a
 * ray needs most of that for the map and the tables it reads; the walk is
 * as much code again as the rest of a ray and is wanted for a cell in
 * dozens (a creature, a door, the stairs). Apart, it is only in the cache
 * when it is in use. */
static inline __attribute__((always_inline))
int walk(walk_t *w, const int WIDE, const int YS) {
    const int32_t EYE_H = job.eye_h, T_NEAR = job.t_near;
    const int32_t HORIZON = job.horizon << YS;
    const int YEND = 0;
    const pcell_t *pc = &cells[w->cy][w->cx];
    const vmodel_t *m = pc->m;
    const unsigned st = pc->light, plinth = pc->plinth;
    const int lift = pc->lift;
    const int32_t dx = w->dx, dy = w->dy, tdx = w->tdx, tdy = w->tdy;
    const int cx = w->cx, cy = w->cy;
    const int32_t t_in = w->t_in, t_out = w->t_out;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int32_t dx20 = dx << 4, dy20 = dy << 4;
    int64_t ex48 = (int64_t)w->ex << 16, ey48 = (int64_t)w->ey << 16;
    uint8_t *dst = w->dst;
    int ybuf = w->ybuf, prev_h = w->prev_h, x_side = w->x_side;
    int done = 0;

    /* Where the ray is as it comes in, nudged inside so rounding cannot
     * put it next door. */
    int32_t tq = (t_in >> 4) + 1;
    int ix = AT_X(tq), iy = AT_Y(tq);
    if (ix >> 3 != cx) ix = cx * 8 + (ix >> 3 < cx ? 0 : 7);
    if (iy >> 3 != cy) iy = cy * 8 + (iy >> 3 < cy ? 0 : 7);

    /* Depth at this voxel's far line on each axis: the cell's own far
     * line, less the voxel lines still to cross before it. */
    int32_t tvx = tdx >= NEVER ? NEVER : w->tcx - tdx * (dx < 0 ? ix & 7 : 7 - (ix & 7));
    int32_t tvy = tdy >= NEVER ? NEVER : w->tcy - tdy * (dy < 0 ? iy & 7 : 7 - (iy & 7));
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
                ROWS(top,
                    int zi = z >> 16;
                    if (zi < 0) zi = 0;
                    if (zi >= h) zi = h - 1;
                    p = vox_wtab[(zi < lift ? plinth : col->c[zi - lift]) + sf];
                    z += zstep);
                if (ybuf == YEND) { done = 1; break; }
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
                int painted = pc->stain && col->h == 1 && vox_stain_pixel(ix & 7, iy & 7);
                uint16_t pix = col->h ? vox_wtab[(painted ? vox_blood_material : col->c[col->h - 1]) + st]
                                      : (h ? vox_wtab[plinth + st] : BLACK);
                ROWS(top, p = pix);
                if (ybuf == YEND) { done = 1; break; }
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
    w->dst = dst; w->ybuf = ybuf; w->prev_h = prev_h;
    return done;
}

static inline __attribute__((always_inline))
void ray(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy,
         int32_t tdx, int32_t tdy, const int WIDE, const int YS,
         walk_fn *const WALK) {
    walk_t w;
    w.dx = dx; w.dy = dy; w.tdx = tdx; w.tdy = tdy; w.ex = ex; w.ey = ey;
    const int32_t EYE_H = job.eye_h, T_NEAR = job.t_near, T_FAR = job.t_far;
    const int32_t HORIZON = job.horizon << YS;
    const int YEND = 0;
    const int start_row = VIEW_H << YS;
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

    int32_t dx20 = dx << 4, dy20 = dy << 4;
    int64_t ex48 = (int64_t)ex << 16, ey48 = (int64_t)ey << 16;

    int32_t t_in = T_NEAR;
    int ybuf = start_row;   /* rows from here down are drawn */
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
        } else if ((cx < 0 && sx < 0) || (cx >= MAP_W && sx > 0) ||
                   (cy < 0 && sy < 0) || (cy >= MAP_H && sy > 0)) {
            /* This axis is moving away from the map. The rest is sky. */
            break;
        }

        if (kind == 0) {
            uint32_t s = INV(t_out);
            int top = HORIZON + (int)(((uint32_t)EYE_H * s) >> (16 - YS));
            if (top < YEND) top = YEND;
            if (top < ybuf) {
                ROWS(top, p = BLACK);
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
                    ROWS(top,
                        int zi = z >> 16;
                        if (zi < 0) zi = 0;
                        if (zi >= h) zi = h - 1;
                        p = vox_wtab[(zi < lift ? plinth : c[zi - lift]) + sf];
                        z += zstep);
                    if (ybuf == YEND) return;
                }
            }

            /* The top, each row worked back to the voxel under it. */
            uint32_t s = INV(t_out);
            int top = HORIZON + (int)(((uint32_t)(EYE_H - h) * s) >> (16 - YS));
            if (top < YEND) top = YEND;
            if (top < ybuf) {
                /* The ray is over the voxel at eye + direction * depth, and
                 * the depth on a row is row_t's for it times how far below
                 * the eye this top is. Direction times that, as Q18, is the
                 * same for the whole run; times row_t's Q30 it is a Q48
                 * whose high word is how far from the eye in Q16, so a row
                 * costs two multiplies and nothing wider than a word. */
                const uint32_t *pt = &row_t[(ybuf - 1) << (1 - YS)];
                int32_t below = EYE_H - h;
                int32_t ax = (dx * below) << 2, ay = (dy * below) << 2;
                const uint16_t *lit = vox_wtab + st;
                ROWS(top,
                    int32_t r = (int32_t)*pt;
                    pt -= 2 >> YS;
                    int vx = (ex + (int32_t)(((int64_t)ax * r) >> 32)) >> 16;
                    int vy = (ey + (int32_t)(((int64_t)ay * r) >> 32)) >> 16;
                    p = lit[tex[(vx & 7) * CELL + (vy & 7)]]);
                if (ybuf == YEND) return;
            }
            prev_h = h;
        } else {
            /* Anything else is walked a voxel column at a time, by code
             * kept apart from this: see walk(). */
            w.dst = dst; w.ybuf = ybuf; w.prev_h = prev_h; w.x_side = x_side;
            w.cx = cx; w.cy = cy;
            w.t_in = t_in; w.t_out = t_out; w.tcx = tcx; w.tcy = tcy;
            if (WALK(&w)) return;
            dst = w.dst; ybuf = w.ybuf; prev_h = w.prev_h;
        }
        if (t_out >= T_FAR) break;

        if (x_next) { cx += sx; tcx += tdcx; }
        else        { cy += sy; tcy += tdcy; }
        x_side = x_next;
        t_in = t_out;
    }
    if (ybuf > YEND) ROWS(YEND, p = BLACK);
}


/* The walks go after all the rays, out of their way. */
#define RAY(name, wide, ys)                                                    \
    __attribute__((section(".ramtext.walk"), noinline))                        \
    static int name##_walk(walk_t *w) {                                        \
        return walk(w, wide, ys);                                              \
    }                                                                         \
    __attribute__((section(".ramtext"), noinline))                             \
    static void name(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy, \
                     int32_t tdx, int32_t tdy) {                               \
        ray(dst, ex, ey, dx, dy, tdx, tdy, wide, ys, name##_walk);              \
    }
RAY(ray_coarse, 1, 0)
RAY(ray_wide, 0, 0)
RAY(ray_fine, 0, 1)

#define SECONDARY_GO  1

typedef void ray_fn(uint8_t *dst, int32_t ex, int32_t ey, int32_t dx, int32_t dy,
                    int32_t tdx, int32_t tdy);

/* The SH-2's own divider, which each of the two has: 39 cycles, where the
 * compiler's routine is some hundreds. 1 / a for a Q16 a of 256 or more. */
#define DIVU_DVSR    (*(volatile uint32_t *)0xFFFFFF00)
#define DIVU_DVDNTH  (*(volatile uint32_t *)0xFFFFFF10)
#define DIVU_DVDNTL  (*(volatile uint32_t *)0xFFFFFF14)

static inline __attribute__((always_inline)) int32_t recip(int32_t a) {
    DIVU_DVSR = (uint32_t)a;
    DIVU_DVDNTH = 1;                       /* 2^32 over a */
    DIVU_DVDNTL = 0;
    return (int32_t)DIVU_DVDNTL;
}

/* How far to the side of straight ahead each column looks, as a slope:
 * lens() works these out, since only the lens changes them. */
static int32_t slope_t[2 * VIEW_W];

static void slopes(int32_t *t, int cols, int32_t focal) {
    int32_t divisor = 2 * focal * (cols / VIEW_W);
    for (int col = 0; col < cols / 2; col++) {
        int32_t slope = ((cols - 1 - 2 * col) << 16) / divisor;
        t[col] = -slope;
        t[cols - 1 - col] = slope;
    }
}

/* Send the ray for column col across the screen.
 *
 * This and columns() run from RAM with the rays. From ROM, as they were
 * (with a divide and two reciprocals done by subroutine), a ray cost a
 * third as much to send as to draw: a ray leaves little of the cache as it
 * found it, and code that is read back from ROM is read back slowly. */
static inline __attribute__((always_inline))
void cast(ray_fn *fn, uint8_t *dst, int col) {
    int32_t s = job.s, c = job.c, k = slope_t[col];
    int32_t dx = c - (int32_t)(((int64_t)s * k) >> 16);
    int32_t dy = s + (int32_t)(((int64_t)c * k) >> 16);

    /* Depth between voxel lines on each axis. A ray that runs along an
     * axis, or nearly, never meets the other axis's lines on screen. */
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int32_t tdx = ax < 256 ? NEVER : recip(ax);
    int32_t tdy = ay < 256 ? NEVER : recip(ay);

    fn(dst, job.ex, job.ey, dx, dy, tdx, tdy);
}

/* This CPU's share of the frame: every other column, starting at `first`. */
__attribute__((section(".ramtext"), noinline))
static void columns(int first) {
    uint8_t *pixels = (uint8_t *)(job.fb + FB_PIXELS);

    switch (job.res) {
    case PERSP_RES_COARSE:
        for (int col = first; col < VIEW_W; col += 2) {
            cast(ray_coarse, pixels + (VIEW_H - 1) * STRIDE + col * 2, col);
        }
        break;
    case PERSP_RES_WIDE:
        for (int col = first; col < 2 * VIEW_W; col += 2) {
            cast(ray_wide, pixels + (VIEW_H - 1) * STRIDE + col, col);
        }
        break;
    case PERSP_RES_FINE:
        for (int col = first; col < 2 * VIEW_W; col += 2) {
            cast(ray_fine, pixels + (2 * VIEW_H - 1) * STRIDE + col, col);
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
    int px, py, pz, piece;
    unsigned colour;

    for (int i = 0; (piece = vox_piece(i, &px, &py, &pz, &colour)); i++) {
        sh2z_fx_t rx = (px << 12) - job.ex, ry = (py << 12) - job.ey;
        sh2z_fx_t t = sh2z_mul(rx, job.c) + sh2z_mul(ry, job.s);
        sh2z_fx_t u = sh2z_mul(ry, job.c) - sh2z_mul(rx, job.s);
        if (t < job.t_near || t > job.t_far) continue;

        int32_t scale = (int32_t)(inv_t[(t - job.t_near) >> INV_STEP] >> 4);   /* focal / depth, Q12 */
        int fine_x = job.cols > VIEW_W, fine_y = job.rows > VIEW_H;
        int col = job.cols / 2 + (((u >> 8) * scale) >> (20 - fine_x));
        int row = (job.horizon << fine_y) + (((job.eye_h * 16 - pz) * scale) >> (16 - fine_y));
        if ((unsigned)col >= (unsigned)job.cols || (unsigned)row >= (unsigned)job.rows) continue;
        int x = fine_x ? col & ~1 : col * 2;
        uint8_t *at = (uint8_t *)(job.fb + FB_PIXELS) + row * STRIDE + x;
        if (piece == 2) {
            /* Larger pieces keep a lit top and a darker side as they fall. */
            unsigned material = colour & ~7u;
            int rows = fine_y ? 4 : 2;
            for (int dy = 0; dy < rows && row + dy < job.rows; dy++) {
                *(pair_t *)(at + dy * STRIDE) = vox_wtab[material + (dy < rows / 2 ? 7 : 6)];
                if (x + 2 < STRIDE)
                    *(pair_t *)(at + dy * STRIDE + 2) = vox_wtab[material + (dy < rows / 2 ? 6 : 4)];
            }
        } else {
            *(pair_t *)at = vox_wtab[colour];
            if (fine_y && row + 1 < job.rows) *(pair_t *)(at + STRIDE) = vox_wtab[colour];
        }
    }
}

/* During the drop, project the dressed model over the room. The ordinary
 * heightfield cannot leave air between a model and its floor. */
static void arrival_hero(void) {
    vox_creature_t hero;
    for (int i = 0; ; i++) {
        if (!vox_creature(i, &hero)) return;
        if (!hero.kind) break;
    }
    const vcol_t (*model)[CELL] = hero_dressed(0);
    int ground = vox_height_at(vox_hero_cx, vox_hero_cy);
    int fine_x = job.cols > VIEW_W, fine_y = job.rows > VIEW_H;
    uint8_t *pixels = (uint8_t *)(job.fb + FB_PIXELS);
    for (int ix = 0; ix < CELL; ix++) {
        int x = job.c >= 0 ? CELL - 1 - ix : ix;
        for (int iy = 0; iy < CELL; iy++) {
            int y = job.s >= 0 ? CELL - 1 - iy : iy;
            const vcol_t *col = vox_model_column(model, hero.face, x, y);
            if (!col->h) continue;
            int32_t rx = ((hero.x + x) << 16) + 0x8000 - job.ex;
            int32_t ry = ((hero.y + y) << 16) + 0x8000 - job.ey;
            int32_t t = sh2z_mul(rx, job.c) + sh2z_mul(ry, job.s);
            int32_t u = sh2z_mul(ry, job.c) - sh2z_mul(rx, job.s);
            if (t < job.t_near || t > job.t_far) continue;
            int scale = (int)(inv_t[(t - job.t_near) >> INV_STEP] >> 4);
            int width = ((scale << fine_x) + 4095) >> 12;
            int rows = ((scale << fine_y) + 4095) >> 12;
            int left = job.cols / 2 + (((u >> 8) * scale) >> (20 - fine_x)) - width / 2;
            int right = left + width;
            if (left < 0) left = 0;
            if (right > job.cols) right = job.cols;
            for (int z = 0; z < col->h; z++) {
                int altitude = ground + arrival_altitude + z + 1;
                int top = (job.horizon << fine_y) +
                          (((job.eye_h - altitude) * scale) >> (12 - fine_y));
                int bottom = top + rows;
                if (top < 0) top = 0;
                if (bottom > job.rows) bottom = job.rows;
                for (int row = top; row < bottom; row++) {
                    uint8_t colour = col->c[z] + (row == top ? 7 : 5);
                    uint8_t *at = pixels + row * STRIDE;
                    for (int sx = left; sx < right; sx++) {
                        if (fine_x) at[sx] = colour;
                        else ((pair_t *)at)[sx] = vox_wtab[colour];
                    }
                }
            }
        }
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

    int i = flat >> 4, f = flat & 15;
    int32_t focal = focal_step[i] + (i < 16 ? (focal_step[i + 1] - focal_step[i]) * f / 16 : 0);
    int32_t eye = focal + ((focal * PULL_BACK * pull) >> 16);
    int32_t horizon = TARGET_ROW - focal;      /* in coarse rows; a fine ray doubles it */
    int projection_changed = focal != job.focal;
    int depth_changed = projection_changed || eye != job.eye_h;
    int resolution_changed = res != job.res;
    job.res = res;
    job.cols = res == PERSP_RES_COARSE ? VIEW_W : 2 * VIEW_W;
    job.rows = res == PERSP_RES_FINE ? 2 * VIEW_H : VIEW_H;
    job.focal = focal;
    job.horizon = horizon;
    job.eye_h = eye;

    if (depth_changed) {
        /* Only depths between the first and last possible ray sample can
         * index these tables. Pulling closer shrinks that interval. */
        uint64_t eye_focal = (uint64_t)eye * focal;
        job.t_near = (int32_t)((eye_focal << 16) / (uint32_t)(VIEW_H - horizon));
        job.t_far = (int32_t)((eye_focal << 16) / (uint32_t)(0 - horizon));
        if (job.t_far - job.t_near > (int32_t)((INV_N - 2) << INV_STEP)) {
            job.t_far = job.t_near + (int32_t)((INV_N - 2) << INV_STEP);
        }

        unsigned last = (uint32_t)(job.t_far - job.t_near) >> INV_STEP;
        const uint32_t step = 1u << INV_STEP;
        const uint32_t divisor = (uint32_t)focal;
        uint32_t rise = (uint32_t)job.t_near / divisor;
        uint32_t remainder = (uint32_t)job.t_near - rise * divisor;
        uint32_t rise_step = step / divisor;
        uint32_t remainder_step = step - rise_step * divisor;
        uint32_t depth = (uint32_t)job.t_near >> 13;
        uint32_t numerator = divisor << 19;
        for (unsigned n = 0; n <= last; n++) {
            inv_t[n] = numerator / depth;
            rise_t[n] = rise;
            depth += step >> 13;

            /* Carry the remainder so this is exactly floor(t / focal)
             * without a second division for every depth sample. */
            rise += rise_step;
            remainder += remainder_step;
            if (remainder >= divisor) {
                rise++;
                remainder -= divisor;
            }
        }
    }
    /* A dolly changes the eye distance only. Rays per column and the
     * scanline projection stay fixed unless the focal length changes. */
    if (projection_changed || resolution_changed) {
        slopes(slope_t, job.cols, focal);
    }
    if (projection_changed) {
        for (int y = 0; y < 2 * VIEW_H; y++) {
            row_t[y] = (uint32_t)((((uint64_t)focal) << 31) / (uint32_t)(y - 2 * horizon));
        }
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
    if (arrival_altitude) arrival_hero();

    /* Two line offsets per aligned longword. The 112-row modes map
     * consecutive scanlines to the same source row. */
    quad_t *lines = (quad_t *)fb;
    unsigned stride = STRIDE / 2;
    uint32_t offsets = FB_PIXELS * 0x00010001u;
    if (job.rows > VIEW_H) {
        offsets += stride;
        stride *= 2;
    }
    uint32_t advance = stride * 0x00010001u;
    for (int k = 0; k < VIEW_H; k++, offsets += advance) lines[k] = offsets;
}
#endif
