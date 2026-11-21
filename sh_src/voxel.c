#include "mars.h"
#include "string.h"
#include "voxel.h"
#include "voxel_world.h"
#include "baked.h"

/* --- Materials ------------------------------------------------------
 * CRAM is laid out as material * 8 + shade, shade 7 brightest. Material 0
 * is reserved so index 0 stays the transparent void. The materials
 * themselves come from vox_models/materials.txt by way of baked.h. */
#define N_SHADES 8
#define MAT(n)   ((n) * N_SHADES)

/* --- Terrain vox_models ------------------------------------------------- */
enum {
    M_VOID, M_FLOOR, M_FLOOR2, M_WALL, M_DOOR_H, M_DOOR_V,
    M_PASSAGE, M_STAIRS, M_TRAP, M_GOLD, M_POTION, M_SCROLL, M_FOOD, M_WEAPON,
    M_ARMOR, M_RING, M_STICK, M_AMULET, M_MAGIC,
    M_ROCK_EDGE, M_ROCK_CORNER = M_ROCK_EDGE + 4,
    M_ROCK_JOIN = M_ROCK_CORNER + 4,
    M_COUNT = M_ROCK_JOIN + 4
};

vmodel_t vox_models[M_COUNT];
typedef char models_fit[M_COUNT <= VOX_MODELS_MAX ? 1 : -1];
const int vox_model_count = M_COUNT;
uint16_t vox_wtab[256];              /* CRAM index -> doubled pixel word */

/* The world as last loaded, one entry per Rogue screen cell. */
uint8_t vox_tile[MAP_H][MAP_W];    /* terrain model */
uint8_t vox_lift[MAP_H][MAP_W];    /* voxels of plinth under the cell's model */
static uint8_t stains[(MAP_H * MAP_W + 7) / 8];
uint8_t vox_light[MAP_H][MAP_W];   /* top-face shade 0..7 */

int vox_hero_cx, vox_hero_cy;
int vox_hero_armor = -1, vox_hero_weapon = -1;
int vox_hero_x, vox_hero_y;
int vox_hero_busy, vox_hero_jumped;
int vox_moving;
int vox_events;
int vox_hero_face;

/* --- Creatures --------------------------------------------------------
 * Rogue moves a creature a whole cell at a time. Here each one has a
 * position in voxels that walks to its cell over a few frames, with a
 * small hop, so the grid underneath does not show. */
#define MAX_ENTS   32
#define HERO_ID    1u
#define HOP_MAX    2
#define LUNGE      12     /* frames an attack keeps the hero busy */
#define JAB_WAIT   6      /* frames a creature's answer waits for the hero's blow */
#define FLASH      5      /* frames a hurt creature shows in one colour */
#define IMPACT_HOLD 6     /* vblanks of hitstop for a blow worth 25% max HP */
#define LOW_HP_HALF 12    /* vblanks in each half of the warning pulse */
#define MAX_PARTS  96
#define PART_LIFE  28
#define PART_ENEMY_SPATTER 2u  /* unused bit below a particle’s material index */
#define PART_REST  10     /* frames a piece lies where it fell */
#define GRAVITY    5      /* 1/16 voxel per frame per frame */
#define SLIDE_MAX  2      /* cells; a longer move is a teleport, not a walk */

typedef struct {
    uint32_t id;
    int16_t x, y;         /* top left voxel of its footprint, now */
    int16_t tx, ty;       /* and where that is heading */
    uint8_t kind;         /* 0 the hero, 1..26 A..Z */
    uint8_t left, total;  /* frames of the walk left, and its length */
    uint8_t lunge;        /* frames of a lunge left */
    int8_t ldx, ldy;      /* which way it lunges */
    int8_t ox, oy, z;     /* lunge offset and hop height, now */
    uint8_t seen;
    uint8_t face;         /* way it looks: 0 south, 1 west, 2 north, 3 east */
    uint8_t wait;         /* frames until the lunge starts */
    uint8_t flash_wait;   /* frames until the flash starts */
    uint8_t flash;        /* frames of flash left */
    uint8_t heavy_hit;    /* begin hitstop when the hit flash starts */
    int16_t hp;           /* hit points when last looked at */
} ent_t;

static ent_t ents[MAX_ENTS];
static int n_ents;
static ent_t defeated;
static int defeat_staged;
static uint32_t duel_foe;
static int hero_hit_pending;
static uint32_t impact_id;
static unsigned impact_left;
static unsigned low_hp_phase;
static int low_hp, hero_appearance_changed;

enum { PART_SMALL, PART_CHUNK, PART_BUOYANT, PART_FLOATING = PART_BUOYANT + 3 };

/* A loose voxel, in sixteenths: what a creature leaves when it dies. */
typedef struct {
    int16_t x, y, z;
    int8_t vx, vy, vz;
    uint8_t life;
    uint8_t pixel;        /* as in a creature sprite: material and face */
    uint8_t chunk;        /* particle size and whether it can float */
} part_t;

static part_t parts[MAX_PARTS];
static int n_parts;
static int last_depth;

#define HIT_FLASH    (MAT(MT_PAPER) + 7)

static int recoil_x(const ent_t *e) {
    if (!impact_left || e->id != impact_id) return 0;
    return (impact_left & 2) ? 1 : -1;
}

static unsigned creature_flash(const ent_t *e) {
    if (e->flash && !e->flash_wait) return HIT_FLASH;
    if (e->id == HERO_ID && low_hp && low_hp_phase < LOW_HP_HALF)
        return MAT(MT_RED) + 7;
    return 0;
}

/* Which way a step of (dx, dy) looks. */
static uint8_t facing(int dx, int dy) {
    if (abs(dx) > abs(dy)) return dx > 0 ? 3 : 1;
    return dy > 0 ? 0 : 2;
}

static const int8_t hop_z[8] = { 0, 1, 2, 2, 2, 2, 1, 1 };
static const int8_t lunge_reach[LUNGE + 1] = { 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 2 };

#define LIGHT_BRIGHT  7    /* the hero's room, and right around the hero */
#define LIGHT_NEAR    5
#define LIGHT_DIM     3    /* seen before, not in view now */
#define LIGHT_MEMORY  2    /* seen before, and Rogue has stopped showing it */

/* How high the ground of each cell stands, from the game, taken in once per
 * level. Rooms stand on plinths of different heights and passages run
 * between them at ground level. */
#define STAIR_DROP  2      /* the stairs' cell is this much lower than its room */

/* A front face is darker than the top face lit by the same light. */
const uint8_t vox_shadow_material = MAT(MT_SHADOW);
const uint8_t vox_stone_material = MAT(MT_STONE);
const uint8_t vox_blood_material = MAT(MT_FOOD); /* deep red */
const uint8_t vox_front_shade[N_SHADES] = { 0, 0, 1, 2, 2, 3, 4, 5 };

/* --- The canvas ------------------------------------------------------
 * Each framebuffer keeps a rendered strip of the world, so moving the
 * camera costs a line table rewrite and whatever columns newly came into
 * view, not a redraw.
 *
 * Every row the world can reach is kept (NR of them), so vertical movement
 * is free. Horizontally the framebuffer is treated as one long tape: the
 * word for canvas row r, world column X lives at (r * STRIDE + X) mod TAPE.
 * Pushing X past the end of a row runs into the start of the next row's
 * span, which is exactly where that row's column X - STRIDE used to be, so
 * the strip slides along the tape forever with no copying. A display line
 * that starts within VIEW_W of the tape's end reads past it; those words
 * are mirrored from the tape's start. */
#define ROW_MIN  CAM_Y_MIN
#define NR       (MAP_H * CELL - ROW_MIN)     /* 208 canvas rows */
#define STRIDE   312
#define TAPE     (NR * STRIDE)                /* 64896 words */
#define GUARD    VIEW_W
#define FB_TAPE  0x100                        /* tape starts after the line table */

typedef char tape_fits[(FB_TAPE + TAPE + GUARD <= 0x10000) ? 1 : -1];

/* slot_owner[buf][X % STRIDE] is the world column a framebuffer holds, up
 * to date, in that tape slot; -1 when it holds nothing good. */
static int16_t slot_owner[2][STRIDE];

/* When a cell changes, the columns through it that a framebuffer already
 * holds only need the rows that cell can reach repainted. One span of
 * canvas rows per cell column and framebuffer; lo > hi means clean. */
static uint8_t dirty_lo[2][MAP_W], dirty_hi[2][MAP_W];

/* Columns drawn ahead of need per frame, and how far past the view. */
#define AHEAD_PER_FRAME  8
#define AHEAD_REACH      24

static void col_set(vcol_t *c, int h, uint8_t mat) {
    c->h = (uint8_t)h;
    for (int z = 0; z < h; z++) c->c[z] = mat;
}

/* Pile n more voxels of one material on every column in a footprint. */
static void stack(vmodel_t *m, int x0, int x1, int y0, int y1, int n, uint8_t mat) {
    for (int lx = x0; lx <= x1; lx++) {
        for (int ly = y0; ly <= y1; ly++) {
            vcol_t *c = &m->col[lx][ly];
            for (int i = 0; i < n && c->h < COL_MAXH; i++) c->c[c->h++] = mat;
        }
    }
}

/* Clockwise from north, shared by the rock mesh and adjacency pass. */
static const int8_t edge_x[] = { 0, 1, 0, -1 };
static const int8_t edge_y[] = { -1, 0, 1, 0 };

static void build_models(void) {
    memset(vox_models, 0, sizeof(vox_models));

    for (int lx = 0; lx < CELL; lx++) {
        for (int ly = 0; ly < CELL; ly++) {
            int speck = ((lx * 5 + ly * 3) ^ (lx * ly)) & 3;

            col_set(&vox_models[M_FLOOR].col[lx][ly], 1,
                    terrain_floor[ly][lx]);
            col_set(&vox_models[M_FLOOR2].col[lx][ly], 1,
                    terrain_floor[ly][(CELL - lx) & (CELL - 1)]);
            col_set(&vox_models[M_PASSAGE].col[lx][ly], 1,
                    speck ? MAT(MT_DIRT) : MAT(MT_DIRT2));

            /* A single brick course and cap on every side of a room. */
            vcol_t *w = &vox_models[M_WALL].col[lx][ly];
            col_set(w, 2, MAT(MT_BRICKB));
            w->c[1] = terrain_wall_cap[ly][lx];

            /* The stone jambs line up with the wall caps around the threshold. */
            vcol_t *dh = &vox_models[M_DOOR_H].col[lx][ly];
            vcol_t *dv = &vox_models[M_DOOR_V].col[lx][ly];
            col_set(dh, 1, MAT(MT_WOOD));
            col_set(dv, 1, MAT(MT_WOOD));
            if (lx == 0 || lx == CELL - 1) *dh = *w;
            if (ly == 0 || ly == CELL - 1) *dv = *w;

            /* Steps going down to the right, from the level of the floor
             * around them into the pit their cell is sunk into. */
            int step = lx >> 1;
            col_set(&vox_models[M_STAIRS].col[lx][ly], STAIR_DROP + 1 - step,
                    (step & 1) ? MAT(MT_STAIR) : MAT(MT_CAP));
        }
    }

    /* Things lying on the floor. */
    for (int m = M_TRAP; m < M_ROCK_EDGE; m++) vox_models[m] = vox_models[M_FLOOR];

    for (int x = 0; x < CELL; x++) {
        for (int y = 0; y < CELL; y++) {
            int distance[] = { y, CELL - 1 - x, CELL - 1 - y, x };
            for (int d = 0; d < 4; d++) {
                unsigned rough = (x * 5 + y * 3 + d * 7) ^ (x * y);
                int a = distance[d], b = distance[(d + 1) & 3];
                uint8_t stone = MAT((rough & 2) ? MT_DIRT : MT_DIRT2);
                if (a < 2 + (int)(rough & 1))
                    col_set(&vox_models[M_ROCK_EDGE + d].col[x][y],
                            a == 0 ? 2 + ((rough >> 2) & 1) : 1, stone);
                if (a * a + b * b <= 9)
                    col_set(&vox_models[M_ROCK_CORNER + d].col[x][y],
                            a * a + b * b <= 4 ? 2 : 1, stone);
                /* Round the inside of the bend between two straight banks. */
                int ax = 6 - a, by = 6 - b;
                if (ax < 0) ax = 0;
                if (by < 0) by = 0;
                int radius_sq = ax * ax + by * by;
                if (radius_sq >= 20)
                    col_set(&vox_models[M_ROCK_JOIN + d].col[x][y],
                            radius_sq >= 30 ? 2 : 1, stone);
            }
        }
    }

    for (int i = 0; i < 4; i++)
        stack(&vox_models[M_TRAP], 2 + (i & 1) * 3, 2 + (i & 1) * 3,
              2 + (i >> 1) * 3, 2 + (i >> 1) * 3, 2, MAT(MT_STEEL));

    stack(&vox_models[M_GOLD], 2, 5, 2, 5, 1, MAT(MT_GOLD));
    stack(&vox_models[M_GOLD], 3, 4, 3, 4, 2, MAT(MT_GOLD));

    stack(&vox_models[M_POTION], 2, 5, 3, 4, 2, MAT(MT_POTION));
    stack(&vox_models[M_POTION], 3, 4, 3, 4, 1, MAT(MT_POTION));
    stack(&vox_models[M_POTION], 3, 4, 3, 4, 1, MAT(MT_CAP));

    stack(&vox_models[M_SCROLL], 2, 5, 3, 4, 1, MAT(MT_PAPER));
    stack(&vox_models[M_SCROLL], 2, 2, 3, 4, 1, MAT(MT_PAPER));
    stack(&vox_models[M_SCROLL], 5, 5, 3, 4, 1, MAT(MT_PAPER));

    stack(&vox_models[M_FOOD], 2, 5, 2, 5, 1, MAT(MT_FOOD));
    stack(&vox_models[M_FOOD], 3, 4, 3, 4, 1, MAT(MT_FOOD));
    stack(&vox_models[M_FOOD], 3, 3, 3, 3, 1, MAT(MT_TUNIC));

    stack(&vox_models[M_WEAPON], 2, 5, 3, 3, 1, MAT(MT_WOOD));
    stack(&vox_models[M_WEAPON], 3, 4, 3, 3, 5, MAT(MT_STEEL));

    stack(&vox_models[M_ARMOR], 2, 5, 3, 4, 3, MAT(MT_STEEL));
    stack(&vox_models[M_ARMOR], 3, 4, 3, 4, 1, MAT(MT_STEEL));

    stack(&vox_models[M_RING], 2, 5, 2, 2, 1, MAT(MT_GOLD));
    stack(&vox_models[M_RING], 2, 5, 5, 5, 1, MAT(MT_GOLD));
    stack(&vox_models[M_RING], 2, 2, 3, 4, 1, MAT(MT_GOLD));
    stack(&vox_models[M_RING], 5, 5, 3, 4, 1, MAT(MT_GOLD));
    stack(&vox_models[M_RING], 3, 4, 5, 5, 1, MAT(MT_RED));

    for (int i = 0; i < 4; i++)
        stack(&vox_models[M_STICK], 2 + i, 2 + i, 5 - i, 5 - i, 1 + i, MAT(MT_WOOD));
    stack(&vox_models[M_STICK], 5, 5, 2, 2, 1, MAT(MT_MAGIC));

    stack(&vox_models[M_AMULET], 3, 4, 3, 4, 2, MAT(MT_GOLD));
    stack(&vox_models[M_AMULET], 3, 4, 3, 4, 1, MAT(MT_RED));

    stack(&vox_models[M_MAGIC], 3, 4, 3, 4, 3, MAT(MT_MAGIC));

    for (int m = 0; m < M_COUNT; m++) vox_plinth[m] = MAT(MT_BRICKB);
    vox_plinth[M_STAIRS] = MAT(MT_SHADOW);
}

/* What a cell's plinth is made of, by the model that stands on it. */
uint8_t vox_plinth[M_COUNT];

static int is_wall(int ch) {
    return ch == '-' || ch == '|';
}

/* Model for a terrain or item character. under() answers what the level
 * itself holds at a cell, seen or not, which is what decides how a wall or
 * door sits. */
static uint8_t tile_for(int ch, int cx, int cy, vox_under_fn under) {
    switch (ch) {
    case '.': return ((cx + cy) & 1) ? M_FLOOR2 : M_FLOOR;
    case '|':
    case '-': return M_WALL;
    case '+':
        if (!is_wall(under(cx - 1, cy)) && !is_wall(under(cx + 1, cy))) return M_DOOR_V;
        return M_DOOR_H;
    case '#': return M_PASSAGE;
    case '%': return M_STAIRS;
    case '^': return M_TRAP;
    case '*': return M_GOLD;
    case '!': return M_POTION;
    case '?': return M_SCROLL;
    case ':': return M_FOOD;
    case ')': return M_WEAPON;
    case ']': return M_ARMOR;
    case '=': return M_RING;
    case '/': return M_STICK;
    case ',': return M_AMULET;
    case '$': return M_MAGIC;
    default:  return M_VOID;
    }
}

/* Creature index for a screen character, or -1: 0 the hero, 1..26 A..Z. */
static int ent_for(int ch) {
    if (ch == '@') return 0;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 1;
    return -1;
}

/* Canvas rows lo..hi of world columns x0..x1 no longer show the world, in
 * either framebuffer. */
static void stale(int x0, int x1, int lo, int hi) {
    if (lo < 0) lo = 0;
    if (hi > NR - 1) hi = NR - 1;
    for (int cx = x0 >> 3; cx <= x1 >> 3; cx++) {
        if (cx < 0 || cx >= MAP_W) continue;
        for (int buf = 0; buf < 2; buf++) {
            if (lo < dirty_lo[buf][cx]) dirty_lo[buf][cx] = (uint8_t)lo;
            if (hi > dirty_hi[buf][cx]) dirty_hi[buf][cx] = (uint8_t)hi;
        }
    }
}

/* The terrain of a cell lands on its own eight screen columns, no higher
 * than the tallest column and no lower than its near edge. */
static void stale_cell(int cx, int cy) {
    int near = cy * CELL - ROW_MIN + CELL - 1;
    stale(cx * CELL, cx * CELL + CELL - 1, near - (CELL - 1) - COL_MAXH, near);
}

#define MAX_ASSEMBLY 8
static struct {
    vmodel_t model;
    uint8_t x, y, tile;
} assembly[MAX_ASSEMBLY];
static int assembly_count, assembly_age, assembly_dx, assembly_dy;

int vox_assembly_cell(int i, int *cx, int *cy) {
    if (i >= assembly_count) return 0;
    *cx = assembly[i].x;
    *cy = assembly[i].y;
    return 1;
}

const vmodel_t *vox_assembly_model(int cx, int cy) {
    if (assembly_age >= 8) return 0;
    for (int i = 0; i < assembly_count; i++) {
        unsigned tile = assembly[i].tile;
        int end = tile == M_PASSAGE ? 6 : 8;
        if (assembly_age < end && assembly[i].x == cx && assembly[i].y == cy &&
            tile == vox_tile[cy][cx]) return &assembly[i].model;
    }
    return 0;
}

static void build_assembly(int i) {
    unsigned tile = assembly[i].tile;
    int bank = tile != M_PASSAGE;
    int limit = assembly_age / 2 * 3 + (bank ? 0 : 2);
    vmodel_t *m = &assembly[i].model;
    const vmodel_t *base = &vox_models[tile];
    for (int x = 0; x < CELL; x++) {
        for (int y = 0; y < CELL; y++) {
            int dx = assembly_dx > 0 ? x : CELL - 1 - x;
            int dy = assembly_dy > 0 ? y : CELL - 1 - y;
            int order = (assembly_dx ? dx : 0) + (assembly_dy ? dy : 0);
            if (assembly_dx && assembly_dy) order >>= 1;
            int scatter = ((x * 3 + y * 5) ^ (x * y)) & 3;
            if (order + scatter < limit) m->col[x][y] = base->col[x][y];
            else m->col[x][y].h = 0;
        }
    }
}

static void queue_assembly(int x, int y, unsigned tile) {
    if (assembly_count == MAX_ASSEMBLY) return;
    int i = assembly_count++;
    assembly[i].x = (uint8_t)x;
    assembly[i].y = (uint8_t)y;
    assembly[i].tile = (uint8_t)tile;
    build_assembly(i);
}

static void animate_assembly(void) {
    if (!assembly_count || assembly_age >= 8 || (++assembly_age & 1)) return;
    for (int i = 0; i < assembly_count; i++) {
        int end = assembly[i].tile == M_PASSAGE ? 6 : 8;
        if (assembly_age > end) continue;
        if (assembly_age < end) build_assembly(i);
        stale_cell(assembly[i].x, assembly[i].y);
        vox_moving = 1;
    }
}

/* How high the ground is under the middle of a footprint at (x, y). */
static int lift_under(int x, int y) {
    return vox_lift[(y + CELL / 2) >> 3][(x + CELL / 2) >> 3];
}

/* The patch of canvas a creature covers where it stands right now. A cell
 * that changes height stales itself, and the creature is redrawn with it,
 * so the tallest ground is allowed for here. */
static void stale_ent(const ent_t *e) {
    int x = e->x + e->ox + recoil_x(e);
    int r = e->y + e->oy - ROW_MIN;
    stale(x, x + CELL - 1, r - ENT_ORIGIN - e->z - LIFT_MAX, r + CELL - 1);
}

/* Rogue shows a creature at a cell. If it is one already known and the cell
 * is close, it walks there over `frames` frames; otherwise it is simply
 * there. */
static void ent_place(uint32_t id, int kind, int cx, int cy, int frames) {
    int tx = cx * CELL, ty = cy * CELL;
    ent_t *e = 0;

    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id == id) { e = &ents[i]; break; }
    }
    if (e) {
        int dx = abs(tx - e->tx), dy = abs(ty - e->ty);
        e->seen = 1;
        if (e->kind != kind) {
            e->kind = (uint8_t)kind;
            stale_ent(e);
        }
        if (dx == 0 && dy == 0) return;
        e->face = facing(tx - e->tx, ty - e->ty);
        e->tx = (int16_t)tx;
        e->ty = (int16_t)ty;
        if (frames > 0 && dx <= SLIDE_MAX * CELL && dy <= SLIDE_MAX * CELL) {
            if (id == HERO_ID) vox_events |= VOX_STEPPED;
            e->left = e->total = (uint8_t)frames;
            return;
        }
        stale_ent(e);
    } else {
        if (n_ents == MAX_ENTS) return;
        e = &ents[n_ents++];
        memset(e, 0, sizeof(*e));
        e->id = id;
        e->kind = (uint8_t)kind;
        e->seen = 1;
        e->tx = (int16_t)tx;
        e->ty = (int16_t)ty;
    }
    e->x = e->tx;
    e->y = e->ty;
    e->left = 0;
    e->z = 0;
    stale_ent(e);
    if (id == HERO_ID) vox_hero_jumped = 1;
}

static void stale_part(const part_t *p) {
    int x = p->x >> 4;
    int r = (p->y >> 4) - (p->z >> 4) - ROW_MIN;
    stale(x, x, r, r);
}

static unsigned chance(void) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
}

int vox_stained(int cx, int cy) {
    unsigned cell = cy * MAP_W + cx;
    return (stains[cell >> 3] >> (cell & 7)) & 1;
}

static void stain_enemy_floor(const ent_t *e) {
    unsigned cx = e->tx / CELL, cy = e->ty / CELL;
    if (cx >= MAP_W || cy >= MAP_H || vox_stained(cx, cy)) return;
    unsigned cell = cy * MAP_W + cx;
    stains[cell >> 3] |= 1u << (cell & 7);
    stale_cell(cx, cy);
}

static void spatter(const ent_t *e, int count);

/* Turn a creature into loose voxels where it stands: a sample of its
 * sprite, thrown up and outward. */
static void shatter(const ent_t *e, int chunk) {
    int ex = e->x + e->ox, ey = e->y + e->oy;
    int total = 0, skip, n = 0;
    unsigned floating_bands = 0;

    for (int lx = 0; lx < CELL; lx++) {
        for (int k = 0; k < ENT_ROWS; k++) total += ent_sprite[e->kind][lx][k] != 0;
    }
    skip = total / 40 + 1;

    for (int lx = 0; lx < CELL; lx++) {
        for (int k = 0; k < ENT_ROWS; k++) {
            unsigned v = ent_sprite[e->kind][lx][k];
            if (!v || n++ % skip || n_parts == MAX_PARTS) continue;

            /* The sprite is flat; give the pixel a depth somewhere in the
             * footprint, and the height that puts it on the same row. */
            int d = 1 + (int)(chance() % (CELL - 2));
            int y = ey + d, z = d + ENT_ORIGIN - k;
            if (z < 0) { y -= z; z = 0; }
            z += lift_under(ex, ey);

            part_t *p = &parts[n_parts++];
            p->x = (int16_t)((ex + lx) << 4);
            p->y = (int16_t)(y << 4);
            p->z = (int16_t)(z << 4);
            p->vx = (int8_t)((lx * 2 - 7) * 2 + (int)(chance() & 7) - 4);
            p->vy = (int8_t)((int)(chance() & 15) - 8);
            p->vz = (int8_t)(16 + (chance() & 15));
            p->life = PART_LIFE + (chunk ? PART_REST : 0);
            p->pixel = (uint8_t)v;
            p->chunk = (uint8_t)chunk;
            unsigned band = k < ENT_ORIGIN - 4 ? 2 : 0;
            if (chunk && e->kind && lx >= 2 && lx <= 5 &&
                !(floating_bands & (1u << band)) && (v & ~7u) != MAT(MT_SHADOW)) {
                floating_bands |= 1u << band;
                p->chunk = (uint8_t)(PART_BUOYANT + band);
                p->life = 66;
                p->vx = (int8_t)(e->tx / CELL);
                p->vy = (int8_t)(e->ty / CELL);
            }
            stale_part(p);
        }
    }
}

int vox_defeat_target(int *x, int *y) {
    if (!defeat_staged) return 0;
    *x = defeated.x + defeated.ox + CELL / 2;
    *y = defeated.y + defeated.oy + CELL / 2;
    return 1;
}

void vox_defeat_flash(void) {
    if (!defeat_staged) return;
    defeated.flash = FLASH;
    defeated.flash_wait = 0;
    spatter(&defeated, 12);
}

static int adjacent_to_hero(const ent_t *e) {
    int dx = e->tx - vox_hero_cx * CELL;
    int dy = e->ty - vox_hero_cy * CELL;
    return dx >= -CELL && dx <= CELL && dy >= -CELL && dy <= CELL;
}

int vox_duel_target(vox_duel_t *shot) {
    const ent_t *target = 0;
    int nearest = 2 * CELL * CELL + 1;
    for (int i = 0; i < n_ents; i++) {
        const ent_t *e = &ents[i];
        if (!e->kind || !adjacent_to_hero(e)) continue;
        /* Keep the current opponent while it is in reach. Otherwise
         * prefer an edge neighbor over one across a corner. */
        if (e->id == duel_foe) {
            target = e;
            break;
        }
        int dx = e->tx - vox_hero_cx * CELL;
        int dy = e->ty - vox_hero_cy * CELL;
        int distance = dx * dx + dy * dy;
        if (distance < nearest) {
            target = e;
            nearest = distance;
        }
    }
    duel_foe = target ? target->id : 0;
    if (!target) return 0;
    shot->id = target->id;
    shot->x = target->x + CELL / 2;
    shot->y = target->y + CELL / 2;
    shot->dx = target->tx / CELL - vox_hero_cx;
    shot->dy = target->ty / CELL - vox_hero_cy;
    return 1;
}

void vox_release_defeat(void) {
    if (!defeat_staged) return;
    stale_ent(&defeated);
    shatter(&defeated, 1);
    stain_enemy_floor(&defeated);
    vox_events |= VOX_BURST;
    defeat_staged = 0;
}

void vox_hero_hit_flash(int on) {
    for (int i = 0; i < n_ents; i++) {
        ent_t *e = &ents[i];
        if (e->id != HERO_ID) continue;
        stale_ent(e);
        e->flash = on ? FLASH : 0;
        e->flash_wait = 0;
        e->heavy_hit = 0;
        return;
    }
}

static void hero_fragment(int x, int y, int z, int vx, int vy, int vz,
                          unsigned pixel, int chunk) {
    part_t *p = &parts[n_parts++];
    *p = (part_t){
        .x = (int16_t)(x * 16), .y = (int16_t)(y * 16), .z = (int16_t)(z * 16),
        .vx = (int8_t)vx, .vy = (int8_t)vy, .vz = (int8_t)vz,
        .life = chunk >= PART_BUOYANT ? 66 : PART_LIFE + (chunk ? PART_REST : 0),
        .pixel = (uint8_t)pixel, .chunk = (uint8_t)chunk,
    };
    stale_part(p);
}

static void spatter(const ent_t *e, int count) {
    int ex = e->x + e->ox, ey = e->y + e->oy;
    int floor = lift_under(ex, ey) + e->z;
    for (int n = 0; n < count && n_parts < MAX_PARTS; n++) {
        int vx = (int)(chance() % 55) - 27;
        int vy = (int)(chance() % 47) - 23;
        int vz = 17 + (int)(chance() & 15);
        int z = floor + 5 + (int)(chance() & 3);
        unsigned pixel = MAT(MT_RED) | (n & 1);
        if (e->kind) pixel |= PART_ENEMY_SPATTER;
        hero_fragment(ex + CELL / 2, ey + CELL / 2, z, vx, vy, vz, pixel, 0);
    }
}

void vox_hero_spatter(void) {
    for (int i = 0; i < n_ents; i++) {
        const ent_t *e = &ents[i];
        if (e->id != HERO_ID) continue;
        /* Reserve room for the spray and the later body breakup. */
        while (n_parts > MAX_PARTS / 8) stale_part(&parts[--n_parts]);
        spatter(e, 28);
        return;
    }
}

void vox_hero_shatter(const vcol_t model[CELL][CELL]) {
    for (int i = 0; i < n_ents; i++) {
        ent_t *e = &ents[i];
        if (e->id != HERO_ID) continue;
        int ex = e->x + e->ox, ey = e->y + e->oy;
        int floor = lift_under(ex, ey) + e->z;

        if (model) {
            unsigned floating_bands = 0;
            for (int x = 0; x < CELL; x++) {
                for (int y = 0; y < CELL; y++) {
                    const vcol_t *col = vox_model_column(model, e->face, x, y);
                    for (int z = 0; z < col->h && n_parts < MAX_PARTS; z++) {
                        unsigned material = col->c[z];
                        if (material == MAT(MT_SHADOW) || (x + y + z) % 6) continue;
                        int vx = (2 * x - 7) * 2 + (int)(chance() & 7) - 3;
                        int vy = (2 * y - 7) * 2 + (int)(chance() & 7) - 3;
                        int vz = 4 + (int)(chance() & 7);
                        int kind = PART_CHUNK;
                        unsigned band = z >= 9 ? 2 : z >= 3 ? 1 : 0;
                        if (x >= 2 && x <= 5 && y >= 2 && y <= 5 && !(floating_bands & (1u << band))) {
                            floating_bands |= 1u << band;
                            kind = PART_BUOYANT + band;
                            vx = vox_hero_cx;
                            vy = vox_hero_cy;
                        }
                        hero_fragment(ex + x, ey + y, floor + 1 + z, vx, vy, vz, material + 1, kind);
                    }
                }
            }
        } else {
            shatter(e, 0);
        }
        stale_ent(e);
        *e = ents[--n_ents];
        return;
    }
}

int vox_particles_active(void) { return n_parts != 0; }

static int drift_toward(int difference, int speed) {
    return difference < -speed ? -speed : difference > speed ? speed : difference;
}

static void parts_animate(void) {
    for (int i = 0; i < n_parts; ) {
        part_t *p = &parts[i];
        stale_part(p);
        int rest = p->chunk ? 2 * PART_REST : PART_REST;
        if (p->chunk >= PART_BUOYANT) {
            static const int8_t pool_x[] = { -23, 22, 0 };
            static const int8_t pool_y[] = {  10,  9, -23 };
            int floating = p->chunk >= PART_FLOATING;
            unsigned band = p->chunk - (floating ? PART_FLOATING : PART_BUOYANT);
            int phase = (p->life + band * 5) & 15;
            int bob = phase < 8 ? phase : 15 - phase;
            int target_x = (p->vx * CELL + CELL / 2) * 16 + pool_x[band];
            int target_y = (p->vy * CELL + CELL / 2) * 16 + pool_y[band];
            if (floating) {
                target_x += bob - 3;
                target_y -= bob - 3;
            }
            p->x += drift_toward(target_x - p->x, floating ? 1 : 5);
            p->y += drift_toward(target_y - p->y, floating ? 1 : 5);
            if (!floating) {
                p->z += p->vz;
                p->vz -= GRAVITY;
            }
        } else if (p->life > rest) {
            p->x += p->vx;
            p->y += p->vy;
            p->z += p->vz;
            p->vz -= GRAVITY;
        }
        if (--p->life == 0 || p->x < 0 || p->x >= (MAP_W * CELL) << 4 ||
            p->y < CELL << 4 || p->y >= ((MAP_H - 1) * CELL) << 4) {
            *p = parts[--n_parts];
            continue;
        }
        int cx = p->x >> 7, cy = p->y >> 7;
        int floor = vox_lift[cy][cx] << 4;
        if (p->chunk >= PART_BUOYANT) {
            const vcol_t *terrain = &vox_models[vox_tile[cy][cx]].col[(p->x >> 4) & 7][(p->y >> 4) & 7];
            int surface = floor + terrain->h * 16;
            if (p->chunk < PART_FLOATING && p->z <= surface && p->vz <= 0)
                p->chunk += PART_FLOATING - PART_BUOYANT;
            if (p->chunk >= PART_FLOATING) {
                int phase = (p->life + p->pixel) & 15;
                p->z = (int16_t)(surface + 5 + (phase < 8 ? phase : 15 - phase));
            }
        } else if (p->z <= floor && p->vz <= 0) {
            p->z = (int16_t)floor;
            if (p->life > rest) p->life = (uint8_t)rest;
        }
        stale_part(p);
        i++;
    }
}

/* Wind-up, contact, follow-through. Weapon offsets are relative to the
 * front-facing hero. Tilt brings the upper end forward around the grip. */
const vox_attack_pose_t *vox_attack_pose(int weapon, unsigned remaining) {
    static const vox_attack_pose_t poses[10][3] = {
        { {-1,0, 0,-1,2,-1}, { 3,1, 0,1,0,3}, { 1,0, 0,2,-1,3} }, /* mace */
        { {-1,0, 0,-2,1,-2}, { 3,0, 0,1,1,-4}, { 1,0, 1,1,0,-3} }, /* sword */
        { { 0,0, 0,-1,1, 0}, { 2,0, 0,2,1,1}, { 0,0, 0,1, 0,1} }, /* bow */
        { { 0,0, 0,-1,1, 0}, { 2,0, 0,1,2,3}, { 1,0, 0,1, 1,1} }, /* arrow */
        { { 0,0, 0,-1,1, 0}, { 4,0, 0,2,1,4}, { 2,0, 0,1, 0,2} }, /* dagger */
        { {-2,0, 0,-2,2,-3}, { 2,1, 0,1,1,-5}, { 1,0, 1,2, 0,-4} }, /* two-handed sword */
        { { 0,0, 0,-1,2,-1}, { 2,0, 0,2,2,3}, { 0,0, 0,1, 1,0} }, /* dart */
        { {-1,0, 0,-1,1,-1}, { 2,0, 0,2,1,3}, { 1,0, 1,1, 0,2} }, /* shuriken */
        { {-1,0, 0,-2,1, 2}, { 4,0, 0,1,0,5}, { 2,0, 0,1, 0,4} }, /* spear */
        { {-1,0, 0, 0,0, 0}, { 3,0, 0,0,0,0}, { 1,0, 0,0, 0,0} }, /* unarmed */
    };
    if (remaining <= 2) return 0;
    unsigned phase = remaining >= 10 ? 0 : remaining >= 6 ? 1 : 2;
    return &poses[(unsigned)weapon < 9 ? weapon : 9][phase];
}

static void hero_follow(void) {
    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id != HERO_ID) continue;
        vox_hero_x = ents[i].x;
        vox_hero_y = ents[i].y;
        vox_hero_busy = ents[i].left || ents[i].lunge || ents[i].heavy_hit || impact_left;
        vox_hero_face = ents[i].face;
        return;
    }
    vox_hero_busy = 0;
}

void vox_animate(void) {
    vox_moving = hero_appearance_changed;
    hero_appearance_changed = 0;
    if (low_hp && ++low_hp_phase == 2 * LOW_HP_HALF) low_hp_phase = 0;
    ent_t *recoiling = 0;
    for (int i = 0; i < n_ents; i++) {
        ent_t *e = &ents[i];
        if (low_hp && e->id == HERO_ID &&
            (low_hp_phase == 0 || low_hp_phase == LOW_HP_HALF)) {
            stale_ent(e);
            vox_moving = 1;
        }
        if (!impact_left && e->heavy_hit && !e->flash_wait) {
            stale_ent(e);
            e->heavy_hit = 0;
            impact_id = e->id;
            impact_left = IMPACT_HOLD + 1;
        }
        if (impact_left && e->id == impact_id) recoiling = e;
    }
    if (impact_left) {
        if (recoiling) stale_ent(recoiling);
        impact_left--;
        if (recoiling) stale_ent(recoiling);
        vox_moving = 1;
        hero_follow();
        return;
    }
    animate_assembly();
    for (int i = 0; i < n_ents; i++) {
        ent_t *e = &ents[i];
        if (!e->left && !e->lunge && !e->flash) continue;
        vox_moving = 1;

        stale_ent(e);
        if (e->left) {
            e->x += (e->tx - e->x) / e->left;
            e->y += (e->ty - e->y) / e->left;
            e->left--;
            e->z = e->left ? hop_z[(e->total - e->left) & 7] : 0;
        }
        if (e->wait) {
            e->wait--;
        } else if (e->lunge) {
            e->lunge--;
            int reach = lunge_reach[e->lunge];
#ifdef PERSP
            if (e->id == HERO_ID) {
                const vox_attack_pose_t *pose = vox_attack_pose(vox_hero_weapon, e->lunge);
                reach = pose ? pose->reach : 0;
                e->z = pose ? pose->hop : 0;
            }
#endif
            e->ox = (int8_t)(e->ldx * reach);
            e->oy = (int8_t)(e->ldy * reach);
        }
        if (e->flash_wait) e->flash_wait--;
        else if (e->flash) e->flash--;
        stale_ent(e);
    }
    parts_animate();
    hero_follow();
}

void vox_jab(uint32_t id) {
    ent_t *hero = 0, *e = 0;

    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id == HERO_ID) hero = &ents[i];
        if (ents[i].id == id) e = &ents[i];
    }
    if (!hero || !e) return;
    e->ldx = (int8_t)((hero->tx > e->tx) - (hero->tx < e->tx));
    e->ldy = (int8_t)((hero->ty > e->ty) - (hero->ty < e->ty));
    e->lunge = LUNGE;
    e->wait = JAB_WAIT;
    if (e->ldx || e->ldy) e->face = facing(e->ldx, e->ldy);
    /* If the hero was just hurt, this is the blow that did it. */
    if (hero_hit_pending) {
        hero->flash_wait = JAB_WAIT + 2;
        if (!duel_foe && adjacent_to_hero(e)) duel_foe = id;
        hero_hit_pending = 0;
    }
}

static inline int spatter_dimmed(const part_t *p) {
    return (p->pixel & PART_ENEMY_SPATTER) && p->life <= PART_LIFE - 2;
}

int vox_piece(int i, int *x, int *y, int *z, unsigned *colour) {
    if (i >= n_parts) return 0;
    const part_t *p = &parts[i];
    *x = p->x;
    *y = p->y;
    *z = p->z;
    *colour = (p->pixel & ~7u) + ((p->pixel & 1) ? 7 : 5) - spatter_dimmed(p);
    return p->chunk ? 2 : 1;
}

int vox_creature(int i, vox_creature_t *c) {
    if (i >= n_ents + defeat_staged) return 0;
    const ent_t *e = i < n_ents ? &ents[i] : &defeated;
    c->x = e->x + e->ox + recoil_x(e);
    c->y = e->y + e->oy;
    c->z = e->z;
    c->kind = e->kind;
    c->face = e->face;
    c->attack = e->id == HERO_ID ? e->lunge : 0;
    if (defeat_staged && e->id == HERO_ID)
        c->face = facing(defeated.x - e->x, defeated.y - e->y);
    c->flash = creature_flash(e);
    return 1;
}

void vox_hero_turn(int dx, int dy) {
    for (int i = 0; i < n_ents; i++) {
        ent_t *hero = &ents[i];
        if (hero->id != HERO_ID) continue;
        uint8_t face = facing(dx, dy);
        if (hero->face != face) {
            stale_ent(hero);
            hero->face = face;
            vox_hero_face = face;
            hero_appearance_changed = 1;
        }
        return;
    }
}

void vox_hero_lunge(int dx, int dy) {
    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id != HERO_ID) continue;
        ents[i].lunge = LUNGE;
        ents[i].ldx = (int8_t)dx;
        ents[i].ldy = (int8_t)dy;
        ents[i].face = facing(dx, dy);
        vox_hero_busy = 1;
        vox_events |= VOX_SWUNG;
    }
}

static unsigned passage_at(int x, int y) {
    return (unsigned)x < MAP_W && y > 0 && y < MAP_H - 1 &&
           vox_tile[y][x] == M_PASSAGE;
}

/* Line discovered passages on the rendering map after the real tiles have
 * settled. Blank screen cells can conceal a real floor or door, so check
 * the underlying cell before placing scenery there. */
static void fringe_passages(vox_under_fn under, int assembling, int tread_count) {
    for (int y = 1; y < MAP_H - 1; y++) {
        for (int x = 0; x < MAP_W; x++) {
            unsigned old = vox_tile[y][x];
            if (old != M_VOID && old < M_ROCK_EDGE) continue;
            unsigned rock = M_VOID, light = vox_light[y][x];
            unsigned edges = 0;
            for (int d = 0; d < 4; d++) {
                int nx = x + edge_x[d], ny = y + edge_y[d];
                if (!passage_at(nx, ny)) continue;
                if (!edges || vox_light[ny][nx] > light) light = vox_light[ny][nx];
                if (!edges) rock = M_ROCK_EDGE + d;
                edges |= 1u << d;
            }
            for (int d = 0; edges && d < 4; d++) {
                unsigned pair = (1u << d) | (1u << ((d + 1) & 3));
                if ((edges & pair) != pair) continue;
                rock = M_ROCK_JOIN + d;
                break;
            }
            if (rock == M_VOID) {
                for (int d = 0; d < 4; d++) {
                    int next = (d + 1) & 3;
                    int nx = x + edge_x[d] + edge_x[next];
                    int ny = y + edge_y[d] + edge_y[next];
                    if (!passage_at(nx, ny) ||
                        !passage_at(nx + edge_x[d], ny + edge_y[d]) ||
                        !passage_at(nx + edge_x[next], ny + edge_y[next])) continue;
                    rock = M_ROCK_CORNER + d;
                    light = vox_light[ny][nx];
                    break;
                }
            }
            if (rock != M_VOID && under(x, y) != ' ') rock = M_VOID;
            if (rock == old && (rock == M_VOID || light == vox_light[y][x])) continue;
            vox_tile[y][x] = (uint8_t)rock;
            vox_light[y][x] = (uint8_t)light;
            vox_lift[y][x] = 0;
            stale_cell(x, y);
            if (!assembling || old != M_VOID || rock == M_VOID) continue;
            for (int i = 0; i < tread_count; i++) {
                if (abs(x - assembly[i].x) > 1 || abs(y - assembly[i].y) > 1) continue;
                queue_assembly(x, y, rock);
                break;
            }
        }
    }
}

/* Row 0 of Rogue's screen is its message line and the last row its status
 * line; neither is dungeon. */
int vox_load_screen(const uint8_t *cells, const vox_source_t *src,
                     const int *room, int depth, int frames, int stage_defeat) {
    vox_under_fn under = src->under;
    int same_level = (depth == last_depth);
    int terrain_changed = !same_level;
    int has_passages = 0;
    int old_x = vox_hero_cx, old_y = vox_hero_cy;

    last_depth = depth;
    hero_hit_pending = 0;
    if (!same_level) {
        frames = 0;
        duel_foe = 0;
        impact_left = 0;
        memset(stains, 0, sizeof stains);
        while (n_parts) stale_part(&parts[--n_parts]);
    }
    for (int i = 0; i < n_ents; i++) ents[i].seen = 0;

    for (int i = MAP_W; i < (MAP_H - 1) * MAP_W; i++) {
        if ((cells[i] & 0x7F) == '@') {
            vox_hero_cx = i % MAP_W;
            vox_hero_cy = i / MAP_W;
            break;
        }
    }
    int dx = vox_hero_cx - old_x, dy = vox_hero_cy - old_y;
    int assembling = same_level && frames > 0 && (dx || dy) && abs(dx) <= 1 && abs(dy) <= 1;
    if (!same_level || dx || dy) {
        for (int i = 0; i < assembly_count; i++) stale_cell(assembly[i].x, assembly[i].y);
        assembly_count = assembly_age = 0;
        assembly_dx = dx;
        assembly_dy = dy;
    }

    for (int cy = 1; cy < MAP_H - 1; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            int ch = cells[cy * MAP_W + cx] & 0x7F;
            int ent = ent_for(ch);
            uint8_t tile = tile_for(ent < 0 ? ch : under(cx, cy), cx, cy, under);
            if (tile == M_VOID && same_level && vox_tile[cy][cx] >= M_ROCK_EDGE)
                continue;   /* refreshed after all discovered tiles are loaded */

            uint8_t light = LIGHT_DIM;
            if (room && cx >= room[0] && cx <= room[2] && cy >= room[1] && cy <= room[3]) {
                light = LIGHT_BRIGHT;
            } else {
                int dx = abs(cx - vox_hero_cx), dy = abs(cy - vox_hero_cy);
                int d = dx > dy ? dx : dy;
                if (d <= 1) light = LIGHT_BRIGHT;
                else if (d == 2) light = LIGHT_NEAR;
            }

            /* Rogue forgets the floor of a dark room as soon as the hero
             * steps away. The hero does not: what was there stays, dimmer. */
            if (tile == M_VOID && same_level && vox_tile[cy][cx] != M_VOID) {
                tile = vox_tile[cy][cx];
                if (light < LIGHT_NEAR) light = LIGHT_MEMORY;
            }
            has_passages |= tile == M_PASSAGE;

            int lift = 0;
            if (tile != M_VOID) {
                unsigned old = vox_tile[cy][cx];
                /* Room height is fixed for the level. Query it when this
                 * tile first appears or changes to or from a stair. */
                if (same_level && old != M_VOID && old < M_ROCK_EDGE &&
                    (old == M_STAIRS) == (tile == M_STAIRS)) {
                    lift = vox_lift[cy][cx];
                } else {
                    lift = src->ground(cx, cy);
                    if (lift > LIFT_MAX) lift = LIFT_MAX;
                    if (tile == M_STAIRS) lift = lift > STAIR_DROP ? lift - STAIR_DROP : 0;
                }
            }

            if (tile != vox_tile[cy][cx] || light != vox_light[cy][cx] ||
                lift != vox_lift[cy][cx]) {
                if (assembling && tile == M_PASSAGE && vox_tile[cy][cx] != M_PASSAGE) {
                    int x = cx - vox_hero_cx, y = cy - vox_hero_cy;
                    if (abs(x) <= 1 && abs(y) <= 1 && x * dx + y * dy >= 0)
                        queue_assembly(cx, cy, tile);
                }
                vox_tile[cy][cx] = tile;
                vox_light[cy][cx] = light;
                vox_lift[cy][cx] = (uint8_t)lift;
                stale_cell(cx, cy);
                terrain_changed = 1;
            }

            if (ent >= 0) {
                /* A letter nobody answers for is known by where it is. */
                uint32_t id = ent == 0 ? HERO_ID : src->who(cx, cy);
                if (id == 0) id = 0x80000000u | (uint32_t)(cy * MAP_W + cx);
                ent_place(id, ent, cx, cy, frames);
            }
        }
    }

    if (terrain_changed && has_passages) fringe_passages(under, assembling, assembly_count);

    /* Whatever Rogue no longer shows is gone, in pieces if it died. What
     * is still here and has fewer hit points than before was hurt, and
     * flashes: at once, or for a monster as the hero's blow lands. */
    for (int i = 0; i < n_ents; ) {
        ent_t *e = &ents[i];
        if (!e->seen) {
            stale_ent(e);
            if (same_level && !(e->id & 0x80000000u) && !src->alive(e->id)) {
                int dx = e->tx - vox_hero_cx * CELL;
                int dy = e->ty - vox_hero_cy * CELL;
                if (stage_defeat && !defeat_staged && e->kind &&
                    dx * dx + dy * dy <= 36 * CELL * CELL) {
                    defeated = *e;
                    defeated.flash = 0;
                    defeated.face = facing(-dx, -dy);
                    defeat_staged = 1;
                } else {
                    shatter(e, 0);
                    if (e->kind) {
                        stain_enemy_floor(e);
                        terrain_changed = 1;
                    }
                    vox_events |= VOX_BURST;
                }
            }
            *e = ents[--n_ents];
            continue;
        }
        if (!(e->id & 0x80000000u)) {
            int max_hp;
            int hp = src->hp(e->id, &max_hp);
            if (e->id == HERO_ID) {
                int warning = hp > 0 && hp * 20 < max_hp;
                if (warning != low_hp) {
                    low_hp = warning;
                    low_hp_phase = 0;
                    hero_appearance_changed = 1;
                    stale_ent(e);
                }
            }
            if (hp > 0 && hp < e->hp && same_level) {
                e->flash = FLASH;
                e->heavy_hit = max_hp > 0 && (e->hp - hp) * 4 >= max_hp;
                vox_events |= e->id == HERO_ID ? VOX_HURT : VOX_HIT;
                e->flash_wait = (uint8_t)(e->id == HERO_ID ? 0 : 2);
                if (e->id == HERO_ID) hero_hit_pending = 1;
                else if (adjacent_to_hero(e)) duel_foe = e->id;
            }
            e->hp = (int16_t)hp;
        }
        i++;
    }
    hero_follow();
    return terrain_changed;
}

void vox_palette(int light) {
    volatile uint16_t *cram = &MARS_CRAM;

    for (int i = 0; i < N_SHADES; i++) cram[i] = 0;
    for (int m = 1; m < MT_COUNT; m++) {
        for (int s = 0; s < N_SHADES; s++) {
            int k = light * (s + 2);   /* shade 0 is 2/9 brightness, shade 7 is full */
            cram[MAT(m) + s] = COLOR(mat_rgb[m][0] * k / (9 * VOX_FULL_LIGHT),
                                     mat_rgb[m][1] * k / (9 * VOX_FULL_LIGHT),
                                     mat_rgb[m][2] * k / (9 * VOX_FULL_LIGHT));
        }
    }
}

static uint8_t model_height[M_COUNT];      /* tallest column of each model */

int vox_stair_at(int cx, int cy) {
    return (unsigned)cx < MAP_W && (unsigned)cy < MAP_H && vox_tile[cy][cx] == M_STAIRS;
}

int vox_height_at(int cx, int cy) {
    if ((unsigned)cx >= MAP_W || (unsigned)cy >= MAP_H || vox_tile[cy][cx] == M_VOID) return 0;
    return vox_lift[cy][cx] + model_height[vox_tile[cy][cx]];
}

void vox_forget(void) {
    memset(slot_owner, 0xFF, sizeof(slot_owner));
}

void vox_init(void) {
    vox_palette(VOX_FULL_LIGHT);
    for (int i = 0; i < 256; i++) vox_wtab[i] = (uint16_t)(i * 0x0101);
    memset(slot_owner, 0xFF, sizeof(slot_owner));
    memset(dirty_lo, 0xFF, sizeof(dirty_lo));
    build_models();
    for (int m = 0; m < M_COUNT; m++) {
        for (int lx = 0; lx < CELL; lx++) {
            for (int ly = 0; ly < CELL; ly++) {
                if (vox_models[m].col[lx][ly].h > model_height[m]) {
                    model_height[m] = vox_models[m].col[lx][ly].h;
                }
            }
        }
    }
}

/* Write one fat pixel at tape position a and step a up one canvas row. */
#define PUT(w) do {                                   \
        uint16_t w_ = (w);                            \
        tape[a] = w_;                                 \
        if (a < GUARD) tape[a + TAPE] = w_;           \
        a -= STRIDE;                                  \
        if (a < 0) a += TAPE;                         \
    } while (0)

#define MAX_COL_ENTS 16  /* creatures and pieces one screen column can hold */
#define ENT_REACH    (ENT_ORIGIN + HOP_MAX + LIFT_MAX)

/* Draw canvas rows ra..rb of world column X into a framebuffer's tape.
 *
 * Walking the column from the near edge of the map to the far edge paints
 * it bottom to top, and `limit` (the lowest row already painted) is the
 * whole occlusion test: every terrain pixel is written exactly once. The
 * walk starts no nearer than the tallest column that could still reach
 * row rb, and stops once row ra is covered.
 *
 * Creatures have gaps, so they cannot join that walk. The ones standing in
 * this column are sorted nearest first, each is given the limit at the
 * moment the walk passes its near edge, which is precisely what the terrain
 * in front of it hides, and they are stamped afterwards, farthest first.
 * The last terrain column of the walk may paint above row ra, so whatever
 * stands in this column is stamped whole, not just rows ra..rb. */
__attribute__((section(".ramtext"), noinline))
static void render_column(uint16_t *tape, int X, int ra, int rb) {
    struct {
        const uint8_t *rows;
        int16_t near, row0, clip;
        uint8_t st, sf, n, flash;
    } in[MAX_COL_ENTS];
    int n_in = 0, noted = 0;

    for (int i = 0; i < n_ents && n_in < MAX_COL_ENTS; i++) {
        const ent_t *e = &ents[i];
        int ex = e->x + e->ox + recoil_x(e), ey = e->y + e->oy;
        if ((unsigned)(X - ex) >= CELL) continue;
        if (ey - ROW_MIN - ENT_REACH > rb) continue;

        int k = n_in++;
        for (; k > 0 && in[k - 1].near < ey + CELL - 1; k--) in[k] = in[k - 1];
        unsigned st = vox_light[(ey + CELL / 2) >> 3][(ex + CELL / 2) >> 3];
        in[k].rows = ent_sprite[e->kind][X - ex];
        in[k].near = (int16_t)(ey + CELL - 1);
        in[k].row0 = (int16_t)(ey - ROW_MIN - ENT_ORIGIN - e->z - lift_under(ex, ey));
        in[k].st = (uint8_t)st;
        in[k].sf = vox_front_shade[st];
        in[k].n = ENT_ROWS;
        in[k].flash = (uint8_t)creature_flash(e);
    }
    for (int i = 0; i < n_parts && n_in < MAX_COL_ENTS; i++) {
        const part_t *p = &parts[i];
        int py = p->y >> 4, row = py - (p->z >> 4) - ROW_MIN;
        if (p->x >> 4 != X || row > rb) continue;

        int k = n_in++;
        for (; k > 0 && in[k - 1].near < py; k--) in[k] = in[k - 1];
        unsigned st = vox_light[py >> 3][X >> 3];
        in[k].rows = &p->pixel;
        in[k].near = (int16_t)py;
        in[k].row0 = (int16_t)row;
        unsigned sf = vox_front_shade[st];
        int dimmed = spatter_dimmed(p);
        in[k].st = (uint8_t)(st - (st && dimmed));
        in[k].sf = (uint8_t)(sf - (sf && dimmed));
        in[k].n = 1;
        in[k].flash = 0;
    }

    int cx = X >> 3, lx = X & 7;
    int r = rb + COL_MAXH;                /* canvas row of ground level at vy */
    if (r > NR - 1) r = NR - 1;
    int vy = r + ROW_MIN;
    int limit = rb + 1;
    int32_t a = rb * STRIDE + X;
    if (a >= TAPE) a -= TAPE;

    /* Nothing the walk will meet stands in front of these. */
    while (noted < n_in && in[noted].near > vy) in[noted++].clip = (int16_t)limit;
    int next = noted < n_in ? in[noted].near : -1;

    while (limit > ra && vy >= 0) {
        int cy = vy >> 3;
        const vmodel_t *model = vox_assembly_model(cx, cy);
        if (!model) model = &vox_models[vox_tile[cy][cx]];
        const vcol_t *col = &model->col[lx][vy & 7];
        unsigned st = vox_light[cy][cx];
        unsigned sf = vox_front_shade[st];
        int stained = vox_stained(cx, cy);

        /* The cell's model stands on a plinth `lift` voxels high. */
        int lift = vox_lift[cy][cx];
        unsigned plinth = vox_plinth[vox_tile[cy][cx]];
        uint16_t plinth_side = vox_wtab[plinth + sf];

        do {
            while (vy == next) {
                in[noted++].clip = (int16_t)limit;
                next = noted < n_in ? in[noted].near : -1;
            }
            int h = col->h + lift;
            int top = r - h;
            if (top < limit) {
                int stop = top < 0 ? -1 : top;
                int n = limit - 1 - stop;
                int z = r - (limit - 1);
                for (; n > 0; n--, z++) {
                    PUT(z < lift ? plinth_side : vox_wtab[col->c[z - lift] + sf]);
                }
                if (top < 0) { limit = 0; break; }
                int painted = stained && col->h == 1 && vox_stain_pixel(lx, vy & 7);
                PUT(col->h ? vox_wtab[(painted ? vox_blood_material : col->c[col->h - 1]) + st]
                           : (h ? vox_wtab[plinth + st] : 0));
                limit = top;
            }
            col--; r--; vy--;
        } while ((vy & 7) != 7);
    }
    while (limit-- > ra) PUT(0);

    while (noted-- > 0) {
        const uint8_t *rows = in[noted].rows;
        int row = in[noted].row0;
        for (int k = 0; k < in[noted].n; k++, row++) {
            unsigned v = rows[k];
            if (!v || row < 0 || row >= in[noted].clip) continue;
            uint16_t w = vox_wtab[(v & ~7u) + ((v & 1) ? in[noted].st : in[noted].sf)];
            if (in[noted].flash) w = vox_wtab[in[noted].flash];
            a = row * STRIDE + X;
            if (a >= TAPE) a -= TAPE;
            tape[a] = w;
            if (a < GUARD) tape[a + TAPE] = w;
        }
    }
}

int vox_prepare(uint16_t *fb, int buf, int cam_x, int cam_y) {
    uint16_t *tape = fb + FB_TAPE;
    int16_t *owner = slot_owner[buf];
    int drawn = 0;

    /* Columns this framebuffer holds that pass through a changed cell:
     * repaint the changed rows if they are on screen, otherwise let go of
     * the column and redraw it whole if the camera ever gets there. */
    for (int cx = 0; cx < MAP_W; cx++) {
        int lo = dirty_lo[buf][cx], hi = dirty_hi[buf][cx];
        if (lo > hi) continue;
        for (int x = cx * CELL; x < (cx + 1) * CELL; x++) {
            int slot = x % STRIDE;
            if (owner[slot] != x) continue;
            if (x < cam_x || x >= cam_x + VIEW_W) {
                owner[slot] = -1;
                continue;
            }
            render_column(tape, x, lo, hi);
            drawn++;
        }
        dirty_lo[buf][cx] = 0xFF;
        dirty_hi[buf][cx] = 0;
    }

    /* What the camera shows has to be there. */
    for (int x = cam_x; x < cam_x + VIEW_W; x++) {
        int slot = x % STRIDE;
        if (owner[slot] != x) {
            render_column(tape, x, 0, NR - 1);
            owner[slot] = (int16_t)x;
            drawn++;
        }
    }

    /* With time to spare, get ahead of the camera on both sides. */
    int spare = AHEAD_PER_FRAME - drawn;
    for (int d = 1; d <= AHEAD_REACH && spare > 0; d++) {
        for (int side = 0; side < 2 && spare > 0; side++) {
            int x = side ? cam_x - d : cam_x + VIEW_W - 1 + d;
            if (x < 0 || x >= MAP_W * CELL) continue;
            int slot = x % STRIDE;
            if (owner[slot] == x) continue;
            render_column(tape, x, 0, NR - 1);
            owner[slot] = (int16_t)x;
            spare--;
        }
    }

    /* Scanlines 2k and 2k+1 both show canvas row cam_y - ROW_MIN + k. */
    int32_t a = (cam_y - ROW_MIN) * STRIDE + cam_x;
    if (a >= TAPE) a -= TAPE;
    for (int k = 0; k < VIEW_H; k++) {
        fb[2 * k] = fb[2 * k + 1] = (uint16_t)(FB_TAPE + a);
        a += STRIDE;
        if (a >= TAPE) a -= TAPE;
    }
    return drawn;
}
