#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Shared by all exercises. Values an exercise may override from the
 * Makefile are wrapped in #ifndef. */

#ifndef configUSE_PREEMPTION
#define configUSE_PREEMPTION                    1
#endif
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configCPU_CLOCK_HZ                      25000000UL
#ifndef configTICK_RATE_HZ
#define configTICK_RATE_HZ                      1000      /* 1 tick = 1 ms */
#endif
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256       /* words */
#define configMAX_TASK_NAME_LEN                 12
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (64 * 1024)

#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   2
#define configQUEUE_REGISTRY_SIZE               0

#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            256

#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_TRACE_FACILITY                1

/* Run-time stats use the free-running TIMER1 (25 MHz) set up in bsp_init(). */
#define configGENERATE_RUN_TIME_STATS           1
#define configRUN_TIME_COUNTER_TYPE             uint32_t
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()        (0xFFFFFFFFUL - (*(volatile uint32_t *)0x40001004UL))

/* Cortex-M interrupt priorities. Lower number = more urgent.
 * The MPS2 CMSIS header says 3 priority bits (like many real parts), so the
 * values are written already shifted into the top 3 bits of the byte.
 * Any ISR that calls a ...FromISR() API must be at
 * configMAX_SYSCALL_INTERRUPT_PRIORITY or numerically higher (less urgent). */
#define configKERNEL_INTERRUPT_PRIORITY         (7 << 5)  /* 0xE0, lowest */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (5 << 5)  /* 0xA0 */

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xSemaphoreGetMutexHolder        1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTaskGetIdleTaskHandle          1

#ifndef __ASSEMBLER__
#include <stdint.h>
void vAssertCalled(const char *file, uint32_t line);
#endif
#define configASSERT(x) if ((x) == 0) vAssertCalled(__FILE__, __LINE__)

/* Context-switch tracing (bsp/trace.c), used to draw task timelines. */
#ifndef __ASSEMBLER__
void trace_switched_in(void);
#endif
#define traceTASK_SWITCHED_IN() trace_switched_in()

#endif
