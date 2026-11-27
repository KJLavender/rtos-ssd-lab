/*
 * Exercise 2: interrupt -> queue -> task.
 *
 * TIMER0 fires every 700 us and pretends to be the host sending a command.
 * The ISR only stamps the command and pushes it into a queue; a handler task
 * blocks on the queue and does the real work (200 us per command).
 *
 * Two phases with the same traffic:
 *   1. ISR ends with portYIELD_FROM_ISR(woken): the handler runs right away.
 *   2. ISR does not yield: the handler only runs at the next tick (<= 1 ms).
 * Each phase checks that no command is lost or out of order, and uses the
 * run-time stats to show the handler only uses CPU when there is work.
 *
 * VARIANT=BAD_ISR_PRIO sets TIMER0 more urgent than
 * configMAX_SYSCALL_INTERRUPT_PRIORITY, which FreeRTOS catches with an assert.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "bsp.h"

#define CMD_PERIOD_US   700
#define CMD_COUNT       1000
#define CMD_WORK_US     200
#define QUEUE_DEPTH     8

struct host_cmd {
    uint32_t id;
    uint32_t t_issue;   /* cycles */
};

static QueueHandle_t cmd_queue;
static TaskHandle_t handler_task, ctrl_task;

/* Written by the ISR */
static volatile uint32_t isr_sent, isr_dropped;
static volatile int yield_from_isr;

/* Written by the handler task */
static uint32_t rx_count, rx_order_errors, lat_sum_us, lat_max_us;

void TIMER0_Handler(void)
{
    CMSDK_TIMER0->INTCLEAR = 1;

    BaseType_t woken = pdFALSE;
    struct host_cmd cmd = { isr_sent, bsp_cycles() };
    if (xQueueSendFromISR(cmd_queue, &cmd, &woken) != pdPASS)
        isr_dropped++;
    if (++isr_sent == CMD_COUNT)
        CMSDK_TIMER0->CTRL = 0;

    /* If the send unblocked a task with higher priority than the one we
     * interrupted, switch to it as soon as the ISR returns. */
    if (yield_from_isr)
        portYIELD_FROM_ISR(woken);
}

static void host_start(void)
{
    isr_sent = isr_dropped = 0;
    CMSDK_TIMER0->CTRL = 0;
    CMSDK_TIMER0->INTCLEAR = 1;
    CMSDK_TIMER0->RELOAD = CMD_PERIOD_US * (BSP_CPU_HZ / 1000000UL) - 1;
    CMSDK_TIMER0->VALUE = CMSDK_TIMER0->RELOAD;
    CMSDK_TIMER0->CTRL = CMSDK_TIMER_CTRL_IRQEN_Msk | CMSDK_TIMER_CTRL_EN_Msk;
}

static void task_handler(void *arg)
{
    struct host_cmd cmd;
    uint32_t expected = 0;
    for (;;) {
        /* Blocks (uses no CPU) while the queue is empty. */
        xQueueReceive(cmd_queue, &cmd, portMAX_DELAY);
        uint32_t lat = (bsp_cycles() - cmd.t_issue) / (BSP_CPU_HZ / 1000000UL);

        if (cmd.id == 0)
            expected = 0;
        if (cmd.id != expected)
            rx_order_errors++;
        expected = cmd.id + 1;

        bsp_burn_us(CMD_WORK_US);

        rx_count++;
        lat_sum_us += lat;
        if (lat > lat_max_us)
            lat_max_us = lat;
        if (cmd.id == CMD_COUNT - 1)
            xTaskNotifyGive(ctrl_task);
    }
}

static void run_phase(int yield)
{
    rx_count = rx_order_errors = lat_sum_us = lat_max_us = 0;
    yield_from_isr = yield;

    uint32_t h0 = ulTaskGetRunTimeCounter(handler_task);
    uint32_t i0 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());
    uint32_t t0 = bsp_cycles();

    host_start();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint32_t total = bsp_cycles() - t0;
    uint32_t h_pct = (uint32_t)((uint64_t)(ulTaskGetRunTimeCounter(handler_task) - h0) * 100 / total);
    uint32_t i_pct = (uint32_t)((uint64_t)(ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle()) - i0) * 100 / total);

    log_printf("phase %s: sent %lu, received %lu, dropped %lu, out-of-order %lu\r\n",
               yield ? "yield-from-ISR" : "no-yield", isr_sent, rx_count, isr_dropped, rx_order_errors);
    log_printf("  ISR->task latency avg %lu us, max %lu us; CPU: handler %lu%%, idle %lu%%\r\n",
               lat_sum_us / rx_count, lat_max_us, h_pct, i_pct);
    log_printf("RESULT,ex02,%s,sent=%lu,received=%lu,dropped=%lu,order_errors=%lu,avg_latency_us=%lu,max_latency_us=%lu,handler_cpu_pct=%lu,idle_cpu_pct=%lu\r\n",
               yield ? "yield" : "no_yield", isr_sent, rx_count, isr_dropped, rx_order_errors,
               lat_sum_us / rx_count, lat_max_us, h_pct, i_pct);
}

static void task_ctrl(void *arg)
{
    log_printf("ex02 ISR -> queue -> task: %u commands, one every %u us, %u us of work each\r\n",
               CMD_COUNT, CMD_PERIOD_US, CMD_WORK_US);
    run_phase(1);
    run_phase(0);
    bsp_exit(0);
}

int main(void)
{
    bsp_init();

    cmd_queue = xQueueCreate(QUEUE_DEPTH, sizeof(struct host_cmd));

#ifdef BAD_ISR_PRIO
    NVIC_SetPriority(TIMER0_IRQn, 2);   /* 0x40: more urgent than 0xA0 - not allowed */
#else
    NVIC_SetPriority(TIMER0_IRQn, 6);   /* 0xC0: less urgent than 0xA0 - OK */
#endif
    NVIC_EnableIRQ(TIMER0_IRQn);

    xTaskCreate(task_handler, "CMD", 256, NULL, 2, &handler_task);
    xTaskCreate(task_ctrl, "CTRL", 256, NULL, 3, &ctrl_task);
    vTaskStartScheduler();
    bsp_panic("scheduler returned", 0);
}
