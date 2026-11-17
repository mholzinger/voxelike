#include "common.h"

/* Sound effects on the Mega Drive's PSG, run by the 68000 once a frame.
 * The SH-2s are busy drawing and the 68000 is not, so sound costs the
 * picture nothing. The primary SH-2 asks for effects by setting bits in
 * COMM10, one bit per effect, and only when it reads back zero; this side
 * starts them and clears it.
 *
 * An effect is a list of frames for one PSG channel. A frame is a word:
 * the top four bits are how much quieter than full (0 loudest, 15 silent),
 * the rest is the tone's period (channels 0 to 2) or the noise mode
 * (channel 3). Effects on different channels play together; a new one on
 * a channel replaces what was playing there. */
#define MARS_COMM10  0xA1512A
#define PSG_PORT     0xC00011

static volatile uint16_t *const mars_comm10 = (uint16_t *)MARS_COMM10;
static volatile uint8_t *const psg = (uint8_t *)PSG_PORT;

/* int is 16 bits on this side: the arithmetic is spelled out as long. */
#define T(hz, quiet)   (uint16_t)(((uint32_t)(quiet) << 12) | (111861UL / (hz)))   /* a tone: the PSG divides its clock by 32 */
#define HISS  4                       /* noise modes: white noise, high */
#define RUSH  5                       /* white noise, middle */
#define RUMBLE 6                      /* white noise, low */
#define N(mode, quiet) (uint16_t)(((uint32_t)(quiet) << 12) | (mode))
#define END   0xFFFF

static const uint16_t step[]    = { N(RUMBLE, 9), N(RUMBLE, 12), END };
static const uint16_t swing[]   = { N(HISS, 7), N(HISS, 5), N(HISS, 8), N(HISS, 11), END };
static const uint16_t hit[]     = { T(196, 1), T(147, 2), T(110, 4), T(82, 7), T(62, 10), END };
static const uint16_t hurt[]    = { T(520, 2), T(440, 2), T(370, 3), T(311, 4), T(262, 6),
                                    T(220, 8), T(185, 10), END };
static const uint16_t burst[]   = { N(RUSH, 1), N(RUSH, 2), N(RUMBLE, 3), N(RUMBLE, 4), N(RUMBLE, 5),
                                    N(RUMBLE, 6), N(RUMBLE, 7), N(RUMBLE, 8), N(RUMBLE, 9),
                                    N(RUMBLE, 10), N(RUMBLE, 11), N(RUMBLE, 13), END };
static const uint16_t item[]    = { T(988, 4), T(988, 4), T(988, 5), T(1319, 4), T(1319, 4),
                                    T(1319, 6), T(1319, 9), T(1319, 12), END };
static const uint16_t gold[]    = { T(1568, 4), T(1568, 5), T(2093, 4), T(2093, 5), T(2637, 4),
                                    T(2637, 5), T(2637, 8), T(2637, 11), END };
static const uint16_t stairs[]  = { T(523, 4), T(523, 4), T(523, 6), T(466, 4), T(466, 4), T(466, 6),
                                    T(415, 4), T(415, 4), T(415, 6), T(370, 4), T(370, 4), T(370, 6),
                                    T(330, 4), T(330, 4), T(330, 6), T(294, 4), T(294, 4), T(294, 6),
                                    T(262, 4), T(262, 5), T(262, 7), T(262, 10), END };
static const uint16_t levelup[] = { T(523, 3), T(523, 3), T(523, 3), T(523, 5), T(659, 3), T(659, 3),
                                    T(659, 3), T(659, 5), T(784, 3), T(784, 3), T(784, 3), T(784, 5),
                                    T(1047, 3), T(1047, 3), T(1047, 3), T(1047, 4), T(1047, 5),
                                    T(1047, 7), T(1047, 9), T(1047, 12), END };
static const uint16_t blip[]    = { T(1245, 7), T(1245, 10), END };
#define HOLD8(hz, q) T(hz, q), T(hz, q), T(hz, q), T(hz, q), T(hz, q), T(hz, q), T(hz, q), T(hz, q)
static const uint16_t death[]   = { HOLD8(392, 3), HOLD8(370, 3), HOLD8(330, 3), HOLD8(294, 4),
                                    HOLD8(262, 4), HOLD8(262, 6), HOLD8(262, 9), HOLD8(262, 12), END };

/* In the order of the bits the SH-2 sets: see sh_src/sfx.h. */
static const struct { uint8_t channel; const uint16_t *frames; } effects[] = {
	{ 3, step }, { 3, swing }, { 2, hit }, { 1, hurt }, { 3, burst }, { 0, item },
	{ 0, gold }, { 0, stairs }, { 0, levelup }, { 0, blip }, { 1, death },
};
#define N_EFFECTS (sizeof(effects) / sizeof(effects[0]))

static const uint16_t *playing[4];

static void quiet(uint8_t channel) {
	*psg = 0x90 | (channel << 5) | 15;
}

void sound_init(void) {
	for(uint8_t c = 0; c < 4; c++) quiet(c);
}

/* Once a frame. */
void sound_frame(void) {
	uint16_t asked = *mars_comm10;
	if(asked) {
		for(uint8_t i = 0; i < N_EFFECTS; i++) {
			if(asked & (1u << i)) playing[effects[i].channel] = effects[i].frames;
		}
		*mars_comm10 = 0;
	}

	for(uint8_t c = 0; c < 4; c++) {
		if(!playing[c]) continue;
		uint16_t f = *playing[c]++;
		if(f == END) {
			playing[c] = 0;
			quiet(c);
			continue;
		}
		if(c == 3) {
			*psg = 0xE0 | (f & 7);
		} else {
			*psg = 0x80 | (c << 5) | (f & 15);
			*psg = (f >> 4) & 0x3F;
		}
		*psg = 0x90 | (c << 5) | (f >> 12);
	}
}
