#ifndef VOXEL_WORLD_H
#define VOXEL_WORLD_H

#include "voxel.h"

/* The world as voxel.c holds it, for another renderer to read. */
#define VOX_MODELS_MAX 33                  /* room for every terrain model */
extern vmodel_t vox_models[];
extern const int vox_model_count;
extern uint8_t vox_tile[MAP_H][MAP_W];     /* index into vox_models */
extern uint8_t vox_light[MAP_H][MAP_W];    /* top-face shade 0..7 */
extern uint8_t vox_lift[MAP_H][MAP_W];     /* voxels of plinth under the model */
extern uint8_t vox_plinth[];               /* plinth material, by model index */
extern uint16_t vox_wtab[256];             /* CRAM index -> doubled pixel word */
extern const uint8_t vox_shadow_material;
extern const uint8_t vox_blood_material;
extern const uint8_t vox_front_shade[8];   /* shade of a side under a top's light */

/* Temporary terrain for the handful of tiles assembling during a step. */
const vmodel_t *vox_assembly_model(int cx, int cy);
int vox_assembly_cell(int i, int *cx, int *cy);

/* One bit per dungeon cell. The stain stays until the next floor. */
int vox_stained(int cx, int cy);

static inline int vox_stain_pixel(int x, int y) {
    int dx = x - CELL / 2, dy = y - CELL / 2;
    return dx * dx + dy * dy + ((x + 2 * y) & 1) <= 6;
}

/* A creature as it is this frame. */
typedef struct {
    int x, y;              /* top left voxel of its footprint */
    int z;                 /* how far off the ground a hop has it */
    uint8_t kind;          /* 0 the hero, 1..26 A..Z */
    uint8_t face;          /* way it looks: 0 south, 1 west, 2 north, 3 east */
    uint8_t attack;        /* countdown for the hero’s equipped attack */
    uint8_t flash;         /* CRAM index it is flashing, or 0 */
} vox_creature_t;

/* The hero's body retreat/lunge and the held weapon's offset and tilt. */
typedef struct {
    int8_t reach, hop;
    int8_t across, forward, lift, tilt;
} vox_attack_pose_t;
const vox_attack_pose_t *vox_attack_pose(int weapon, unsigned remaining);

/* Take a column from a model turned to a creature's facing. */
static inline const vcol_t *vox_model_column(const vcol_t model[CELL][CELL],
                                             int face, int x, int y) {
    switch (face) {
    default: return &model[x][y];
    case 1:  return &model[y][CELL - 1 - x];
    case 2:  return &model[CELL - 1 - x][CELL - 1 - y];
    case 3:  return &model[CELL - 1 - y][x];
    }
}

/* The i'th loose voxel of something that died: where it is, in sixteenths
 * of a voxel, and its CRAM index. Returns 2 for a hero chunk, 1 for a
 * regular fragment, or 0 past the last one. */
int vox_piece(int i, int *x, int *y, int *z, unsigned *colour);

/* The i'th creature on screen. 0 past the last one. */
int vox_creature(int i, vox_creature_t *c);


#endif
