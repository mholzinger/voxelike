#include "mars.h"

/* Secondary SH-2: idle for now. It will own monsters, items and particles
 * once terrain cost is measured on the primary alone. */
void s_main(void) {
	for(;;) {
		for(volatile int i = 0; i < 256; i++);
	}
}
