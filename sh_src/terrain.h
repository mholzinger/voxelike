#ifndef TERRAIN_H
#define TERRAIN_H

/* What voxel.c and tools/bake_terrain.c share: the terrain models by
 * name. The models themselves are made by the tool when the ROM is built,
 * and are in the ROM, not in memory: they are a kilobyte each and never
 * change. */

#define N_SHADES 8
#define MAT(n)   ((n) * N_SHADES)

/* --- Terrain vox_models ------------------------------------------------- */
enum {
    M_VOID, M_FLOOR, M_FLOOR2,
    /* A wall is half a cell thick and stands against its room: named for
     * the side of its cell it is on, or for the corner of it. */
    M_WALL_S, M_WALL_N, M_WALL_E, M_WALL_W,
    M_WALL_SE, M_WALL_SW, M_WALL_NE, M_WALL_NW,
    M_DOOR_H, M_DOOR_V,
    M_PASSAGE, M_STAIRS, M_TRAP, M_GOLD, M_POTION, M_SCROLL, M_FOOD, M_WEAPON,
    M_ARMOR, M_RING, M_STICK, M_AMULET, M_MAGIC,
    M_ROCK_EDGE, M_ROCK_CORNER = M_ROCK_EDGE + 4,
    M_ROCK_JOIN = M_ROCK_CORNER + 4,
    M_COUNT = M_ROCK_JOIN + 4
};

/* How high the ground of each cell stands, from the game, taken in once per
 * level. Rooms stand on plinths of different heights and passages run
 * between them at ground level. */
#define STAIR_DROP  2      /* the stairs' cell is this much lower than its room */

#endif
