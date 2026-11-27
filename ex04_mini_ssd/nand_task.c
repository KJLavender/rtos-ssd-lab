/*
 * NAND back end: one die, operations run one at a time in arrival order.
 * vTaskDelay stands in for the time the flash is busy; the CPU is free for
 * other tasks meanwhile, as with a real flash controller.
 */
#include "bsp.h"
#include "nand_sim.h"
#include "ssd.h"

volatile int nand_busy;

static TickType_t duration(uint8_t type)
{
    switch (type) {
    case NAND_READ:  return T_READ_TICKS;
    case NAND_PROG:  return T_PROG_TICKS;
    case NAND_COPY:  return T_READ_TICKS + T_PROG_TICKS;
    default:         return T_ERASE_TICKS;
    }
}

void task_nand(void *arg)
{
    struct nand_op op;
    TickType_t busy_until = 0;

    for (;;) {
        xQueueReceive(nand_q, &op, portMAX_DELAY);
        nand_busy = 1;

        /* Back-to-back operations chain on busy_until, so tick rounding does
         * not add up. */
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(busy_until - now) < 0)
            busy_until = now;
        busy_until += duration(op.type);
        vTaskDelay(busy_until - now);

        struct cmd_msg done = { .kind = MSG_DONE, .cmd = op.cmd };
        uint32_t lpn, data;
        switch (op.type) {
        case NAND_PROG:
            nand_program(op.ppn, op.lpn, op.cmd.data);
            xQueueSend(cmd_q, &done, portMAX_DELAY);
            break;
        case NAND_READ:
            done.read_ok = nand_read(op.ppn, &lpn, &data) == 0 && lpn == op.lpn;
            done.read_data = data;
            xQueueSend(cmd_q, &done, portMAX_DELAY);
            break;
        case NAND_COPY:
            /* GC: move the page inside the flash (read + program). */
            if (nand_read(op.src, &lpn, &data) == 0)
                nand_program(op.ppn, lpn, data);
            break;
        case NAND_ERASE:
            nand_erase(op.ppn);
            xTaskNotifyGiveIndexed(t_gc, 0);
            break;
        }
        nand_busy = uxQueueMessagesWaiting(nand_q) != 0;
    }
}
