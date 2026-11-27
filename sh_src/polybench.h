#ifndef POLYBENCH_H
#define POLYBENCH_H

#include <stdint.h>

/* make POLYBENCH=1: a room of flat polygons, timed. See polybench.c. */

/* Means over one turn of the camera, in FRT ticks. wait is how long the
 * primary waited for the secondary after its own work was done, or with
 * POLYBENCH=2 the time it spent drawing. */
typedef struct {
    uint16_t runs, total, worst, clear, geometry, wait, quads, spans;
} polybench_result_t;
extern polybench_result_t polybench_result;

/* Never returns. */
void polybench_run(void);
void polybench_secondary(void);

#endif
