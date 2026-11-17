#ifndef SFX_H
#define SFX_H

/* Sound effects. The 68000 plays them (md_src/sound.c, where the order of
 * this list is repeated); this side only says which. */
enum {
    SFX_STEP, SFX_SWING, SFX_HIT, SFX_HURT, SFX_BURST, SFX_ITEM,
    SFX_GOLD, SFX_STAIRS, SFX_LEVELUP, SFX_BLIP, SFX_DEATH
};

/* Ask for an effect. It starts within a frame. */
void sfx(int id);

/* Pass what was asked for to the 68000. Call once a frame. */
void sfx_flush(void);

#endif
