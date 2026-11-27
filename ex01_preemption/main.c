/*
 * Exercise 1: preemption.
 *
 * L (low priority) works forever in 1 ms chunks.
 * H (high priority) wakes every 500 ms, does 2 ms of work, and reports
 *   - wake-up latency: how long after its tick it actually got the CPU
 *   - L's progress counter before and after H's work.
 *
 * Built with PREEMPT=1, H runs right after the tick interrupt and L's counter
 * does not move while H runs. Built with PREEMPT=0 (cooperative), H has to
 * wait until L voluntarily yields, which L does every 50 ms.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "bsp.h"

#define H_PERIOD_MS     500
#define H_ROUNDS        10
#define L_YIELD_EVERY   50     /* chunks; only matters when cooperative */

static volatile uint32_t l_progress_ms;

/* Microseconds between the start of tick `target` and now. */
static uint32_t us_since_tick(TickType_t target)
{
    taskENTER_CRITICAL();
    uint32_t reload = SysTick->LOAD + 1;
    TickType_t now = xTaskGetTickCount();
    uint32_t into = reload - SysTick->VAL;
    if (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) {
        /* SysTick wrapped but its ISR has not run yet. */
        now++;
        into = reload - SysTick->VAL;
    }
    taskEXIT_CRITICAL();
    return (now - target) * (1000000UL / configTICK_RATE_HZ) + into / (configCPU_CLOCK_HZ / 1000000UL);
}

static void task_low(void *arg)
{
    for (;;) {
        bsp_burn_us(1000);
        l_progress_ms++;
        if (l_progress_ms % 100 == 0)
            log_printf("  L running, %lu ms of work done\r\n", l_progress_ms);
#if configUSE_PREEMPTION == 0
        if (l_progress_ms % L_YIELD_EVERY == 0)
            taskYIELD();
#endif
    }
}

static void task_high(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    uint32_t sum = 0, max = 0;

    log_printf("ex01 preemption: scheduler is %s\r\n",
               configUSE_PREEMPTION ? "PREEMPTIVE" : "COOPERATIVE");

    for (int round = 1; round <= H_ROUNDS; round++) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(H_PERIOD_MS));
        uint32_t lat = us_since_tick(last);
        uint32_t l_before = l_progress_ms;
        bsp_burn_us(2000);
        uint32_t l_after = l_progress_ms;

        log_printf("H cuts in at tick %lu: wake latency %lu us, L progress %lu -> %lu ms while H ran\r\n",
                   (uint32_t)last, lat, l_before, l_after);
        sum += lat;
        if (lat > max)
            max = lat;
    }

    log_printf("RESULT,ex01,%s,avg_wake_latency_us=%lu,max_wake_latency_us=%lu\r\n",
               configUSE_PREEMPTION ? "preemptive" : "cooperative", sum / H_ROUNDS, max);
    bsp_exit(0);
}

int main(void)
{
    bsp_init();
    xTaskCreate(task_low, "L", 256, NULL, 1, NULL);
    xTaskCreate(task_high, "H", 256, NULL, 3, NULL);
    vTaskStartScheduler();
    bsp_panic("scheduler returned", 0);
}
