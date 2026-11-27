#ifndef SEGA_H_
#define SEGA_H_

/* The Sega logo, in its own shape and made of its own letters. */
#define SEGA_SMALL 16    /* its width as four characters, the least it is shown at */
#define SEGA_FULL  304   /* and at the end */
#define SEGA_WHOLE 56    /* narrower than this it is whole characters */
#define SEGA_LINED 152   /* and than this, the lines in its letters are filled in */
#define SEGA_GONE  16000

/* Its colours in a light, 0 for none to 31. */
void sega_light(int light);
/* The logo that many pixels wide in the middle of a black picture, on the
 * framebuffer that is not showing. The characters are the size they
 * always are and stay where they are; it is the logo that is larger, and
 * wider than SEGA_GONE it has gone past the eye and the picture is black. */
void sega_draw(int width);

#endif
