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
#define LIFT_MAX   4       /* tallest plinth; the tallest model on one must fit COL_MAXH */

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
extern int vox_hero_armor, vox_hero_weapon;   /* Rogue's numbers for what he wears and wields, -1 for nothing */
extern int vox_hero_x, vox_hero_y;     /* top left voxel of the hero right now */
extern int vox_hero_busy;              /* the hero is still walking or lunging */
extern int vox_hero_jumped;            /* set when the hero moved without walking */
extern int vox_hero_face;              /* way the hero looks: 0 south, 1 west, 2 north, 3 east */
/* What happened since this was last cleared, for whoever makes the noise. */
enum { VOX_STEPPED = 1, VOX_SWUNG = 2, VOX_HIT = 4, VOX_HURT = 8, VOX_BURST = 16 };
extern int vox_events;

extern int vox_moving;                 /* something walked, lunged or flashed this frame */

/* The level's own character at a cell, whatever stands on it. */
typedef int (*vox_under_fn)(int cx, int cy);

/* Who the creature shown at a cell is: the same nonzero number for as long
 * as it lives, or 0 if nobody is really there. Never 1, which is the hero. */
typedef uint32_t (*vox_who_fn)(int cx, int cy);

/* What the renderer asks the game about its world. */
typedef struct {
    vox_under_fn under;
    vox_who_fn who;
    int (*alive)(uint32_t id);   /* is this creature still in the level */
    int (*hp)(uint32_t id);      /* hit points of a living creature, 1 for the hero */
    int (*ground)(int cx, int cy);   /* how high the cell's ground stands, in voxels */
} vox_source_t;

void vox_init(void);

/* Rebuild the world from Rogue's screen. room is the lit room the hero is
 * in as { x0, y0, x1, y1 } in cells, or NULL in a passage or a dark room.
 * depth is the dungeon level, so a new level is not mistaken for a massacre.
 * A creature that moved walks to its new cell over `frames` frames, one that
 * lost hit points flashes, one that died falls to pieces. Only what changed
 * is redrawn afterwards. Returns whether terrain or lighting changed. */
int vox_load_screen(const uint8_t *cells, const vox_source_t *src,
                     const int *room, int depth, int frames, int stage_defeat);

/* Hold one defeated opponent on screen until the combat camera is ready. */
int vox_defeat_target(int *x, int *y);
void vox_defeat_flash(void);
void vox_release_defeat(void);

/* Camera target for a landed melee hit while the opponent is nearby. */
int vox_duel_target(int *x, int *y);

/* A creature struck at the hero: have it jab that way, a moment from now. */
void vox_jab(uint32_t id);

/* Move every walking creature on by one frame. */
void vox_animate(void);
int vox_particles_active(void);

/* Have the hero jab toward a neighbouring cell and come back. */
void vox_hero_lunge(int dx, int dy);

/* Clear or start the hero's hit flash. */
void vox_hero_hit_flash(int on);

/* Shed model-coloured chunks and a separate spray. A flat build uses the
 * hero sprite in place of the equipped 3D model. */
void vox_hero_shatter(const vcol_t model[CELL][CELL]);

/* Bring the framebuffer that is about to be shown up to date for a camera
 * position and point its line table there. buf names which of the two
 * framebuffers fb is. Returns how many visible columns had to be drawn. */
int vox_prepare(uint16_t *fb, int buf, int cam_x, int cam_y);

/* How high whatever stands in a cell reaches, in voxels: 0 for nothing. */
int vox_height_at(int cx, int cy);

/* Something else drew on the framebuffers: nothing in them can be kept. */
void vox_forget(void);

/* Light the 32X picture from black to full brightness. */
#define VOX_FULL_LIGHT 16
void vox_palette(int light);

#endif
