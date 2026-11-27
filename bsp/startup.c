/*
 * Vector table and reset handler for mps2-an385 (Cortex-M3).
 */
#include <stdint.h>
#include "bsp.h"

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
extern int main(void);

/* FreeRTOS port handlers (portable/GCC/ARM_CM3/port.c). */
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

void Reset_Handler(void);
static void HardFault_Handler(void) __attribute__((naked));
static void Default_Handler(void);

/* Exercises that use TIMER0 provide their own handler. */
void TIMER0_Handler(void) __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used))
static const void *const vector_table[] = {
    &_estack,
    Reset_Handler,
    Default_Handler,    /* NMI */
    HardFault_Handler,
    Default_Handler,    /* MemManage */
    Default_Handler,    /* BusFault */
    Default_Handler,    /* UsageFault */
    0, 0, 0, 0,
    vPortSVCHandler,    /* SVCall: starts the first task */
    Default_Handler,    /* DebugMonitor */
    0,
    xPortPendSVHandler, /* PendSV: context switch */
    xPortSysTickHandler,/* SysTick: RTOS tick */
    /* External interrupts 0..7 (UARTs) */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    TIMER0_Handler,     /* IRQ 8 */
    Default_Handler,    /* IRQ 9: TIMER1 (free-running timestamp, no IRQ) */
};

void Reset_Handler(void)
{
    /* Copy initialised data from flash to RAM, then zero .bss. */
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata; )
        *dst++ = *src++;
    for (uint32_t *dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    main();
    for (;;) { }
}

static void Default_Handler(void)
{
    uint32_t irq = SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk;
    bsp_panic("unhandled exception", irq);
}

/* Pick MSP or PSP depending on where the fault happened, pass the stacked
 * frame to C so the faulting PC can be printed. */
static void HardFault_Handler(void)
{
    __asm volatile(
        " tst lr, #4        \n"
        " ite eq            \n"
        " mrseq r0, msp     \n"
        " mrsne r0, psp     \n"
        " b hardfault_c     \n");
}

__attribute__((used)) void hardfault_c(uint32_t *frame)
{
    bsp_panic("HardFault, pc", frame[6]);
}
