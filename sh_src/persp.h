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
#ifdef RENDER_TEST
void persp_test_actors(int visible);
#endif
void persp_stair_hop(int sink, int offset, int hop);
/* Negative hides the hero, zero places him on the floor. */
void persp_arrival_hero(int altitude);
void persp_bestiary(uint16_t *fb, int kind, uint16_t yaw, int lunge);
void persp_bestiary_end(void);

/* Tint the existing floor surface during the hero’s death shot. */
void persp_death_pool(int cx, int cy, unsigned age);

/* Which way the camera looks, in sh2zam turns (0xC000 is north). */
extern uint16_t persp_yaw;
extern int camera_yaw_settled;
uint16_t game_input_yaw(void);  /* the on-screen yaw, including combat framing */
int game_camera_fixed(void);
void game_toggle_camera(void);

/* Draw a whole frame into framebuffer fb. (x, y) is the voxel the camera
 * circles, yaw the direction it looks in sh2zam turns (0xC000 is north),
 * pull sets the eye distance: -200 for the intro dive, -54 in gameplay,
 * 256 all the way out. flat takes
 * the perspective out: 0 is all of it, and at 256, looking north, the
 * picture is the fixed view's. res is one of PERSP_RES_x. */
void persp_render(uint16_t *fb, int x, int y, uint16_t yaw, int pull, int flat, int res);
void persp_render_gameplay(uint16_t *fb, int x, int y, uint16_t yaw, int pull, int flat, int res);

/* The secondary SH-2's side of a frame: wait to be asked, draw its share. */
void persp_secondary(void);

#endif
