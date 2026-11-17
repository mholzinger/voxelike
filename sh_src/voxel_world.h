#ifndef VOXEL_WORLD_H
#define VOXEL_WORLD_H

#include "voxel.h"

/* The world as voxel.c holds it, for another renderer to read. */
#define VOX_MODELS_MAX 32                  /* room for every terrain model */
extern vmodel_t vox_models[];
extern const int vox_model_count;
extern uint8_t vox_tile[MAP_H][MAP_W];     /* index into vox_models */
extern uint8_t vox_light[MAP_H][MAP_W];    /* top-face shade 0..7 */
extern uint8_t vox_lift[MAP_H][MAP_W];     /* voxels of plinth under the model */
extern uint8_t vox_plinth[];               /* plinth material, by model index */
extern uint16_t vox_wtab[256];             /* CRAM index -> doubled pixel word */
extern const uint8_t vox_front_shade[8];   /* shade of a side under a top's light */

/* A creature as it is this frame. */
typedef struct {
    int x, y;              /* top left voxel of its footprint */
    int z;                 /* how far off the ground a hop has it */
    uint8_t kind;          /* 0 the hero, 1..26 A..Z */
    uint8_t face;          /* way it looks: 0 south, 1 west, 2 north, 3 east */
    uint8_t flash;         /* CRAM index it is flashing, or 0 */
} vox_creature_t;

/* The i'th creature on screen. 0 past the last one. */
int vox_creature(int i, vox_creature_t *c);


#endif
