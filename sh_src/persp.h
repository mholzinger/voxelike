#ifndef PERSP_H
#define PERSP_H

#include <stdint.h>

/* The perspective renderer: a prototype, in PERSP builds only. The same
 * world as voxel.c draws, seen through a camera that orbits the hero. */
void persp_init(void);

/* The world changed (a screen was loaded): take it in again. */
void persp_sync(void);

/* Creatures moved: stand them where they are now. */
void persp_creatures(void);

/* Which way the camera looks, in sh2zam turns (0xC000 is north). */
extern uint16_t persp_yaw;

/* Draw a whole frame into framebuffer fb. (x, y) is the voxel the camera
 * circles, yaw the direction it looks in sh2zam turns (0xC000 is north),
 * pull how far it stands back: 0 close in, 256 all the way out. flat takes
 * the perspective out: 0 is all of it, and at 256, looking north, the
 * picture is the fixed view's. */
void persp_render(uint16_t *fb, int x, int y, uint16_t yaw, int pull, int flat);

/* The secondary SH-2's side of a frame: wait to be asked, draw its share. */
void persp_secondary(void);

#endif
