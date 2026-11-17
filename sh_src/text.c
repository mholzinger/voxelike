#include "mars.h"
#include "text.h"

#define SOLID_TILES 96   /* second copy of the font, opaque background */

static uint16_t want[TEXT_H][TEXT_W];
static uint16_t shown[TEXT_H][TEXT_W];

void text_begin(void) {
	for(int y = 0; y < TEXT_H; y++)
		for(int x = 0; x < TEXT_W; x++) want[y][x] = 0;
}

void text_put(int x, int y, const char *s, int len, uint16_t color, int solid) {
	if(y < 0 || y >= TEXT_H) return;
	for(int i = 0; s[i] && (len < 0 || i < len) && x + i < TEXT_W; i++) {
		int c = s[i] & 0x7F;
		if(c < 0x20 || c > 0x7E) c = '?';
		if(x + i >= 0)
			want[y][x + i] = (uint16_t)((c - 0x20 + (solid ? SOLID_TILES : 0)) | color);
	}
}

void text_commit(void) {
	for(int y = 0; y < TEXT_H; y++) {
		int at = -1;   /* column the 68K's write pointer sits on, this row */
		for(int x = 0; x < TEXT_W; x++) {
			if(want[y][x] == shown[y][x]) continue;
			if(at != x) HwMdSetOffset(((y << 6) | x) << 1);
			HwMdSetNTable(want[y][x]);
			shown[y][x] = want[y][x];
			at = x + 1;
		}
	}
}
