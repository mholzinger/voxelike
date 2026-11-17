#ifndef VOXEL_H
#define VOXEL_H

#include <stdint.h>

/* The dungeon is Rogue's own 80x24 character grid. Each character cell is a
 * CELL x CELL footprint of voxel columns, and the screen is VIEW_W x VIEW_H
 * "fat pixels": one voxel is 2x2 screen pixels. Horizontal doubling is a word
 * write, vertical doubling is free from the 32X line table. */
#define MAP_W      80
#define MAP_H      24
#define CELL       8
#define VIEW_W     160
#define VIEW_H     112
#define COL_MAXH   15

/* One voxel column: h solid voxels, c[z] the material of each (a CRAM base
 * index, shade 0). 16 bytes so a model strip indexes with a shift. */
typedef struct {
    uint8_t h;
    uint8_t c[COL_MAXH];
} vcol_t;

/* col[lx][ly]: the strip for one screen column is contiguous in memory. */
typedef struct {
    vcol_t col[CELL][CELL];
} vmodel_t;

extern uint8_t vox_light[MAP_H][MAP_W];   /* top-face shade 0..7 per cell */
extern int     vox_hero_cx, vox_hero_cy;  /* where the last screen load found '@' */

/* The level's own character at a cell, whatever stands on it. */
typedef int (*vox_under_fn)(int cx, int cy);

void vox_init(void);
void vox_load_screen(const uint8_t *cells, vox_under_fn under);
void vox_render(uint16_t *fb, int cam_x, int cam_y);

#endif
