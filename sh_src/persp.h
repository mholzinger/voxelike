#ifndef PERSP_H
#define PERSP_H

#include <stdint.h>

/* How finely the perspective picture is drawn. A voxel at the hero covers
 * the same 2x2 screen pixels in all of them; what changes is how many
 * pixels are worked out and how many are copies. */
enum {
    PERSP_RES_COARSE,      /* 160x112: every pixel worked out stands for 2x2 */
    PERSP_RES_WIDE,        /* 320x112: for 1x2 */
    PERSP_RES_FINE,        /* 320x224: for itself */
    PERSP_RES_COUNT
};

/* The perspective renderer: a prototype, in PERSP builds only. The same
 * world as voxel.c draws, seen through a camera that orbits the hero. */
void persp_init(void);

/* The world changed (a screen was loaded): take it in again. */
void persp_sync(int terrain_changed);

/* Height of the equipped hero model in voxel layers. */
int persp_hero_height(void);
void persp_hero_shatter(void);

/* Restamp the creatures. hero_buried crops that many layers from the
 * hero's feet, raising the head out of the floor as it counts down to zero. */
void persp_creatures(int hero_buried);

/* Which way the camera looks, in sh2zam turns (0xC000 is north). */
extern uint16_t persp_yaw;
extern int camera_yaw_settled;

/* Draw a whole frame into framebuffer fb. (x, y) is the voxel the camera
 * circles, yaw the direction it looks in sh2zam turns (0xC000 is north),
 * pull sets the eye distance: -200 for the intro dive, -54 in gameplay,
 * 256 all the way out. flat takes
 * the perspective out: 0 is all of it, and at 256, looking north, the
 * picture is the fixed view's. res is one of PERSP_RES_x. */
void persp_render(uint16_t *fb, int x, int y, uint16_t yaw, int pull, int flat, int res);

/* The secondary SH-2's side of a frame: wait to be asked, draw its share. */
void persp_secondary(void);

#endif
