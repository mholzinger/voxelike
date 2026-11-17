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
    M_VOID, M_FLOOR, M_FLOOR2, M_WALL, M_WALL_LOW, M_DOOR_H, M_DOOR_LOW, M_DOOR_V,
    M_PASSAGE, M_STAIRS, M_TRAP, M_GOLD, M_POTION, M_SCROLL, M_FOOD, M_WEAPON,
    M_ARMOR, M_RING, M_STICK, M_AMULET, M_MAGIC,
    M_COUNT
};

vmodel_t vox_models[M_COUNT];
uint16_t vox_wtab[256];              /* CRAM index -> doubled pixel word */

/* The world as last loaded, one entry per Rogue screen cell. */
uint8_t vox_tile[MAP_H][MAP_W];    /* terrain model */
uint8_t vox_light[MAP_H][MAP_W];   /* top-face shade 0..7 */

int vox_hero_cx, vox_hero_cy;
int vox_hero_x, vox_hero_y;
int vox_hero_busy, vox_hero_jumped;

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
#define MAX_PARTS  96
#define PART_LIFE  28
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
    uint8_t wait;         /* frames until the lunge starts */
    uint8_t flash_wait;   /* frames until the flash starts */
    uint8_t flash;        /* frames of flash left */
    int16_t hp;           /* hit points when last looked at */
} ent_t;

static ent_t ents[MAX_ENTS];
static int n_ents;

/* A loose voxel, in sixteenths: what a creature leaves when it dies. */
typedef struct {
    int16_t x, y, z;
    int8_t vx, vy, vz;
    uint8_t life;
    uint8_t pixel;        /* as in a creature sprite: material and face */
} part_t;

static part_t parts[MAX_PARTS];
static int n_parts;
static int last_depth;

static const int8_t hop_z[8] = { 0, 1, 2, 2, 2, 2, 1, 1 };
static const int8_t lunge_reach[LUNGE + 1] = { 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 2 };

#define LIGHT_BRIGHT  7    /* the hero's room, and right around the hero */
#define LIGHT_NEAR    5
#define LIGHT_DIM     3    /* seen before, not in view now */

/* A front face is darker than the top face lit by the same light. */
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

static void build_models(void) {
    memset(vox_models, 0, sizeof(vox_models));

    for (int lx = 0; lx < CELL; lx++) {
        for (int ly = 0; ly < CELL; ly++) {
            int edge = (lx == 0 || ly == 0);
            int speck = ((lx * 5 + ly * 3) ^ (lx * ly)) & 3;

            col_set(&vox_models[M_FLOOR].col[lx][ly], 1,
                    edge ? MAT(MT_GROUT) : MAT(MT_STONE));
            col_set(&vox_models[M_FLOOR2].col[lx][ly], 1,
                    edge ? MAT(MT_GROUT) : MAT(MT_STONE2));
            col_set(&vox_models[M_PASSAGE].col[lx][ly], 1,
                    speck ? MAT(MT_DIRT) : MAT(MT_DIRT2));

            /* Brick courses two voxels tall, staggered, under a cap stone. */
            vcol_t *w = &vox_models[M_WALL].col[lx][ly];
            w->h = 8;
            for (int z = 0; z < 8; z++) {
                int course = z >> 1;
                int brick = (lx + ((course & 1) ? 2 : 0)) >> 2;
                w->c[z] = ((brick + course) & 1) ? MAT(MT_BRICKA) : MAT(MT_BRICKB);
            }
            w->c[7] = MAT(MT_CAP);

            /* A room's near wall is cut down so it cannot hide the row of
             * floor behind it from this camera. */
            vcol_t *wl = &vox_models[M_WALL_LOW].col[lx][ly];
            col_set(wl, 2, MAT(MT_BRICKB));
            wl->c[1] = MAT(MT_CAP);

            /* Door in a horizontal wall: posts left and right. */
            vcol_t *dh = &vox_models[M_DOOR_H].col[lx][ly];
            col_set(dh, 1, MAT(MT_WOOD));
            if (lx == 0 || lx == CELL - 1) col_set(dh, 10, MAT(MT_CAP));

            vcol_t *dl = &vox_models[M_DOOR_LOW].col[lx][ly];
            col_set(dl, 1, MAT(MT_WOOD));
            if (lx == 0 || lx == CELL - 1) col_set(dl, 3, MAT(MT_CAP));

            /* Door in a vertical wall: a tall back post and a low front
             * stub, so the opening stays visible from this camera. */
            vcol_t *dv = &vox_models[M_DOOR_V].col[lx][ly];
            col_set(dv, 1, MAT(MT_WOOD));
            if (ly == 0) col_set(dv, 10, MAT(MT_CAP));
            if (ly == CELL - 1) col_set(dv, 3, MAT(MT_CAP));

            int step = lx >> 1;
            col_set(&vox_models[M_STAIRS].col[lx][ly], 1 + step,
                    (step & 1) ? MAT(MT_STAIR) : MAT(MT_CAP));
        }
    }

    /* Things lying on the floor. */
    for (int m = M_TRAP; m < M_COUNT; m++) vox_models[m] = vox_models[M_FLOOR];

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
}

static int is_wall(int ch) {
    return ch == '-' || ch == '|';
}

/* True when the cell to the north is inside a room, which makes this the
 * room's near (south) wall. */
static int room_to_north(int cx, int cy, vox_under_fn under) {
    int n = under(cx, cy - 1);
    return n == '|' || (n != ' ' && n != '#' && n != '-' && n != '+');
}

/* Model for a terrain or item character. under() answers what the level
 * itself holds at a cell, seen or not, which is what decides how a wall or
 * door sits. */
static uint8_t tile_for(int ch, int cx, int cy, vox_under_fn under) {
    switch (ch) {
    case '.': return ((cx + cy) & 1) ? M_FLOOR2 : M_FLOOR;
    case '|': return M_WALL;
    case '-': return room_to_north(cx, cy, under) ? M_WALL_LOW : M_WALL;
    case '+':
        if (!is_wall(under(cx - 1, cy)) && !is_wall(under(cx + 1, cy))) return M_DOOR_V;
        return room_to_north(cx, cy, under) ? M_DOOR_LOW : M_DOOR_H;
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

/* The patch of canvas a creature covers where it stands right now. */
static void stale_ent(const ent_t *e) {
    int x = e->x + e->ox;
    int r = e->y + e->oy - ROW_MIN;
    stale(x, x + CELL - 1, r - ENT_ORIGIN - e->z, r + CELL - 1);
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
        e->tx = (int16_t)tx;
        e->ty = (int16_t)ty;
        if (frames > 0 && dx <= SLIDE_MAX * CELL && dy <= SLIDE_MAX * CELL) {
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

/* Turn a creature into loose voxels where it stands: a sample of its
 * sprite, thrown up and outward. */
static void shatter(const ent_t *e) {
    int ex = e->x + e->ox, ey = e->y + e->oy;
    int total = 0, skip, n = 0;

    for (int lx = 0; lx < CELL; lx++) {
        for (int k = 0; k < ENT_ROWS; k++) total += ent_sprite[e->kind][lx][k] != 0;
    }
    skip = total / 40 + 1;

    for (int lx = 0; lx < CELL; lx++) {
        for (int k = 0; k < ENT_ROWS; k++) {
            unsigned v = ent_sprite[e->kind][lx][k];
            if (!v || n++ % skip || n_parts == MAX_PARTS) continue;

            /* The sprite is flat; give the pixel a place in the middle of
             * the footprint at the height that puts it on the same row. */
            int y = ey + CELL / 2, z = CELL / 2 + ENT_ORIGIN - k;
            if (z < 0) { y -= z; z = 0; }

            part_t *p = &parts[n_parts++];
            p->x = (int16_t)((ex + lx) << 4);
            p->y = (int16_t)(y << 4);
            p->z = (int16_t)(z << 4);
            p->vx = (int8_t)((lx * 2 - 7) * 2 + (int)(chance() & 7) - 4);
            p->vy = (int8_t)((int)(chance() & 15) - 8);
            p->vz = (int8_t)(16 + (chance() & 15));
            p->life = PART_LIFE;
            p->pixel = (uint8_t)v;
            stale_part(p);
        }
    }
}

static void parts_animate(void) {
    for (int i = 0; i < n_parts; ) {
        part_t *p = &parts[i];
        stale_part(p);
        if (p->z > 0 || p->vz > 0) {
            p->x += p->vx;
            p->y += p->vy;
            p->z += p->vz;
            p->vz -= GRAVITY;
            if (p->z <= 0) {
                p->z = 0;
                p->vz = 0;
                if (p->life > PART_REST) p->life = PART_REST;
            }
        }
        if (--p->life == 0 || p->x < 0 || p->x >= (MAP_W * CELL) << 4 ||
            p->y < CELL << 4 || p->y >= ((MAP_H - 1) * CELL) << 4) {
            *p = parts[--n_parts];
            continue;
        }
        stale_part(p);
        i++;
    }
}

static void hero_follow(void) {
    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id != HERO_ID) continue;
        vox_hero_x = ents[i].x;
        vox_hero_y = ents[i].y;
        vox_hero_busy = ents[i].left || ents[i].lunge;
        return;
    }
    vox_hero_busy = 0;
}

void vox_animate(void) {
    for (int i = 0; i < n_ents; i++) {
        ent_t *e = &ents[i];
        if (!e->left && !e->lunge && !e->flash) continue;

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
            e->ox = (int8_t)(e->ldx * lunge_reach[e->lunge]);
            e->oy = (int8_t)(e->ldy * lunge_reach[e->lunge]);
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
    /* If the hero was just hurt, this is the blow that did it. */
    if (hero->flash) hero->flash_wait = JAB_WAIT + 2;
}

int vox_creature(int i, int *cx, int *cy, int *kind) {
    if (i >= n_ents) return 0;
    *cx = ents[i].tx / CELL;
    *cy = ents[i].ty / CELL;
    *kind = ents[i].kind;
    return 1;
}

/* The hero is known; anything else is the material its sprite uses most. */
uint8_t vox_creature_colour(int kind, int z) {
    static const uint8_t hero[10] = {
        MAT(MT_BOOT), MAT(MT_BOOT), MAT(MT_TUNIC), MAT(MT_TUNIC), MAT(MT_BELT),
        MAT(MT_TUNIC), MAT(MT_TUNIC), MAT(MT_SKIN), MAT(MT_SKIN), MAT(MT_HAIR),
    };
    static uint8_t main_mat[N_ENTS];

    if (kind == 0) return hero[z];
    if (!main_mat[kind]) {
        uint8_t count[MT_COUNT] = { 0 };
        int best = 1;
        for (int lx = 0; lx < CELL; lx++) {
            for (int k = 0; k < ENT_ROWS; k++) count[ent_sprite[kind][lx][k] >> 3]++;
        }
        for (int m = 1; m < MT_COUNT; m++) if (count[m] > count[best]) best = m;
        main_mat[kind] = (uint8_t)MAT(best);
    }
    return main_mat[kind];
}

void vox_hero_lunge(int dx, int dy) {
    for (int i = 0; i < n_ents; i++) {
        if (ents[i].id != HERO_ID) continue;
        ents[i].lunge = LUNGE;
        ents[i].ldx = (int8_t)dx;
        ents[i].ldy = (int8_t)dy;
        vox_hero_busy = 1;
    }
}

/* Row 0 of Rogue's screen is its message line and the last row its status
 * line; neither is dungeon. */
void vox_load_screen(const uint8_t *cells, const vox_source_t *src,
                     const int *room, int depth, int frames) {
    vox_under_fn under = src->under;
    int same_level = (depth == last_depth);

    last_depth = depth;
    if (!same_level) frames = 0;
    for (int i = 0; i < n_ents; i++) ents[i].seen = 0;

    for (int i = MAP_W; i < (MAP_H - 1) * MAP_W; i++) {
        if ((cells[i] & 0x7F) == '@') {
            vox_hero_cx = i % MAP_W;
            vox_hero_cy = i / MAP_W;
            break;
        }
    }

    for (int cy = 1; cy < MAP_H - 1; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            int ch = cells[cy * MAP_W + cx] & 0x7F;
            int ent = ent_for(ch);
            uint8_t tile = tile_for(ent < 0 ? ch : under(cx, cy), cx, cy, under);

            uint8_t light = LIGHT_DIM;
            if (room && cx >= room[0] && cx <= room[2] && cy >= room[1] && cy <= room[3]) {
                light = LIGHT_BRIGHT;
            } else {
                int dx = abs(cx - vox_hero_cx), dy = abs(cy - vox_hero_cy);
                int d = dx > dy ? dx : dy;
                if (d <= 1) light = LIGHT_BRIGHT;
                else if (d == 2) light = LIGHT_NEAR;
            }

            if (tile != vox_tile[cy][cx] || light != vox_light[cy][cx]) {
                vox_tile[cy][cx] = tile;
                vox_light[cy][cx] = light;
                stale_cell(cx, cy);
            }

            if (ent >= 0) {
                /* A letter nobody answers for is known by where it is. */
                uint32_t id = ent == 0 ? HERO_ID : src->who(cx, cy);
                if (id == 0) id = 0x80000000u | (uint32_t)(cy * MAP_W + cx);
                ent_place(id, ent, cx, cy, frames);
            }
        }
    }

    /* Whatever Rogue no longer shows is gone, in pieces if it died. What
     * is still here and has fewer hit points than before was hurt, and
     * flashes: at once, or for a monster as the hero's blow lands. */
    for (int i = 0; i < n_ents; ) {
        ent_t *e = &ents[i];
        if (!e->seen) {
            stale_ent(e);
            if (same_level && !(e->id & 0x80000000u) && !src->alive(e->id)) shatter(e);
            *e = ents[--n_ents];
            continue;
        }
        if (!(e->id & 0x80000000u)) {
            int hp = src->hp(e->id);
            if (hp < e->hp && same_level) {
                e->flash = FLASH;
                e->flash_wait = (uint8_t)(e->id == HERO_ID ? 0 : 2);
            }
            e->hp = (int16_t)hp;
        }
        i++;
    }
    hero_follow();
}

void vox_palette(int on) {
    volatile uint16_t *cram = &MARS_CRAM;

    for (int i = 0; i < N_SHADES; i++) cram[i] = 0;
    for (int m = 1; m < MT_COUNT; m++) {
        for (int s = 0; s < N_SHADES; s++) {
            int k = on ? s + 2 : 0;   /* shade 0 is 2/9 brightness, shade 7 is full */
            cram[MAT(m) + s] = COLOR(mat_rgb[m][0] * k / 9,
                                     mat_rgb[m][1] * k / 9,
                                     mat_rgb[m][2] * k / 9);
        }
    }
}

void vox_init(void) {
    vox_palette(1);
    for (int i = 0; i < 256; i++) vox_wtab[i] = (uint16_t)(i * 0x0101);
    memset(slot_owner, 0xFF, sizeof(slot_owner));
    memset(dirty_lo, 0xFF, sizeof(dirty_lo));
    build_models();
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
#define FLASH_HERO   (MAT(MT_RED) + 7)
#define FLASH_OTHER  (MAT(MT_PAPER) + 7)
#define ENT_REACH    (ENT_ORIGIN + HOP_MAX)

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
        int ex = e->x + e->ox, ey = e->y + e->oy;
        if ((unsigned)(X - ex) >= CELL) continue;
        if (ey - ROW_MIN - ENT_REACH > rb) continue;

        int k = n_in++;
        for (; k > 0 && in[k - 1].near < ey + CELL - 1; k--) in[k] = in[k - 1];
        unsigned st = vox_light[(ey + CELL / 2) >> 3][(ex + CELL / 2) >> 3];
        in[k].rows = ent_sprite[e->kind][X - ex];
        in[k].near = (int16_t)(ey + CELL - 1);
        in[k].row0 = (int16_t)(ey - ROW_MIN - ENT_ORIGIN - e->z);
        in[k].st = (uint8_t)st;
        in[k].sf = vox_front_shade[st];
        in[k].n = ENT_ROWS;
        in[k].flash = (e->flash && !e->flash_wait)
                    ? (e->id == HERO_ID ? FLASH_HERO : FLASH_OTHER) : 0;
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
        in[k].st = (uint8_t)st;
        in[k].sf = vox_front_shade[st];
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
        const vcol_t *col = &vox_models[vox_tile[cy][cx]].col[lx][vy & 7];
        unsigned st = vox_light[cy][cx];
        unsigned sf = vox_front_shade[st];

        do {
            while (vy == next) {
                in[noted++].clip = (int16_t)limit;
                next = noted < n_in ? in[noted].near : -1;
            }
            int h = col->h;
            int top = r - h;
            if (top < limit) {
                int stop = top < 0 ? -1 : top;
                int n = limit - 1 - stop;
                int z = r - (limit - 1);
                while (n-- > 0) PUT(vox_wtab[col->c[z++] + sf]);
                if (top < 0) { limit = 0; break; }
                PUT(h ? vox_wtab[col->c[h - 1] + st] : 0);
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
