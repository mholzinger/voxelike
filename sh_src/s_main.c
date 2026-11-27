#include "mars.h"
#include "persp.h"
#include "polybench.h"

/* Secondary SH-2. In PERSP builds it draws half of every frame; otherwise
 * it has nothing to do yet. */
void s_main(void) {
	for(;;) {
#ifdef POLYBENCH
		polybench_secondary();
#elif defined(PERSP)
		persp_secondary();
#else
		for(volatile int i = 0; i < 256; i++);
#endif
	}
}
