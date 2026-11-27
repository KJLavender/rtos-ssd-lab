#include "FreeRTOS.h"
#include "task.h"
#include "bsp.h"
#include "trace.h"

#define TRACE_MAX 512

struct trace_ev {
    uint32_t t;            /* cycles */
    TaskHandle_t task;
    const char *label;     /* NULL for a context switch */
    uint8_t prio;
};

static struct trace_ev evs[TRACE_MAX];
static volatile uint32_t n_evs;
static volatile int enabled;
static uint32_t t_start;

static void record(TaskHandle_t task, const char *label, UBaseType_t prio)
{
    if (!enabled || n_evs >= TRACE_MAX)
        return;
    struct trace_ev *e = &evs[n_evs++];
    e->t = bsp_cycles();
    e->task = task;
    e->label = label;
    e->prio = (uint8_t)prio;
}

/* Called by the kernel from vTaskSwitchContext() (PendSV, interrupts masked),
 * so only ISR-safe calls here. */
void trace_switched_in(void)
{
    if (!enabled)
        return;
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    record(cur, NULL, uxTaskPriorityGetFromISR(cur));
}

void trace_start(void)
{
    taskENTER_CRITICAL();
    n_evs = 0;
    t_start = bsp_cycles();
    enabled = 1;
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    record(cur, NULL, uxTaskPriorityGet(cur));
    taskEXIT_CRITICAL();
}

void trace_stop(void)
{
    enabled = 0;
}

void trace_mark(const char *label)
{
    taskENTER_CRITICAL();
    TaskHandle_t cur = xTaskGetCurrentTaskHandle();
    record(cur, label, uxTaskPriorityGet(cur));
    taskEXIT_CRITICAL();
}

void trace_dump(const char *tag)
{
    for (uint32_t i = 0; i < n_evs; i++) {
        const struct trace_ev *e = &evs[i];
        uint32_t us = (e->t - t_start) / (BSP_CPU_HZ / 1000000UL);
        const char *name = pcTaskGetName(e->task);
        if (e->label)
            log_printf("TM,%s,%lu,%s,%s\r\n", tag, us, name, e->label);
        else
            log_printf("TR,%s,%lu,%s,%u\r\n", tag, us, name, (unsigned)e->prio);
    }
    if (n_evs >= TRACE_MAX)
        log_printf("TRACE_FULL,%s\r\n", tag);
}
