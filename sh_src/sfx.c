#include "mars.h"
#include "sfx.h"

static uint16_t asked;

void sfx(int id) {
	asked |= (uint16_t)(1u << id);
}

/* The 68000 clears COMM10 when it has taken what was there. Writing only
 * then means neither side can lose the other's word. */
void sfx_flush(void) {
	if(asked && MARS_SYS_COMM10 == 0) {
		MARS_SYS_COMM10 = asked;
		asked = 0;
	}
}
