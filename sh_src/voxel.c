#include "mars.h"
#include "string.h"
#include "voxel.h"

/* --- Materials ------------------------------------------------------
 * CRAM is laid out as material * 8 + shade, shade 7 brightest. Material 0
 * is reserved so index 0 stays the transparent void. */
#define N_SHADES 8
#define MAT(n)   ((n) * N_SHADES)

enum {
    MT_STONE = 1, MT_STONE2, MT_GROUT, MT_BRICKA, MT_BRICKB, MT_CAP, MT_WOOD,
    MT_DIRT, MT_DIRT2, MT_GOLD, MT_STAIR, MT_SHADOW,
    MT_HAIR, MT_SKIN, MT_EYE, MT_TUNIC, MT_BELT, MT_BOOT,
    MT_STEEL, MT_PAPER, MT_POTION, MT_FOOD, MT_MAGIC,
    MT_MON0, MT_MON1, MT_MON2, MT_MON3, MT_MON4, MT_MON5,
    MT_COUNT
};
#define N_MON_MATS 6

static const uint8_t mat_rgb[MT_COUNT][3] = {
    [MT_STONE]  = { 15, 16, 20 }, [MT_STONE2] = { 19, 20, 24 },
    [MT_GROUT]  = {  9, 10, 13 }, [MT_BRICKA] = { 23, 11,  8 },
    [MT_BRICKB] = { 18,  9,  7 }, [MT_CAP]    = { 25, 21, 16 },
    [MT_WOOD]   = { 20, 13,  6 }, [MT_DIRT]   = { 14, 11,  8 },
    [MT_DIRT2]  = { 11,  9,  7 }, [MT_GOLD]   = { 31, 26,  6 },
    [MT_STAIR]  = { 22, 22, 12 }, [MT_SHADOW] = {  4,  3,  6 },
    [MT_HAIR]   = { 21, 10,  3 }, [MT_SKIN]   = { 31, 23, 17 },
    [MT_EYE]    = {  3,  3,  8 }, [MT_TUNIC]  = {  8, 25,  8 },
    [MT_BELT]   = { 14,  8,  3 }, [MT_BOOT]   = { 12,  7,  4 },
    [MT_STEEL]  = { 24, 26, 29 }, [MT_PAPER]  = { 30, 28, 21 },
    [MT_POTION] = {  6, 14, 31 }, [MT_FOOD]   = { 29,  9,  6 },
    [MT_MAGIC]  = { 24,  8, 30 },
    [MT_MON0]   = { 29,  7,  7 }, [MT_MON1]   = { 21,  9, 28 },
    [MT_MON2]   = { 10, 26, 12 }, [MT_MON3]   = { 31, 17,  4 },
    [MT_MON4]   = {  6, 24, 27 }, [MT_MON5]   = { 29, 28,  8 },
};

/* --- Models --------------------------------------------------------- */
#define N_DYN 16   /* hero and monster models, rebuilt on every screen load */

enum {
    M_VOID, M_FLOOR, M_FLOOR2, M_WALL, M_WALL_LOW, M_DOOR_H, M_DOOR_LOW, M_DOOR_V,
    M_PASSAGE,
    M_STAIRS, M_TRAP, M_GOLD, M_POTION, M_SCROLL, M_FOOD, M_WEAPON, M_ARMOR,
    M_RING, M_STICK, M_AMULET, M_MAGIC,
    M_DYN0,
    M_COUNT = M_DYN0 + N_DYN
};

static vmodel_t models[M_COUNT];
static uint8_t  vox_tile[MAP_H][MAP_W];   /* model index per cell */
static uint16_t wtab[256];                /* CRAM index -> doubled pixel word */

uint8_t vox_light[MAP_H][MAP_W];
int vox_hero_cx, vox_hero_cy;

/* A front face is darker than the top face lit by the same light. */
static const uint8_t front_shade[N_SHADES] = { 0, 0, 1, 2, 2, 3, 4, 5 };

/* The hero is a 2D sprite extruded into depth, 3D Dot Game Heroes style.
 * Row 0 is the top of the head. Every column is solid from the ground up
 * because the terrain renderer draws columns, not free voxels. */
#define HERO_H 12
static const char hero_sprite[HERO_H][CELL + 1] = {
    "..hhhh..",
    ".hhhhhh.",
    ".sesses.",
    ".ssssss.",
    ".gggggg.",
    ".gggggg.",
    ".gggggg.",
    ".sbbbbs.",
    ".gggggg.",
    ".gg..gg.",
    ".ll..ll.",
    ".ll..ll.",
};

/* Monsters are Rogue's own letters, extruded. Classic 5x7 glyphs, A to Z,
 * bit 4 the left column; they are emboldened to 6 wide when stacked. */
#define GLYPH_H 7
static const uint8_t glyphs[26][GLYPH_H] = {
    { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   /* A */
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F },
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },   /* Z */
};

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

static uint8_t hero_mat(char ch) {
    switch (ch) {
    case 'h': return MAT(MT_HAIR);
    case 's': return MAT(MT_SKIN);
    case 'e': return MAT(MT_EYE);
    case 'g': return MAT(MT_TUNIC);
    case 'b': return MAT(MT_BELT);
    case 'l': return MAT(MT_BOOT);
    default:  return 0;
    }
}

/* Stack a front-view sprite column on top of whatever the base column is.
 * get(x, z) returns the material at sprite column x, z voxels above its
 * feet, or 0 for empty. Gaps below the topmost voxel fill with shadow. */
static void stack_column(vcol_t *c, int x, int sprite_h, uint8_t (*get)(int, int)) {
    int top = -1;
    for (int z = sprite_h - 1; z >= 0; z--) {
        if (get(x, z)) { top = z; break; }
    }
    if (top < 0 || c->h + top + 1 > COL_MAXH) return;
    for (int z = 0; z <= top; z++) {
        uint8_t m = get(x, z);
        c->c[c->h + z] = m ? m : MAT(MT_SHADOW);
    }
    c->h = (uint8_t)(c->h + top + 1);
}

static uint8_t hero_get(int x, int z) {
    return hero_mat(hero_sprite[HERO_H - 1 - z][x]);
}

static const uint8_t *cur_glyph;
static uint8_t cur_glyph_mat;
static uint8_t glyph_get(int x, int z) {
    unsigned row = cur_glyph[GLYPH_H - 1 - z];
    row = (row << 2) | (row << 1);   /* columns 1..5, emboldened to 1..6 */
    return (row & (0x80 >> x)) ? cur_glyph_mat : 0;
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
    for (int m = M_TRAP; m < M_DYN0; m++) models[m] = models[M_FLOOR];

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
    stack(&models[M_RING], 3, 4, 5, 5, 1, MAT(MT_MON0));

    for (int i = 0; i < 4; i++)
        stack(&models[M_STICK], 2 + i, 2 + i, 5 - i, 5 - i, 1 + i, MAT(MT_WOOD));
    stack(&models[M_STICK], 5, 5, 2, 2, 1, MAT(MT_MAGIC));

    stack(&models[M_AMULET], 3, 4, 3, 4, 2, MAT(MT_GOLD));
    stack(&models[M_AMULET], 3, 4, 3, 4, 1, MAT(MT_MON0));

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
 * itself holds at a cell, seen or not, which is what decides how a door
 * sits in its wall. */
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

/* Torch falloff around the hero. Blocky per-cell light suits the look. */
static void relight(void) {
    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            int dx = abs(cx - vox_hero_cx), dy = abs(cy - vox_hero_cy);
            int d = (dx > dy) ? dx + (dy >> 1) : dy + (dx >> 1);
            int s = (N_SHADES - 1) - (d >> 1);
            vox_light[cy][cx] = (uint8_t)(s < 3 ? 3 : s);
        }
    }
}

/* Rebuild the world from Rogue's screen. Row 0 is its message line and the
 * last row its status line; neither is dungeon. The hero and each visible
 * monster get a model of their own, standing on whatever is under them. */
void vox_load_screen(const uint8_t *cells, vox_under_fn under) {
    int dyn = 0;

    for (int cy = 0; cy < MAP_H; cy++) {
        for (int cx = 0; cx < MAP_W; cx++) {
            int ch = cells[cy * MAP_W + cx] & 0x7F;
            int sprite = (ch == '@' || (ch >= 'A' && ch <= 'Z'));

            if (cy == 0 || cy == MAP_H - 1) {
                vox_tile[cy][cx] = M_VOID;
                continue;
            }
            if (!sprite || dyn == N_DYN) {
                vox_tile[cy][cx] = tile_for(sprite ? under(cx, cy) : ch, cx, cy, under);
                continue;
            }

            vmodel_t *m = &models[M_DYN0 + dyn];
            *m = models[tile_for(under(cx, cy), cx, cy, under)];
            if (ch == '@') {
                vox_hero_cx = cx;
                vox_hero_cy = cy;
                for (int lx = 0; lx < CELL; lx++)
                    for (int ly = 2; ly <= 5; ly++)
                        stack_column(&m->col[lx][ly], lx, HERO_H, hero_get);
            } else {
                cur_glyph = glyphs[ch - 'A'];
                cur_glyph_mat = MAT(MT_MON0 + (ch - 'A') % N_MON_MATS);
                for (int lx = 0; lx < CELL; lx++)
                    for (int ly = 3; ly <= 4; ly++)
                        stack_column(&m->col[lx][ly], lx, GLYPH_H, glyph_get);
            }
            vox_tile[cy][cx] = (uint8_t)(M_DYN0 + dyn++);
        }
    }
    relight();
}

void vox_init(void) {
    volatile uint16_t *cram = &MARS_CRAM;

    for (int i = 0; i < N_SHADES; i++) cram[i] = 0;
    for (int m = 1; m < MT_COUNT; m++) {
        for (int s = 0; s < N_SHADES; s++) {
            int k = s + 2;   /* shade 0 is 2/9 brightness, shade 7 is full */
            cram[MAT(m) + s] = COLOR(mat_rgb[m][0] * k / 9,
                                     mat_rgb[m][1] * k / 9,
                                     mat_rgb[m][2] * k / 9);
        }
    }
    for (int i = 0; i < 256; i++) wtab[i] = (uint16_t)(i * 0x0101);

    build_models();
}

/* Front-to-back column renderer for a fixed oblique camera.
 *
 * A voxel at (x, y, z) lands on fat-pixel column x - cam_x and row
 * (y - cam_y) - z, so there is no per-voxel math: walking a screen column
 * from the near edge of the map to the far edge paints it bottom to top,
 * and `limit` (the lowest row already painted) is the whole occlusion test.
 * Every pixel is written exactly once and nothing is cleared up front. */
__attribute__((section(".ramtext"), noinline))
void vox_render(uint16_t *fb, int cam_x, int cam_y) {
    for (int sx = 0; sx < VIEW_W; sx++) {
        uint16_t *p = fb + (VIEW_H - 1) * 160 + sx;
        int limit = VIEW_H;
        int vx = cam_x + sx;
        int vy = cam_y + VIEW_H - 1 + COL_MAXH;
        if (vy >= MAP_H * CELL) vy = MAP_H * CELL - 1;

        if ((unsigned)vx < MAP_W * CELL && vy >= 0) {
            int cx = vx >> 3, lx = vx & 7;
            int r = vy - cam_y;

            /* void between the map's near edge and the screen bottom */
            while (limit > r + 1 && limit > 0) { *p = 0; p -= 160; limit--; }

            while (limit > 0) {
                int cy = vy >> 3;
                const vcol_t *col = &models[vox_tile[cy][cx]].col[lx][vy & 7];
                unsigned st = vox_light[cy][cx];
                unsigned sf = front_shade[st];
                do {
                    if (r < 0) goto column_done;
                    int h = col->h;
                    int top = r - h;
                    if (top < limit) {
                        int stop = top < 0 ? -1 : top;
                        int n = limit - 1 - stop;
                        int z = r - (limit - 1);
                        while (n-- > 0) {
                            *p = wtab[col->c[z++] + sf];
                            p -= 160;
                        }
                        if (top < 0) { limit = 0; goto column_done; }
                        *p = h ? wtab[col->c[h - 1] + st] : 0;
                        p -= 160;
                        limit = top;
                    }
                    col--; r--; vy--;
                } while ((vy & 7) != 7);
                if (vy < 0) break;
            }
        }
column_done:
        while (limit-- > 0) { *p = 0; p -= 160; }
    }
}
