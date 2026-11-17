#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

/* Text on the Mega Drive layer, which sits in front of the 32X picture.
 * A screen is composed from scratch each time and committed; only cells
 * that differ from what is already up cost a trip to the 68K. */
#define TEXT_W 40
#define TEXT_H 28

#define TEXT_WHITE  0x2000
#define TEXT_YELLOW 0x4000

void text_begin(void);
/* Writes up to len characters, stopping early at a terminating zero; len < 0
 * means no limit. solid puts the text on an opaque black strip so it reads
 * over the dungeon. Clips to the screen. */
void text_put(int x, int y, const char *s, int len, uint16_t color, int solid);
void text_commit(void);

#endif
