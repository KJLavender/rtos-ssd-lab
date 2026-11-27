/*
 * Board support for the exercises: UART log output, a microsecond timestamp,
 * calibrated CPU burn loops and a way to end the QEMU run.
 */
#ifndef BSP_H
#define BSP_H

#include <stdint.h>
#include "CMSDK_CM3.h"

#define BSP_CPU_HZ 25000000UL

void bsp_init(void);

/* printf subset: %d %u %x %s %c %%, optional zero pad / width, 'l' ignored.
 * The whole line is written inside a critical section, so lines from
 * different tasks never interleave. Callable from tasks only. */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Free-running TIMER1 at 25 MHz. Wraps after ~171 s. */
static inline uint32_t bsp_cycles(void)
{
    return 0xFFFFFFFFUL - CMSDK_TIMER1->VALUE;
}
static inline uint32_t bsp_now_us(void)
{
    return bsp_cycles() / (BSP_CPU_HZ / 1000000UL);
}

/* Burn CPU for roughly `us` microseconds of *run time*. If the task gets
 * preempted, the burn takes longer in wall-clock time - that is the point:
 * it models a fixed amount of work, not a fixed delay. */
void bsp_burn_us(uint32_t us);

/* Stop QEMU (semihosting SYS_EXIT). */
void bsp_exit(int code) __attribute__((noreturn));
void bsp_panic(const char *what, uint32_t value) __attribute__((noreturn));

#endif
