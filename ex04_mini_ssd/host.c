/*
 * Host side: a timer interrupt that issues commands at a fixed rate, and the
 * CMD task that hands commands to the FTL and handles completions (latency,
 * read-data check).
 */
#include "bsp.h"
#include "ssd.h"

uint32_t shadow[LOGICAL_PAGES];     /* what the host last wrote to each LPN */

static uint32_t rng = 2463534242u;
static uint32_t data_seq = 0x10000;
static volatile uint32_t issued, host_dropped;

static uint32_t xorshift32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* "Host": one command per interrupt. Runs in ISR context. */
void TIMER0_Handler(void)
{
    CMSDK_TIMER0->INTCLEAR = 1;
    if (issued >= HOST_TOTAL) {
        CMSDK_TIMER0->CTRL = 0;
        return;
    }

    struct cmd_msg m = { .kind = MSG_NEW };
    m.cmd.lpn = xorshift32() % LOGICAL_PAGES;
    m.cmd.t_issue = bsp_cycles();
    m.cmd.id = issued;
    if (xorshift32() % 100 < HOST_READ_PERCENT) {
        m.cmd.op = CMD_READ;
        m.cmd.data = shadow[m.cmd.lpn];
    } else {
        m.cmd.op = CMD_WRITE;
        m.cmd.data = ++data_seq;
    }

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(cmd_q, &m, &woken) == pdPASS) {
        if (m.cmd.op == CMD_WRITE)
            shadow[m.cmd.lpn] = m.cmd.data;
        issued++;
    } else {
        host_dropped++;     /* submission queue full; try again next time */
    }
    portYIELD_FROM_ISR(woken);
}

void host_start(void)
{
    issued = 0;
    CMSDK_TIMER0->CTRL = 0;
    CMSDK_TIMER0->INTCLEAR = 1;
    CMSDK_TIMER0->RELOAD = HOST_PERIOD_US * (BSP_CPU_HZ / 1000000UL) - 1;
    CMSDK_TIMER0->VALUE = CMSDK_TIMER0->RELOAD;
    CMSDK_TIMER0->CTRL = CMSDK_TIMER_CTRL_IRQEN_Msk | CMSDK_TIMER_CTRL_EN_Msk;
}

void host_stop(void)
{
    CMSDK_TIMER0->CTRL = 0;
}

/* ---- latency statistics ---- */

#define HIST_BUCKET_US  50
#define HIST_BUCKETS    4000            /* up to 200 ms */

struct lat_stats {
    uint32_t count, max_us;
    uint64_t sum_us;
    uint16_t hist[HIST_BUCKETS];
};

static struct lat_stats lat_write, lat_read;
static uint32_t done, read_errors;

static void lat_add(struct lat_stats *s, uint32_t us)
{
    s->count++;
    s->sum_us += us;
    if (us > s->max_us)
        s->max_us = us;
    uint32_t b = us / HIST_BUCKET_US;
    s->hist[b < HIST_BUCKETS ? b : HIST_BUCKETS - 1]++;
}

/* Upper edge of the bucket holding the given percentile (x10: 990 = p99),
 * never above the exact maximum. */
static uint32_t lat_pct(const struct lat_stats *s, uint32_t pct_x10)
{
    uint32_t want = (uint32_t)(((uint64_t)s->count * pct_x10 + 999) / 1000), seen = 0;
    for (uint32_t b = 0; b < HIST_BUCKETS; b++) {
        seen += s->hist[b];
        if (seen >= want)
            return (b + 1) * HIST_BUCKET_US < s->max_us ? (b + 1) * HIST_BUCKET_US : s->max_us;
    }
    return s->max_us;
}

void host_reset_stats(void)
{
    lat_write = (struct lat_stats){0};
    lat_read = (struct lat_stats){0};
    done = read_errors = host_dropped = 0;
}

void host_report(const char *mode, const char *gc_prio)
{
    const struct lat_stats *w = &lat_write, *r = &lat_read;
    log_printf("  writes %lu: avg %lu us, p50 %lu, p99 %lu, p99.9 %lu, max %lu us\r\n",
               w->count, (uint32_t)(w->sum_us / w->count), lat_pct(w, 500),
               lat_pct(w, 990), lat_pct(w, 999), w->max_us);
    log_printf("  reads  %lu: avg %lu us, p50 %lu, p99 %lu, p99.9 %lu, max %lu us\r\n",
               r->count, (uint32_t)(r->sum_us / r->count), lat_pct(r, 500),
               lat_pct(r, 990), lat_pct(r, 999), r->max_us);
    log_printf("RESULT,ex04,%s,gc_prio=%s,write_avg_us=%lu,write_p50_us=%lu,write_p99_us=%lu,"
               "write_p999_us=%lu,write_max_us=%lu,read_avg_us=%lu,read_p99_us=%lu,read_max_us=%lu,"
               "read_errors=%lu,host_dropped=%lu\r\n",
               mode, gc_prio, (uint32_t)(w->sum_us / w->count), lat_pct(w, 500), lat_pct(w, 990),
               lat_pct(w, 999), w->max_us, (uint32_t)(r->sum_us / r->count), lat_pct(r, 990),
               r->max_us, read_errors, host_dropped);
}

uint32_t host_read_errors(void) { return read_errors; }

/* ---- CMD task: front end ---- */

void task_cmd(void *arg)
{
    struct cmd_msg m;
    for (;;) {
        xQueueReceive(cmd_q, &m, portMAX_DELAY);
        if (m.kind == MSG_NEW) {
            xQueueSend(ftl_q, &m.cmd, portMAX_DELAY);
            continue;
        }

        uint32_t us = (bsp_cycles() - m.cmd.t_issue) / (BSP_CPU_HZ / 1000000UL);
        int measured = m.cmd.id >= HOST_WARMUP;
        if (m.cmd.op == CMD_READ) {
            if (!m.read_ok || m.read_data != m.cmd.data)
                read_errors++;      /* checked during warmup too */
            if (measured)
                lat_add(&lat_read, us);
        } else if (measured) {
            lat_add(&lat_write, us);
        }
        /* Tell the controller when warmup ends and when everything is done. */
        if (++done == HOST_WARMUP || done == HOST_TOTAL)
            xTaskNotifyGive(t_ctrl);
    }
}
