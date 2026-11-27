/*
 * Exercise 4: a mini SSD firmware on FreeRTOS.
 *
 * The C FTL core (ftl/) split into tasks:
 *   CMD  (prio 4)  front end: takes host commands, records completions
 *   FTL  (prio 3)  looks up / updates the mapping table (mutex)
 *   NAND (prio 3)  one flash die; vTaskDelay models read/program/erase time
 *   GC   (prio 1 or 5)  woken through an event group when free blocks run low
 *
 * The controller runs the same host workload four times:
 *   burst GC (copy a whole victim at once) vs paced GC (copy a few pages per
 *   host write), each with GC priority below and above the CMD task.
 * Each run starts from a full, steady-state drive, then checks that every
 * read returned the latest data and that the mapping table is consistent.
 */
#include "bsp.h"
#include "nand_sim.h"
#include "ssd.h"

QueueHandle_t cmd_q, ftl_q, nand_q;
SemaphoreHandle_t map_lock;
EventGroupHandle_t ssd_events;
TaskHandle_t t_ctrl, t_cmd, t_ftl, t_nand, t_gc;

uint32_t host_read_errors(void);

/* Bring the drive to steady state without timing: sequential fill, then one
 * capacity of random overwrites, with synchronous GC that keeps as many free
 * blocks as the GC mode under test (as in the host test). */
static void precondition(void)
{
    uint32_t rng = 88172645u;
    ftl_init(LOGICAL_PAGES);
    nand_init();

    for (uint32_t i = 0; i < 2 * LOGICAL_PAGES; i++) {
        uint32_t lpn = i;
        if (i >= LOGICAL_PAGES) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            lpn = rng % LOGICAL_PAGES;
        }
        ftl_unmap(lpn);
        if (ftl_host_blocked(gc_level())) {
            while (ftl_free_blocks() < gc_level()) {
                uint32_t victim = ftl_gc_pick_victim();
                for (uint32_t p = 0; p < FTL_PPB; p++) {
                    uint32_t src = victim * FTL_PPB + p, owner = ftl_page_owner(src), oob, data;
                    if (owner == FTL_NONE)
                        continue;
                    nand_read(src, &oob, &data);
                    nand_program(ftl_gc_relocate(owner, src), owner, data);
                }
                nand_erase(victim);
                ftl_block_erased(victim);
            }
        }
        shadow[lpn] = i + 1;
        nand_program(ftl_host_alloc(lpn), lpn, shadow[lpn]);
    }
    ftl_reset_stats();
}

/* Read every LPN straight from the flash model and compare with the host. */
static uint32_t full_scan(void)
{
    uint32_t bad = 0;
    for (uint32_t lpn = 0; lpn < LOGICAL_PAGES; lpn++) {
        uint32_t oob, data;
        if (nand_read(ftl_lookup(lpn), &oob, &data) != 0 || oob != lpn || data != shadow[lpn])
            bad++;
    }
    return bad;
}

static void wait_until_idle(void)
{
    int quiet = 0;
    gc_drain = 1;
    xTaskNotifyGiveIndexed(t_gc, 1);
    while (quiet < 2) {
        vTaskDelay(pdMS_TO_TICKS(5));
        int idle = !nand_busy && !gc_active && uxQueueMessagesWaiting(nand_q) == 0 &&
                   uxQueueMessagesWaiting(ftl_q) == 0 && uxQueueMessagesWaiting(cmd_q) == 0;
        quiet = idle ? quiet + 1 : 0;
    }
    gc_drain = 0;
}

static void run(enum gc_mode mode, UBaseType_t gc_prio)
{
    const char *mode_name = mode == GC_PACED ? "paced" : "burst";
    const char *prio_name = gc_prio == PRIO_GC_HIGH ? "above_cmd" : "below_cmd";

    gc_mode = mode;
    vTaskPrioritySet(t_gc, gc_prio);
    xEventGroupClearBits(ssd_events, EV_GC_WAKE | EV_BLOCK_FREED);
    precondition();
    host_reset_stats();
    fg_stalls = 0;

    host_start();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* warmup commands done */
    xSemaphoreTake(map_lock, portMAX_DELAY);
    ftl_reset_stats();
    fg_stalls = 0;
    xSemaphoreGive(map_lock);
    uint32_t t0 = bsp_cycles();
    uint32_t idle0 = ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle());

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* measured commands done */
    host_stop();
    uint32_t cycles = bsp_cycles() - t0;
    uint32_t idle_pct = (uint32_t)((uint64_t)(ulTaskGetRunTimeCounter(xTaskGetIdleTaskHandle()) - idle0) * 100 / cycles);
    xSemaphoreTake(map_lock, portMAX_DELAY);
    const struct ftl_stats s = *ftl_stats();   /* before the drain adds GC work */
    xSemaphoreGive(map_lock);
    wait_until_idle();

    uint32_t scan_bad = full_scan(), check = ftl_check(), viol = nand_violations();
    uint32_t waf_x1000 = s.host_writes ? (uint32_t)((uint64_t)s.nand_writes * 1000 / s.host_writes) : 0;

    log_printf("\r\n[%s GC, GC priority %s] %u commands in %lu ms\r\n", mode_name,
               gc_prio == PRIO_GC_HIGH ? "above CMD" : "below CMD", HOST_COMMANDS,
               cycles / (BSP_CPU_HZ / 1000));
    host_report(mode_name, prio_name);
    log_printf("  WAF %lu.%03lu, GC victims %lu, foreground stalls %lu, CPU idle %lu%%\r\n",
               waf_x1000 / 1000, waf_x1000 % 1000, s.gc_victims, fg_stalls, idle_pct);
    log_printf("  checks: read errors %lu, full scan mismatches %lu, map problems %lu, NAND rule violations %lu\r\n",
               host_read_errors(), scan_bad, check, viol);
    log_printf("RESULT,ex04_check,%s,gc_prio=%s,waf_x1000=%lu,fg_stalls=%lu,cpu_idle_pct=%lu,read_errors=%lu,scan_mismatches=%lu,map_problems=%lu,nand_violations=%lu\r\n",
               mode_name, prio_name, waf_x1000, fg_stalls, idle_pct, host_read_errors(), scan_bad, check, viol);
}

static void task_ctrl(void *arg)
{
    log_printf("ex04 mini SSD: %u blocks x %u pages, OP %u%%, %lu logical pages\r\n",
               FTL_BLOCKS, FTL_PPB, OP_PERCENT, (uint32_t)LOGICAL_PAGES);
    log_printf("host: 1 command / %u us, %u%% reads; NAND: read %u us, program %u us, erase %u us\r\n",
               HOST_PERIOD_US, HOST_READ_PERCENT, T_READ_TICKS * 100, T_PROG_TICKS * 100,
               T_ERASE_TICKS * 100);

    run(GC_BURST, PRIO_GC_LOW);
    run(GC_BURST, PRIO_GC_HIGH);
    run(GC_PACED, PRIO_GC_LOW);
    run(GC_PACED, PRIO_GC_HIGH);
    bsp_exit(0);
}

int main(void)
{
    bsp_init();

    cmd_q = xQueueCreate(64, sizeof(struct cmd_msg));
    ftl_q = xQueueCreate(64, sizeof(struct host_cmd));
    nand_q = xQueueCreate(128, sizeof(struct nand_op));
    map_lock = xSemaphoreCreateMutex();
    ssd_events = xEventGroupCreate();

    NVIC_SetPriority(TIMER0_IRQn, 6);
    NVIC_EnableIRQ(TIMER0_IRQn);

    xTaskCreate(task_ctrl, "CTRL", 512, NULL, PRIO_CTRL, &t_ctrl);
    xTaskCreate(task_cmd, "CMD", 512, NULL, PRIO_CMD, &t_cmd);
    xTaskCreate(task_ftl, "FTL", 512, NULL, PRIO_FTL, &t_ftl);
    xTaskCreate(task_nand, "NAND", 512, NULL, PRIO_NAND, &t_nand);
    xTaskCreate(task_gc, "GC", 512, NULL, PRIO_GC_LOW, &t_gc);
    vTaskStartScheduler();
    bsp_panic("scheduler returned", 0);
}
