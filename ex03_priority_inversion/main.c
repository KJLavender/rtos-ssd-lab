/*
 * Exercise 3: priority inversion, binary semaphore vs mutex.
 *
 * Three workers share one lock:
 *   L (prio 1): takes the lock, does 10 ms of work inside it
 *   H (prio 3): wants the same lock 2 ms later
 *   M (prio 2): does not need the lock; becomes ready at 3 ms and burns 50 ms
 * A controller task (prio 4) releases them at those times, so every round
 * follows the same script.
 *
 * Binary semaphore: H waits, M preempts L, and H ends up waiting for M too.
 * Mutex: when H blocks, L inherits H's priority, M cannot preempt L, and H
 * only waits for the rest of L's critical section.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bsp.h"
#include "trace.h"

#define ROUNDS          5
#define L_HOLD_US       10000
#define H_WORK_US       1000
#define M_WORK_US       50000
#define H_ARRIVES_MS    2
#define M_ARRIVES_MS    3

enum lock_kind { LOCK_BINARY_SEM, LOCK_MUTEX };

static SemaphoreHandle_t lock;
static TaskHandle_t t_ctrl, t_low, t_mid, t_high;
static uint32_t h_wait_us;

static void task_low(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(lock, portMAX_DELAY);
        trace_mark("L takes lock");
        bsp_burn_us(L_HOLD_US);
        trace_mark("L gives lock");
        xSemaphoreGive(lock);
        xTaskNotifyGive(t_ctrl);
    }
}

static void task_mid(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        trace_mark("M starts");
        bsp_burn_us(M_WORK_US);
        trace_mark("M done");
        xTaskNotifyGive(t_ctrl);
    }
}

static void task_high(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint32_t t0 = bsp_now_us();
        trace_mark("H wants lock");
        xSemaphoreTake(lock, portMAX_DELAY);
        h_wait_us = bsp_now_us() - t0;
        trace_mark("H got lock");
        bsp_burn_us(H_WORK_US);
        xSemaphoreGive(lock);
        xTaskNotifyGive(t_ctrl);
    }
}

static void run_case(enum lock_kind kind)
{
    const char *name = kind == LOCK_MUTEX ? "mutex" : "binary_semaphore";

    if (kind == LOCK_MUTEX) {
        lock = xSemaphoreCreateMutex();
    } else {
        lock = xSemaphoreCreateBinary();
        xSemaphoreGive(lock);   /* binary semaphores start empty */
    }

    uint32_t sum = 0, max = 0, min = UINT32_MAX;
    for (int r = 0; r < ROUNDS; r++) {
        vTaskDelay(1);          /* start each round right after a tick */
        if (r == 0)
            trace_start();

        xTaskNotifyGive(t_low);
        vTaskDelay(pdMS_TO_TICKS(H_ARRIVES_MS));
        xTaskNotifyGive(t_high);
        vTaskDelay(pdMS_TO_TICKS(M_ARRIVES_MS - H_ARRIVES_MS));
        xTaskNotifyGive(t_mid);

        for (int done = 0; done < 3; done++)
            ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        if (r == 0)
            trace_stop();
        log_printf("%s round %d: H waited %lu us for the lock\r\n", name, r + 1, h_wait_us);
        sum += h_wait_us;
        if (h_wait_us > max)
            max = h_wait_us;
        if (h_wait_us < min)
            min = h_wait_us;
    }

    log_printf("RESULT,ex03,%s,avg_h_wait_us=%lu,min_h_wait_us=%lu,max_h_wait_us=%lu\r\n",
               name, sum / ROUNDS, min, max);
    trace_dump(name);
    vSemaphoreDelete(lock);
}

static void task_ctrl(void *arg)
{
    log_printf("ex03 priority inversion: L holds the lock %u ms, M burns %u ms, H needs the lock\r\n",
               L_HOLD_US / 1000, M_WORK_US / 1000);
    run_case(LOCK_BINARY_SEM);
    run_case(LOCK_MUTEX);
    bsp_exit(0);
}

int main(void)
{
    bsp_init();
    xTaskCreate(task_low, "L", 256, NULL, 1, &t_low);
    xTaskCreate(task_mid, "M", 256, NULL, 2, &t_mid);
    xTaskCreate(task_high, "H", 256, NULL, 3, &t_high);
    xTaskCreate(task_ctrl, "CTRL", 256, NULL, 4, &t_ctrl);
    vTaskStartScheduler();
    bsp_panic("scheduler returned", 0);
}
