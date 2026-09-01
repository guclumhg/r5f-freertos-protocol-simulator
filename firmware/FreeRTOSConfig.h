/*
 * FreeRTOSConfig.h - RP2350 (Cortex-M33, no TrustZone), single core.
 *
 * Derived from pico-examples/freertos/FreeRTOSConfig_examples_common.h.
 *
 * The setting that matters most for this project is
 * configMAX_SYSCALL_INTERRUPT_PRIORITY. RP2350 implements 4 NVIC priority
 * bits, so priorities step in units of 16. A value of 16 means the kernel's
 * critical sections mask everything at priority 16 and below, and leave
 * priority 0 alone. The UART RX ISR runs at priority 0: FreeRTOS cannot
 * delay it, mask it or schedule around it. The price is that the ISR may not
 * call any FreeRTOS API - it does not; it only writes one byte into a
 * lock-free ring.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* We run on core 0 only. The target is a lockstep R5F, which is a single
 * stream of execution; spreading tasks over two cores would measure
 * something the target cannot do. */
#define configNUMBER_OF_CORES                   1

#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)512)
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (96 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Left on deliberately. It costs a few cycles per context switch, which is
 * not on the measured path (the RX ISR is), and it turns a silent stack
 * overflow into an immediate, obvious failure. */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* Per-task CPU usage is one of the things we have to report. */
#define configGENERATE_RUN_TIME_STATS           1
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* The RP2350 port needs the timer service task: configSUPPORT_PICO_SYNC_INTEROP
 * hands a blocked task back to the scheduler through xEventGroupSetBitsFromISR,
 * which only exists when timers and pended function calls are compiled in.
 *
 * FreeRTOS normally recommends running this service task at the top priority.
 * It is deliberately placed below the sensor task here instead, so that it can
 * never preempt the protocol task. Nothing on the measured path depends on it -
 * its only user in this system is the USB mutex the telemetry task takes, and
 * telemetry runs below it, so there is no inversion. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               2
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            512

#ifndef __ASSEMBLER__
/* Free-running 1 us counter, implemented in port_rp2350.c over the RP2350
 * always-on timer. FreeRTOS only needs it to be monotonic and much faster
 * than the tick. */
unsigned long r5f_runtime_counter(void);
#endif
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()   /* timer already running */
#define portGET_RUN_TIME_COUNTER_VALUE()           r5f_runtime_counter()

#ifndef __ASSEMBLER__
#include <assert.h>
#define configASSERT(x)                         assert(x)
#endif

/* Cortex-M33 on RP2350: secure-only, no TrustZone, no MPU, FPU on. */
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configENABLE_FPU                        1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16

/* The RP2350 port is an SMP port even when configNUMBER_OF_CORES is 1. */
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1
#define configUSE_PASSIVE_IDLE_HOOK             0

/* Let FreeRTOS tasks use pico_sync / pico_time primitives safely. */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1

#endif /* FREERTOS_CONFIG_H */
