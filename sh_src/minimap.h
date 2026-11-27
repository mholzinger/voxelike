#ifndef MINIMAP_H
#define MINIMAP_H

/* A map of the level on the Mega Drive layer, two pixels to a cell, made
 * from what Rogue has on its own screen: Rogue leaves what the hero has
 * seen drawn there. It costs the 32X picture nothing. */

/* Rogue's screen has changed: look at all of it again. */
void minimap_changed(void);

/* Send the 68000 some of the tiles that differ from what is up, a few a
 * frame so that no frame waits long for it. */
void minimap_update(const unsigned char *screen);

/* Put the map in the text layer's top right corner. Call between
 * text_begin and text_commit. */
void minimap_place(void);

#endif
