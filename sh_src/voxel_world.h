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

/* The i'th creature on screen: its cell and kind. 0 past the last one. */
int vox_creature(int i, int *cx, int *cy, int *kind);

/* Material (CRAM base) of a creature kind at height z of a stand-in block. */
uint8_t vox_creature_colour(int kind, int z);

#endif
