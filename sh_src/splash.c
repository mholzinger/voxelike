#include "mars.h"
#include "splash.h"
#include "splash_frames.h"

enum { SPLASH_W = 160, SPLASH_HEIGHT = 112, FB_PIXELS = 0x100 };

unsigned splash_title_frame(void) {
    return SPLASH_TITLE_FRAME;
}

unsigned splash_last_frame(void) {
    return sizeof splash_offsets / sizeof splash_offsets[0] - 1;
}

void splash_palette_init(void) {
    volatile uint16_t *cram = &MARS_CRAM;
    for (unsigned i = 0; i < sizeof splash_palette / sizeof splash_palette[0]; i++)
        cram[i] = splash_palette[i];
}

void splash_draw(unsigned frame) {
    volatile uint16_t *fb = &MARS_FRAMEBUFFER;
    for (unsigned y = 0; y < SPLASH_HEIGHT; y++) {
        fb[2 * y] = FB_PIXELS + SPLASH_W * y;
        fb[2 * y + 1] = fb[2 * y];
    }

    // A word writes both horizontal pixels. The line table doubles the rows.
    volatile uint32_t *clear = (volatile uint32_t *)(fb + FB_PIXELS);
    for (unsigned i = 0; i < SPLASH_W * SPLASH_HEIGHT / 2; i++)
        clear[i] = 0;

    const uint8_t *run = splash_runs + splash_offsets[frame];
    while (*run != 255) {
        unsigned y = *run++;
        unsigned x = *run++;
        unsigned packed = *run++;
        uint16_t color = (packed & 7) * 0x0101;
        volatile uint16_t *pixel = fb + FB_PIXELS + y * SPLASH_W + x;
        for (unsigned i = 0; i <= (packed >> 3); i++)
            *pixel++ = color;
    }
}
