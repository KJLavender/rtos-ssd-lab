/*
 * Records every context switch (time, task, current priority) into a fixed
 * buffer while enabled. tools/timeline.py turns the dump into a text chart.
 */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

void trace_start(void);
void trace_stop(void);

/* Mark a point of interest, e.g. "H wants lock". Callable from tasks. */
void trace_mark(const char *label);

/* Print the buffer as lines "TR,<tag>,<t_us>,<task>,<prio>" and
 * "TM,<tag>,<t_us>,<task>,<label>", times relative to trace_start(). */
void trace_dump(const char *tag);

#endif
