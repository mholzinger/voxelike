#include "mars.h"
#include "text.h"
#include "string.h"

#define SOLID_TILES 96   /* second copy of the font, opaque background */

static uint16_t want[TEXT_H][TEXT_W];
static uint16_t shown[TEXT_H][TEXT_W];
static uint32_t used_rows, dirty_rows;

void text_begin(void) {
	dirty_rows |= used_rows;
	for(int y = 0; y < TEXT_H; y++)
		if(used_rows & (1u << y)) memset(want[y], 0, sizeof want[y]);
	used_rows = 0;
}

void text_put(int x, int y, const char *s, int len, uint16_t color, int solid) {
	if(y < 0 || y >= TEXT_H) return;
	used_rows |= 1u << y;
	dirty_rows |= 1u << y;
	for(int i = 0; s[i] && (len < 0 || i < len) && x + i < TEXT_W; i++) {
		int c = s[i] & 0x7F;
		if(c < 0x20 || c > 0x7E) c = '?';
		if(x + i >= 0)
			want[y][x + i] = (uint16_t)((c - 0x20 + (solid ? SOLID_TILES : 0)) | color);
	}
}

void text_tile(int x, int y, uint16_t tile) {
	if((unsigned)x >= TEXT_W || (unsigned)y >= TEXT_H) return;
	used_rows |= 1u << y;
	dirty_rows |= 1u << y;
	want[y][x] = tile;
}

void text_fill(int x, int y, int width, uint16_t color) {
	if((unsigned)y >= TEXT_H) return;
	int end = x + width;
	if(end > TEXT_W) end = TEXT_W;
	if(x < 0) x = 0;
	if(x >= end) return;
	used_rows |= 1u << y;
	dirty_rows |= 1u << y;
	for(; x < end; x++) want[y][x] = SOLID_TILES | color;
}

void text_box_outline(int x, int y, int width, int height) {
	enum { HFLIP = 0x0800, VFLIP = 0x1000 };
	int right = x + width - 1, bottom = y + height - 1;
	for(int col = x + 1; col < right; col++) {
		text_tile(col, y, TEXT_HUD | HUD_BOX_EDGE);
		text_tile(col, bottom, TEXT_HUD | HUD_BOX_EDGE | VFLIP);
	}
	for(int row = y + 1; row < bottom; row++) {
		text_tile(x, row, TEXT_HUD | HUD_BOX_SIDE);
		text_tile(right, row, TEXT_HUD | HUD_BOX_SIDE | HFLIP);
	}
	text_tile(x, y, TEXT_HUD | HUD_BOX_CORNER);
	text_tile(right, y, TEXT_HUD | HUD_BOX_CORNER | HFLIP);
	text_tile(x, bottom, TEXT_HUD | HUD_BOX_CORNER | VFLIP);
	text_tile(right, bottom, TEXT_HUD | HUD_BOX_CORNER | HFLIP | VFLIP);
}

void text_commit(void) {
	for(int y = 0; y < TEXT_H; y++) {
		if(!(dirty_rows & (1u << y))) continue;
		int at = -1;   /* column the 68K's write pointer sits on, this row */
		for(int x = 0; x < TEXT_W; x++) {
			if(want[y][x] == shown[y][x]) continue;
			if(at != x) HwMdSetOffset(((y << 6) | x) << 1);
			HwMdSetNTable(want[y][x]);
			shown[y][x] = want[y][x];
			at = x + 1;
		}
	}
	dirty_rows = 0;
}
