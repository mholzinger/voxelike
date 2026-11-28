#ifndef DEATH_STYLE_H
#define DEATH_STYLE_H

typedef enum {
    DEATH_CLAW, DEATH_CUT, DEATH_CRUSH, DEATH_PIERCE,
    DEATH_DRAIN, DEATH_FLAME, DEATH_FROST, DEATH_SPARK,
    DEATH_COLLAPSE, DEATH_SLICES,
    DEATH_BOW_STRIKE, DEATH_ARROW_SHOT, DEATH_ARROW, DEATH_DAGGER,
    DEATH_DART, DEATH_SHURIKEN, DEATH_SPEAR, DEATH_STYLE_COUNT
} death_style_t;

typedef struct {
    death_style_t style;
    int dx, dy; /* direction the blow travels, in dungeon coordinates */
} death_blow_t;

#endif
