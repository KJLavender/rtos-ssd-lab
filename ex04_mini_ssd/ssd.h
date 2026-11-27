/*
 * Mini SSD firmware: shared configuration, message types and task handles.
 *
 *   TIMER0 ISR (host) --cmd_q--> CMD task --ftl_q--> FTL task --nand_q--> NAND task
 *        ^                          ^                    |                  |
 *        |                          +------ completions -+------------------+
 *        |                                    GC task --nand_q (copies, erases)
 *   shadow copy of what the host wrote, to check every read
 */
#ifndef SSD_H
#define SSD_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "ftl_core.h"

/* ---- configuration ---- */
#define OP_PERCENT          25
#define LOGICAL_PAGES       (FTL_PHYS_PAGES * 100 / (100 + OP_PERCENT))

#define HOST_PERIOD_US      2000    /* one host command every 2 ms */
#define HOST_READ_PERCENT   20
#define HOST_WARMUP         1000    /* commands before measuring starts */
#define HOST_COMMANDS       4000    /* measured commands per run */
#define HOST_TOTAL          (HOST_WARMUP + HOST_COMMANDS)

/* NAND timing, in 100 us ticks (one die, one operation at a time) */
#define T_READ_TICKS        1       /* 100 us */
#define T_PROG_TICKS        5       /* 500 us */
#define T_ERASE_TICKS       30      /* 3 ms */

/* Free-block levels */
#define HOST_RESERVE        2       /* host may not open a block below this */
#define GC_LEVEL_BURST      FTL_GC_THRESHOLD        /* 3, as in ftl-sim */
#define GC_SOFT_EXTRA       3
#define GC_LEVEL_PACED      (FTL_GC_THRESHOLD + GC_SOFT_EXTRA)
#define GC_URGENT_LEVEL     FTL_GC_THRESHOLD        /* paced GC stops waiting for credit */

/* Task priorities (higher = more urgent) */
#define PRIO_CTRL           6
#define PRIO_GC_HIGH        5
#define PRIO_CMD            4
#define PRIO_FTL            3
#define PRIO_NAND           3
#define PRIO_GC_LOW         1

enum gc_mode { GC_BURST, GC_PACED };

/* ---- messages ---- */
enum { CMD_WRITE, CMD_READ };

struct host_cmd {
    uint8_t op;
    uint32_t lpn;
    uint32_t data;      /* write: data to store; read: data the host expects */
    uint32_t t_issue;   /* bsp_cycles() when the host issued it */
    uint32_t id;        /* issue order; the first HOST_WARMUP are not measured */
};

enum { MSG_NEW, MSG_DONE };

struct cmd_msg {        /* into the CMD task */
    uint8_t kind;
    uint8_t read_ok;
    uint32_t read_data;
    struct host_cmd cmd;
};

enum { NAND_PROG, NAND_READ, NAND_COPY, NAND_ERASE };

struct nand_op {
    uint8_t type;
    uint32_t ppn;       /* PROG/READ target, COPY destination, ERASE block */
    uint32_t src;       /* COPY source page */
    uint32_t lpn;
    struct host_cmd cmd;    /* host ops: completed back to the CMD task */
};

/* ---- shared objects ---- */
extern QueueHandle_t cmd_q, ftl_q, nand_q;
extern SemaphoreHandle_t map_lock;      /* protects the FTL core */
extern EventGroupHandle_t ssd_events;
#define EV_GC_WAKE      (1u << 0)
#define EV_BLOCK_FREED  (1u << 1)

extern TaskHandle_t t_ctrl, t_cmd, t_ftl, t_nand, t_gc;

/* GC settings and state, written under map_lock */
extern volatile enum gc_mode gc_mode;
extern volatile int gc_active;          /* collecting a victim right now */
extern volatile int gc_drain;           /* end of run: finish without credits */
extern int32_t gc_credit;               /* paced: pages GC may copy, x256 */
extern int32_t gc_ratio;                /* paced: credit per host write, x256 */

extern uint32_t fg_stalls;              /* host writes that had to wait for GC */
extern volatile int nand_busy;

uint32_t gc_level(void);

/* host.c */
extern uint32_t shadow[LOGICAL_PAGES];
void host_start(void);
void host_stop(void);
void host_reset_stats(void);
void host_report(const char *mode, const char *gc_prio);
void task_cmd(void *arg);

/* ftl_task.c, nand_task.c */
void task_ftl(void *arg);
void task_gc(void *arg);
void task_nand(void *arg);

#endif
