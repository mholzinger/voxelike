#include "mars.h"
#include "string.h"
#include "voxel.h"
#include "baked.h"

/* --- Materials ------------------------------------------------------
 * CRAM is laid out as material * 8 + shade, shade 7 brightest. Material 0
 * is reserved so index 0 stays the transparent void. The materials
 * themselves come from models/materials.txt by way of baked.h. */
#define N_SHADES 8
#define MAT(n)   ((n) * N_SHADES)

/* --- Terrain models ------------------------------------------------- */
enum {
    M_VOID, M_FLOOR, M_FLOOR2, M_WALL, M_WALL_LOW, M_DOOR_H, M_DOOR_LOW, M_DOOR_V,
    M_PASSAGE, M_STAIRS, M_TRAP, M_GOLD, M_POTION, M_SCROLL, M_FOOD, M_WEAPON,
    M_ARMOR, M_RING, M_STICK, M_AMULET, M_MAGIC,
    M_COUNT
};

static vmodel_t models[M_COUNT];
static uint16_t wtab[256];                /* CRAM index -> doubled pixel word */

/* The world as last loaded, one entry per Rogue screen cell. */
static uint8_t vox_tile[MAP_H][MAP_W];    /* terrain model */
static uint8_t vox_ent[MAP_H][MAP_W];     /* creature + 1, 0 for none */
static uint8_t vox_light[MAP_H][MAP_W];   /* top-face shade 0..7 */

int vox_hero_cx, vox_hero_cy;

#define LIGHT_BRIGHT  7    /* the hero's room, and right around the hero */
#define LIGHT_NEAR    5
#define LIGHT_DIM     3    /* seen before, not in view now */

/* A front face is darker than the top face lit by the same light. */
static const uint8_t front_shade[N_SHADES] = { 0, 0, 1, 2, 2, 3, 4, 5 };

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
    memset(models, 0, sizeof(models));

    for (int lx = 0; lx < CELL; lx++) {
        for (int ly = 0; ly < CELL; ly++) {
            int edge = (lx == 0 || ly == 0);
            int speck = ((lx * 5 + ly * 3) ^ (lx * ly)) & 3;

            col_set(&models[M_FLOOR].col[lx][ly], 1,
                    edge ? MAT(MT_GROUT) : MAT(MT_STONE));
            col_set(&models[M_FLOOR2].col[lx][ly], 1,
                    edge ? MAT(MT_GROUT) : MAT(MT_STONE2));
            col_set(&models[M_PASSAGE].col[lx][ly], 1,
                    speck ? MAT(MT_DIRT) : MAT(MT_DIRT2));

            /* Brick courses two voxels tall, staggered, under a cap stone. */
            vcol_t *w = &models[M_WALL].col[lx][ly];
            w->h = 8;
            for (int z = 0; z < 8; z++) {
                int course = z >> 1;
                int brick = (lx + ((course & 1) ? 2 : 0)) >> 2;
                w->c[z] = ((brick + course) & 1) ? MAT(MT_BRICKA) : MAT(MT_BRICKB);
            }
            w->c[7] = MAT(MT_CAP);

            /* A room's near wall is cut down so it cannot hide the row of
             * floor behind it from this camera. */
            vcol_t *wl = &models[M_WALL_LOW].col[lx][ly];
            col_set(wl, 2, MAT(MT_BRICKB));
            wl->c[1] = MAT(MT_CAP);

            /* Door in a horizontal wall: posts left and right. */
            vcol_t *dh = &models[M_DOOR_H].col[lx][ly];
            col_set(dh, 1, MAT(MT_WOOD));
            if (lx == 0 || lx == CELL - 1) col_set(dh, 10, MAT(MT_CAP));

            vcol_t *dl = &models[M_DOOR_LOW].col[lx][ly];
            col_set(dl, 1, MAT(MT_WOOD));
            if (lx == 0 || lx == CELL - 1) col_set(dl, 3, MAT(MT_CAP));

            /* Door in a vertical wall: a tall back post and a low front
             * stub, so the opening stays visible from this camera. */
            vcol_t *dv = &models[M_DOOR_V].col[lx][ly];
            col_set(dv, 1, MAT(MT_WOOD));
            if (ly == 0) col_set(dv, 10, MAT(MT_CAP));
            if (ly == CELL - 1) col_set(dv, 3, MAT(MT_CAP));

            int step = lx >> 1;
            col_set(&models[M_STAIRS].col[lx][ly], 1 + step,
                    (step & 1) ? MAT(MT_STAIR) : MAT(MT_CAP));
        }
    }

    /* Things lying on the floor. */
    for (int m = M_TRAP; m < M_COUNT; m++) models[m] = models[M_FLOOR];

    for (int i = 0; i < 4; i++)
        stack(&models[M_TRAP], 2 + (i & 1) * 3, 2 + (i & 1) * 3,
              2 + (i >> 1) * 3, 2 + (i >> 1) * 3, 2, MAT(MT_STEEL));

    stack(&models[M_GOLD], 2, 5, 2, 5, 1, MAT(MT_GOLD));
    stack(&models[M_GOLD], 3, 4, 3, 4, 2, MAT(MT_GOLD));

    stack(&models[M_POTION], 2, 5, 3, 4, 2, MAT(MT_POTION));
    stack(&models[M_POTION], 3, 4, 3, 4, 1, MAT(MT_POTION));
    stack(&models[M_POTION], 3, 4, 3, 4, 1, MAT(MT_CAP));

    stack(&models[M_SCROLL], 2, 5, 3, 4, 1, MAT(MT_PAPER));
    stack(&models[M_SCROLL], 2, 2, 3, 4, 1, MAT(MT_PAPER));
    stack(&models[M_SCROLL], 5, 5, 3, 4, 1, MAT(MT_PAPER));

    stack(&models[M_FOOD], 2, 5, 2, 5, 1, MAT(MT_FOOD));
    stack(&models[M_FOOD], 3, 4, 3, 4, 1, MAT(MT_FOOD));
    stack(&models[M_FOOD], 3, 3, 3, 3, 1, MAT(MT_TUNIC));

    stack(&models[M_WEAPON], 2, 5, 3, 3, 1, MAT(MT_WOOD));
    stack(&models[M_WEAPON], 3, 4, 3, 3, 5, MAT(MT_STEEL));

    stack(&models[M_ARMOR], 2, 5, 3, 4, 3, MAT(MT_STEEL));
    stack(&models[M_ARMOR], 3, 4, 3, 4, 1, MAT(MT_STEEL));

    stack(&models[M_RING], 2, 5, 2, 2, 1, MAT(MT_GOLD));
    stack(&models[M_RING], 2, 5, 5, 5, 1, MAT(MT_GOLD));
    stack(&models[M_RING], 2, 2, 3, 4, 1, MAT(MT_GOLD));
    stack(&models[M_RING], 5, 5, 3, 4, 1, MAT(MT_GOLD));
    stack(&models[M_RING], 3, 4, 5, 5, 1, MAT(MT_RED));

    for (int i = 0; i < 4; i++)
        stack(&models[M_STICK], 2 + i, 2 + i, 5 - i, 5 - i, 1 + i, MAT(MT_WOOD));
    stack(&models[M_STICK], 5, 5, 2, 2, 1, MAT(MT_MAGIC));

    stack(&models[M_AMULET], 3, 4, 3, 4, 2, MAT(MT_GOLD));
    stack(&models[M_AMULET], 3, 4, 3, 4, 1, MAT(MT_RED));

    stack(&models[M_MAGIC], 3, 4, 3, 4, 3, MAT(MT_MAGIC));
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

/* Everything in a cell lands on its own eight screen columns, no higher
 * than the tallest thing that can stand in it and no lower than its near
 * edge. A changed cell stales exactly that patch, in both framebuffers. */
static void stale_cell(int cx, int cy) {
    int near = cy * CELL - ROW_MIN + CELL - 1;
    int far = near - (CELL - 1) - COL_MAXH;
    if (far < 0) far = 0;
    for (int buf = 0; buf < 2; buf++) {
        if (far < dirty_lo[buf][cx]) dirty_lo[buf][cx] = (uint8_t)far;
        if (near > dirty_hi[buf][cx]) dirty_hi[buf][cx] = (uint8_t)near;
    }
}

/* Row 0 of Rogue's screen is its message line and the last row its status
 * line; neither is dungeon. */
void vox_load_screen(const uint8_t *cells, vox_under_fn under, const int *room) {
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

            if (tile != vox_tile[cy][cx] || ent + 1 != vox_ent[cy][cx] ||
                light != vox_light[cy][cx]) {
                vox_tile[cy][cx] = tile;
                vox_ent[cy][cx] = (uint8_t)(ent + 1);
                vox_light[cy][cx] = light;
                stale_cell(cx, cy);
            }
        }
    }
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
    for (int i = 0; i < 256; i++) wtab[i] = (uint16_t)(i * 0x0101);
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

#define MAX_COL_ENTS 8   /* creatures one screen column can pass through */

/* Draw canvas rows ra..rb of world column X into a framebuffer's tape.
 *
 * Walking the column from the near edge of the map to the far edge paints
 * it bottom to top, and `limit` (the lowest row already painted) is the
 * whole occlusion test: every terrain pixel is written exactly once. The
 * walk starts no nearer than the tallest column that could still reach
 * row rb, and stops once row ra is covered.
 *
 * Creatures have gaps, so they cannot join that walk. Each one met on the
 * way is noted with the limit at that moment, which is precisely what the
 * terrain in front of it hides, and stamped afterwards, farthest first. */
__attribute__((section(".ramtext"), noinline))
static void render_column(uint16_t *tape, int X, int ra, int rb) {
    struct {
        const uint8_t *rows;
        int16_t r0, clip;
        uint8_t st, sf;
    } ents[MAX_COL_ENTS];
    int n_ents = 0;

    int cx = X >> 3, lx = X & 7;
    int r = rb + COL_MAXH;                /* canvas row of ground level at vy */
    if (r > NR - 1) r = NR - 1;
    int vy = r + ROW_MIN;
    int limit = rb + 1;
    int32_t a = rb * STRIDE + X;
    if (a >= TAPE) a -= TAPE;

    while (limit > ra && vy >= 0) {
        int cy = vy >> 3;
        const vcol_t *col = &models[vox_tile[cy][cx]].col[lx][vy & 7];
        unsigned st = vox_light[cy][cx];
        unsigned sf = front_shade[st];

        if (vox_ent[cy][cx] && n_ents < MAX_COL_ENTS) {
            ents[n_ents].rows = ent_sprite[vox_ent[cy][cx] - 1][lx];
            ents[n_ents].r0 = (int16_t)(cy * CELL - ROW_MIN);
            ents[n_ents].clip = (int16_t)limit;
            ents[n_ents].st = (uint8_t)st;
            ents[n_ents].sf = (uint8_t)sf;
            n_ents++;
        }

        do {
            int h = col->h;
            int top = r - h;
            if (top < limit) {
                int stop = top < 0 ? -1 : top;
                int n = limit - 1 - stop;
                int z = r - (limit - 1);
                while (n-- > 0) PUT(wtab[col->c[z++] + sf]);
                if (top < 0) { limit = 0; break; }
                PUT(h ? wtab[col->c[h - 1] + st] : 0);
                limit = top;
            }
            col--; r--; vy--;
        } while ((vy & 7) != 7);
    }
    while (limit-- > ra) PUT(0);

    while (n_ents-- > 0) {
        const uint8_t *rows = ents[n_ents].rows;
        int row = ents[n_ents].r0 - ENT_ORIGIN;
        for (int k = 0; k < ENT_ROWS; k++, row++) {
            unsigned v = rows[k];
            if (!v || row < 0 || row >= ents[n_ents].clip) continue;
            uint16_t w = wtab[(v & ~7u) + ((v & 1) ? ents[n_ents].st : ents[n_ents].sf)];
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
