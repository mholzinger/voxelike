#ifndef PERSP_POLY_H
#define PERSP_POLY_H

/* What persp.c and poly.c share: the camera for the frame and the map as
 * the perspective renderers hold it. Nothing else includes this. */
#include <stdint.h>
#include "voxel.h"
#include "voxel_world.h"
#include "sh2zam/sh2zam.h"

#define DETAIL    0xFF

/* One frame's camera, shared by both CPUs. The primary fills it in and
 * raises COMM6; the secondary draws the odd columns, the primary the even
 * ones, and the secondary lowers COMM6 when its share is done. */
typedef struct {
    uint16_t *fb;
    int32_t ex, ey;
    sh2z_fx_t s, c;
    int32_t focal, horizon;
    int32_t eye_h, t_near, t_far;
    /* Past this depth a dim surface is drawn in one colour, not its texture. */
    int32_t texture_depth;
    /* The cells rays may look at: the room the hero is in and a border
     * round it during play, the whole map otherwise. */
    int view_left, view_top, view_right, view_bottom;
    int res;               /* PERSP_RES_x: how finely the picture is drawn */
    int cols, rows;
} persp_job_t;
extern persp_job_t persp_job;

/* All a ray needs to know about a cell, in one cache line. */
typedef struct {
    const vmodel_t *m;
    const uint8_t *top;
    uint8_t flat;          /* its one height, plinth and all, or DETAIL */
    uint8_t light;         /* top-face shade. For an empty cell, how many
                            * cells a ray can cross from here, whichever way
                            * it goes, before it needs to look again */
    uint8_t lift;
    uint8_t plinth;
    uint8_t stain;
    uint8_t creature_index;  /* 1-based index into creature_layers, or 0 */
    uint8_t ground;       /* terrain's own height, before a creature is placed */
    uint8_t distant_top;  /* one colour for the whole top, or 0 for none */
} pcell_t;
typedef char pcell_is_a_line[sizeof(pcell_t) == 16 ? 1 : -1];
extern pcell_t persp_cells[MAP_H][MAP_W];

/* By fine row, focal over how far the row is under the horizon, Q31: what
 * a height under the eye is multiplied by for the depth of level ground
 * that shows on the row. */
extern uint32_t persp_rows[2 * VIEW_H];

/* A creature's voxel columns as it is this frame: the hero in what he
 * wears and holds, anything else as baked. Primary SH-2 only: the hero's
 * is made up in one buffer. */
const vcol_t (*persp_creature_model(const vox_creature_t *c))[CELL];
/* A timing build is drawing this frame without creatures. */
int persp_actors_hidden(void);

#ifdef POLY
/* make POLY=1: the hero's lit room as flat polygons. POLY=2: its floor and
 * the tops of its walls in the bitmaps the rays give them. See poly.c. */
#define POLY_GO 3                          /* COMM6: the secondary's half of a polygon frame */
void poly_init(void);
/* Draw the frame persp_job describes. Zero if it cannot, having drawn
 * nothing, and the rays should. */
int poly_frame(void);
void poly_secondary(void);
#endif

#endif
