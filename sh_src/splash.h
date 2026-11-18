#ifndef SPLASH_H
#define SPLASH_H

#define SPLASH_FRAME_TICKS 3

unsigned splash_title_frame(void);
unsigned splash_last_frame(void);
void splash_palette_init(void);
void splash_draw(unsigned frame);

#endif
