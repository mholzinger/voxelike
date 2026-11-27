/* Makes sh_src/baked_terrain.h: the terrain models, as the game used to
 * make them in memory every time it started. Run on the machine that
 * builds the ROM; `make` does it.
 *
 *   bake_terrain          the header
 *   bake_terrain --bin    the models alone, as they lie in the ROM
 */
#include <stdio.h>
#include <string.h>
#include "voxel.h"
#include "baked.h"
#include "terrain.h"

static vmodel_t vox_models[M_COUNT];
static uint8_t vox_plinth[M_COUNT];
static uint8_t vox_wall_cap[CELL * CELL];
static uint8_t model_height[M_COUNT];

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
            int speck = ((lx * 5 + ly * 3) ^ (lx * ly)) & 3;

            col_set(&vox_models[M_FLOOR].col[lx][ly], 1,
                    terrain_floor[ly][lx]);
            col_set(&vox_models[M_FLOOR2].col[lx][ly], 1,
                    terrain_floor[ly][(CELL - lx) & (CELL - 1)]);
            col_set(&vox_models[M_PASSAGE].col[lx][ly], 1,
                    speck ? MAT(MT_DIRT) : MAT(MT_DIRT2));

            /* A single brick course and cap on every side of a room, on
             * the half of the cell that is against the room. The other
             * half is left empty: the top of the plinth shows there. */
            vox_wall_cap[lx * CELL + ly] = terrain_wall_cap[ly][lx];
            int south = ly >= CELL / 2, east = lx >= CELL / 2;
            const int in_wall[] = { south, !south, east, !east,
                                    south && east, south && !east, !south && east, !south && !east };
            for (int k = 0; k < 8; k++) {
                if (!in_wall[k]) continue;
                vcol_t *w = &vox_models[M_WALL_S + k].col[lx][ly];
                col_set(w, 2, MAT(MT_BRICKB));
                w->c[1] = terrain_wall_cap[ly][lx];
            }

            /* A doorway is a way through a wall as thick as its cell,
             * which is thicker than the wall either side of it: stone the
             * height of the wall on both hands and a trodden path between,
             * dark where it runs along the foot of the stone. Seen from
             * above it is the mouth of a tunnel. */
            for (int k = 0; k < 2; k++) {
                int across = k ? ly : lx;
                vcol_t *door = &vox_models[M_DOOR_H + k].col[lx][ly];
                if (across < 2 || across >= CELL - 2) {
                    col_set(door, 2, MAT(MT_BRICKB));
                    door->c[1] = terrain_wall_cap[ly][lx];
                } else {
                    col_set(door, 1, across == 2 || across == CELL - 3 ? MAT(MT_SHADOW) : MAT(MT_DIRT2));
                }
            }

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

static void bytes(const char *type, const char *name, const char *size, const uint8_t *data, int n, const char *what) {
    printf("/* %s */\n%s %s[%s] = {", what, type, name, size);
    for (int i = 0; i < n; i++) printf("%s%u,", i % 16 ? " " : "\n    ", data[i]);
    printf("\n};\n\n");
}

int main(int argc, char **argv) {
    build_models();
    for (int m = 0; m < M_COUNT; m++)
        for (int lx = 0; lx < CELL; lx++)
            for (int ly = 0; ly < CELL; ly++)
                if (vox_models[m].col[lx][ly].h > model_height[m]) model_height[m] = vox_models[m].col[lx][ly].h;

    if (argc > 1 && !strcmp(argv[1], "--bin")) {
        fwrite(vox_models, sizeof vox_models, 1, stdout);
        return 0;
    }
    printf("/* Made by tools/bake_terrain.c. Do not edit. */\n\n");
    printf("TERRAIN_KEPT vmodel_t vox_models[M_COUNT] = {\n");
    for (int m = 0; m < M_COUNT; m++) {
        printf("{{ /* %d */\n", m);
        for (int lx = 0; lx < CELL; lx++) {
            printf(" {");
            for (int ly = 0; ly < CELL; ly++) {
                const vcol_t *c = &vox_models[m].col[lx][ly];
                printf("{%u,{", c->h);
                for (int z = 0; z < COL_MAXH; z++) printf("%u%s", c->c[z], z + 1 < COL_MAXH ? "," : "");
                printf("}},");
            }
            printf("},\n");
        }
        printf("}},\n");
    }
    printf("};\n\n");
    bytes("TERRAIN_KEPT uint8_t", "vox_plinth", "M_COUNT", vox_plinth, M_COUNT, "What a cell's plinth is made of, by the model that stands on it.");
    bytes("TERRAIN_KEPT uint8_t", "vox_wall_cap", "CELL * CELL", vox_wall_cap, CELL * CELL, "A wall's cap as a whole cell of it.");
    bytes("static TERRAIN_KEPT uint8_t", "model_height", "M_COUNT", model_height, M_COUNT, "The tallest column of each model.");
    return 0;
}
