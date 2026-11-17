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

/* The camera is the world position of the view's top left fat pixel. A voxel
 * at (x, y, z) shows at column x - cam_x, row (y - z) - cam_y. */
#define CAM_X_MIN  0
#define CAM_X_MAX  (MAP_W * CELL - VIEW_W)
#define CAM_Y_MIN  (-16)                       /* room for what stands on row 0 */
#define CAM_Y_MAX  (MAP_H * CELL - VIEW_H)

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

extern int vox_hero_cx, vox_hero_cy;   /* where the last screen load found '@' */
extern int vox_hero_x, vox_hero_y;     /* top left voxel of the hero right now */
extern int vox_hero_busy;              /* the hero is still walking or lunging */
extern int vox_hero_jumped;            /* set when the hero moved without walking */

/* The level's own character at a cell, whatever stands on it. */
typedef int (*vox_under_fn)(int cx, int cy);

/* Who the creature shown at a cell is: the same nonzero number for as long
 * as it lives, or 0 if nobody is really there. Never 1. */
typedef uint32_t (*vox_who_fn)(int cx, int cy);

void vox_init(void);

/* Rebuild the world from Rogue's screen. room is the lit room the hero is
 * in as { x0, y0, x1, y1 } in cells, or NULL in a passage or a dark room.
 * A creature that moved walks to its new cell over `frames` frames. Only
 * what changed is redrawn afterwards. */
void vox_load_screen(const uint8_t *cells, vox_under_fn under, vox_who_fn who,
                     const int *room, int frames);

/* Move every walking creature on by one frame. */
void vox_animate(void);

/* Have the hero jab toward a neighbouring cell and come back. */
void vox_hero_lunge(int dx, int dy);

/* Bring the framebuffer that is about to be shown up to date for a camera
 * position and point its line table there. buf names which of the two
 * framebuffers fb is. Returns how many visible columns had to be drawn. */
int vox_prepare(uint16_t *fb, int buf, int cam_x, int cam_y);

/* Black out or restore the 32X picture without touching the framebuffers. */
void vox_palette(int on);

#endif
