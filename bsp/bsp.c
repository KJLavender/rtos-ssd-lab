#include <stdarg.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "bsp.h"

/* CMSDK APB UART0 */
#define UART0_DATA    (*(volatile uint32_t *)0x40004000UL)
#define UART0_STATE   (*(volatile uint32_t *)0x40004004UL)
#define UART0_CTRL    (*(volatile uint32_t *)0x40004008UL)
#define UART0_BAUDDIV (*(volatile uint32_t *)0x40004010UL)
#define UART_TX_FULL  (1UL << 0)

static uint32_t burn_iters_per_ms;

static void uart_putc(char c)
{
    while (UART0_STATE & UART_TX_FULL) { }
    UART0_DATA = (uint32_t)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void burn_iters(uint32_t n)
{
    for (volatile uint32_t i = 0; i < n; i++) { }
}

void bsp_burn_us(uint32_t us)
{
    burn_iters((uint32_t)(((uint64_t)burn_iters_per_ms * us) / 1000U));
}

void bsp_init(void)
{
    UART0_BAUDDIV = 16;
    UART0_CTRL = 1;   /* TX enable */

    CMSDK_TIMER1->CTRL = 0;
    CMSDK_TIMER1->RELOAD = 0xFFFFFFFFUL;
    CMSDK_TIMER1->VALUE = 0xFFFFFFFFUL;
    CMSDK_TIMER1->CTRL = CMSDK_TIMER_CTRL_EN_Msk;   /* no interrupt */

    /* Interrupts are still off here, so this measures pure CPU time. */
    const uint32_t probe = 200000;
    uint32_t t0 = bsp_cycles();
    burn_iters(probe);
    uint32_t cycles = bsp_cycles() - t0;
    burn_iters_per_ms = (uint32_t)(((uint64_t)probe * (BSP_CPU_HZ / 1000U)) / cycles);
}

/* ---- tiny printf ------------------------------------------------------- */

struct out { char *p, *end; };

static void out_c(struct out *o, char c)
{
    if (o->p < o->end)
        *o->p++ = c;
}

static void out_num(struct out *o, uint32_t v, unsigned base, int neg, int width, char pad)
{
    char tmp[12];
    int n = 0;
    do {
        unsigned d = v % base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    if (neg)
        tmp[n++] = '-';
    for (int i = n; i < width; i++)
        out_c(o, pad);
    while (n)
        out_c(o, tmp[--n]);
}

static void vformat(struct out *o, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_c(o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l')
            fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int32_t v = va_arg(ap, int32_t);
            out_num(o, v < 0 ? (uint32_t)-v : (uint32_t)v, 10, v < 0, width, pad);
            break;
        }
        case 'u': out_num(o, va_arg(ap, uint32_t), 10, 0, width, pad); break;
        case 'x': out_num(o, va_arg(ap, uint32_t), 16, 0, width, pad); break;
        case 'c': out_c(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            int len = 0;
            for (const char *q = s; *q; q++)
                len++;
            for (int i = len; i < width; i++)
                out_c(o, ' ');
            while (*s)
                out_c(o, *s++);
            break;
        }
        case '%': out_c(o, '%'); break;
        case '\0': return;
        default: out_c(o, '?'); break;
        }
    }
}

void log_printf(const char *fmt, ...)
{
    char buf[320];
    struct out o = { buf, buf + sizeof(buf) - 1 };
    va_list ap;
    va_start(ap, fmt);
    vformat(&o, fmt, ap);
    va_end(ap);
    *o.p = '\0';

    /* QEMU's UART never stalls, so this critical section stays short. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        taskENTER_CRITICAL();
        uart_puts(buf);
        taskEXIT_CRITICAL();
    } else {
        uart_puts(buf);
    }
}

/* ---- ending the run ---------------------------------------------------- */

void bsp_exit(int code)
{
    /* Semihosting SYS_EXIT with ADP_Stopped_ApplicationExit. QEMU needs
     * -semihosting-config enable=on; the exit code is not passed through. */
    (void)code;
    register uint32_t r0 __asm("r0") = 0x18;
    register uint32_t r1 __asm("r1") = 0x20026;
    __asm volatile("bkpt 0xAB" : : "r"(r0), "r"(r1) : "memory");
    for (;;) { }
}

void bsp_panic(const char *what, uint32_t value)
{
    __disable_irq();
    uart_puts("\r\nPANIC: ");
    uart_puts(what);
    char hex[11] = "0x00000000";
    for (int i = 0; i < 8; i++) {
        unsigned d = (value >> (28 - 4 * i)) & 0xF;
        hex[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    uart_puts(" ");
    uart_puts(hex);
    uart_puts("\r\n");
    bsp_exit(1);
}

/* ---- FreeRTOS hooks ---------------------------------------------------- */

void vAssertCalled(const char *file, uint32_t line)
{
    char dec[11];
    int n = sizeof(dec) - 1;
    dec[n] = '\0';
    do {
        dec[--n] = (char)('0' + line % 10);
        line /= 10;
    } while (line && n > 0);

    __disable_irq();
    uart_puts("\r\nASSERT failed: ");
    uart_puts(file);
    uart_puts(":");
    uart_puts(&dec[n]);
    uart_puts("\r\n");
    bsp_exit(1);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    uart_puts("\r\nstack overflow: ");
    uart_puts(name);
    bsp_panic("", 0);
}

void vApplicationMallocFailedHook(void)
{
    bsp_panic("pvPortMalloc failed", 0);
}

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *size)
{
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *tcb = &idle_tcb;
    *stack = idle_stack;
    *size = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack, configSTACK_DEPTH_TYPE *size)
{
    static StaticTask_t timer_tcb;
    static StackType_t timer_stack[configTIMER_TASK_STACK_DEPTH];
    *tcb = &timer_tcb;
    *stack = timer_stack;
    *size = configTIMER_TASK_STACK_DEPTH;
}
