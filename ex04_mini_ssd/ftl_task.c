/*
 * FTL task: turns host commands into NAND operations.
 * GC task: reclaims blocks in the background when free blocks run low.
 *
 * Both change the mapping table, so every FTL core call happens with
 * map_lock held. A NAND operation is queued while still holding the lock,
 * so the order in nand_q always matches the order of the table updates:
 * a read queued after a write (or a GC copy) to the same page sees the new
 * data, and a block's erase is queued after every read that looked it up.
 */
#include "bsp.h"
#include "ssd.h"

volatile enum gc_mode gc_mode;
volatile int gc_active, gc_drain;
int32_t gc_credit, gc_ratio;
uint32_t fg_stalls;

uint32_t gc_level(void)
{
    return gc_mode == GC_PACED ? GC_LEVEL_PACED : GC_LEVEL_BURST;
}

static void host_write(const struct host_cmd *cmd)
{
    struct nand_op op = { .type = NAND_PROG, .lpn = cmd->lpn, .cmd = *cmd };

    xSemaphoreTake(map_lock, portMAX_DELAY);
    while (ftl_host_blocked(HOST_RESERVE)) {
        /* Foreground stall: no block to write into until GC frees one. */
        fg_stalls++;
        xEventGroupClearBits(ssd_events, EV_BLOCK_FREED);
        xSemaphoreGive(map_lock);
        xEventGroupSetBits(ssd_events, EV_GC_WAKE);
        xTaskNotifyGiveIndexed(t_gc, 1);
        xEventGroupWaitBits(ssd_events, EV_BLOCK_FREED, pdTRUE, pdFALSE, portMAX_DELAY);
        xSemaphoreTake(map_lock, portMAX_DELAY);
    }
    op.ppn = ftl_host_alloc(cmd->lpn);
    xQueueSend(nand_q, &op, portMAX_DELAY);

    int give_credit = gc_active && gc_mode == GC_PACED;
    if (give_credit)
        gc_credit += gc_ratio;
    uint32_t free_blocks = ftl_free_blocks();
    xSemaphoreGive(map_lock);

    if (give_credit)
        xTaskNotifyGiveIndexed(t_gc, 1);
    if (free_blocks < gc_level())
        xEventGroupSetBits(ssd_events, EV_GC_WAKE);
}

static void host_read(const struct host_cmd *cmd)
{
    struct nand_op op = { .type = NAND_READ, .lpn = cmd->lpn, .cmd = *cmd };

    xSemaphoreTake(map_lock, portMAX_DELAY);
    op.ppn = ftl_lookup(cmd->lpn);
    if (op.ppn != FTL_NONE)
        xQueueSend(nand_q, &op, portMAX_DELAY);
    xSemaphoreGive(map_lock);

    if (op.ppn == FTL_NONE) {   /* never written: complete with zeros */
        struct cmd_msg done = { .kind = MSG_DONE, .read_ok = 1, .read_data = 0, .cmd = *cmd };
        xQueueSend(cmd_q, &done, portMAX_DELAY);
    }
}

void task_ftl(void *arg)
{
    struct host_cmd cmd;
    for (;;) {
        xQueueReceive(ftl_q, &cmd, portMAX_DELAY);
        if (cmd.op == CMD_WRITE)
            host_write(&cmd);
        else
            host_read(&cmd);
    }
}

/* ---- GC ---- */

/* Paced mode: wait until host writes have earned one page copy, unless free
 * blocks are already at the urgent level (or the run is ending). */
static void wait_for_credit(void)
{
    for (;;) {
        xSemaphoreTake(map_lock, portMAX_DELAY);
        int urgent = ftl_free_blocks() < GC_URGENT_LEVEL || gc_drain;
        if (gc_credit >= 256 || urgent) {
            if (gc_credit >= 256)
                gc_credit -= 256;
            xSemaphoreGive(map_lock);
            return;
        }
        xSemaphoreGive(map_lock);
        ulTaskNotifyTakeIndexed(1, pdTRUE, pdMS_TO_TICKS(5));
    }
}

static void collect_one(uint32_t victim)
{
    for (uint32_t i = 0; i < FTL_PPB; i++) {
        uint32_t src = victim * FTL_PPB + i;

        xSemaphoreTake(map_lock, portMAX_DELAY);
        uint32_t lpn = ftl_page_owner(src);
        xSemaphoreGive(map_lock);
        if (lpn == FTL_NONE)
            continue;           /* stale page, nothing to move */

        if (gc_mode == GC_PACED)
            wait_for_credit();

        xSemaphoreTake(map_lock, portMAX_DELAY);
        struct nand_op op = { .type = NAND_COPY, .src = src, .lpn = lpn };
        op.ppn = ftl_gc_relocate(lpn, src);     /* FTL_NONE if overwritten meanwhile */
        if (op.ppn != FTL_NONE)
            xQueueSend(nand_q, &op, portMAX_DELAY);
        xSemaphoreGive(map_lock);
    }

    /* Queued after all the copies, so it runs after them. */
    struct nand_op erase = { .type = NAND_ERASE, .ppn = victim };
    xQueueSend(nand_q, &erase, portMAX_DELAY);
    ulTaskNotifyTakeIndexed(0, pdTRUE, portMAX_DELAY);

    xSemaphoreTake(map_lock, portMAX_DELAY);
    if (ftl_block_valid(victim) != 0)
        bsp_panic("GC erased a block with valid pages", victim);
    ftl_block_erased(victim);
    xSemaphoreGive(map_lock);
    xEventGroupSetBits(ssd_events, EV_BLOCK_FREED);
}

void task_gc(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(ssd_events, EV_GC_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);

        for (;;) {
            xSemaphoreTake(map_lock, portMAX_DELAY);
            if (ftl_free_blocks() >= gc_level()) {
                gc_active = 0;
                gc_credit = 0;
                xSemaphoreGive(map_lock);
                break;
            }
            uint32_t victim = ftl_gc_pick_victim();
            if (victim == FTL_NONE)
                bsp_panic("GC found no victim", 0);
            /* Copying v pages frees P - v pages, so the host may write
             * P - v pages while GC copies v: v / (P - v) copies per write.
             * Rounded up, plus 1/8 so GC slowly gains ground. */
            uint32_t v = ftl_block_valid(victim);
            gc_ratio = v >= FTL_PPB ? 256 * FTL_PPB
                                    : (int32_t)((v * 256 * 9 / 8 + (FTL_PPB - v) - 1) / (FTL_PPB - v));
            /* Credit is kept across victims: host writes that arrive while
             * GC waits for an erase still count. */
            gc_active = 1;
            xSemaphoreGive(map_lock);

            collect_one(victim);
        }
    }
}
