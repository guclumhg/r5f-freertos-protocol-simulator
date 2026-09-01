/*
 * main.c - R5F FreeRTOS Protocol Simulator.
 *
 * A protocol bridge that on the target runs on a lockstep Cortex-R5F inside a
 * Zynq UltraScale+, running here on an RP2350 so its structure and its buffer
 * behaviour can be measured. The timing numbers do not transfer to the
 * target. The code under engine/ does.
 *
 * Priority order, most urgent first:
 *
 *   UART receive interrupt   NVIC 0x00   above the FreeRTOS syscall ceiling
 *   byte tick interrupt      NVIC 0x40   the 86.8 us transmit slot
 *   CAN slot interrupt       NVIC 0x80   the 135 us stimulus generator
 *   protocol task            prio 4
 *   sensor task              prio 3
 *   sweep task               prio 2      idle unless a sweep is running
 *   telemetry task           prio 1
 *   idle                     prio 0
 *
 * The CAN slot generator sits in an interrupt rather than in a task below the
 * protocol task as the specification lists it. It is stimulus, not part of
 * the system under test, and stimulus that can be delayed by the load it is
 * supposed to be applying would make the measurement meaningless. It does
 * almost nothing per slot, so it cannot starve anything.
 */
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"

#include "config.h"
#include "engine/engine.h"
#include "port/port.h"
#include "sweep.h"
#include "telemetry.h"

#define PRIO_PROTOCOL   4
#define PRIO_SENSOR     3
#define PRIO_SWEEP      2
#define PRIO_TELEMETRY  1

#define STACK_PROTOCOL  1024
#define STACK_SENSOR    1024
#define STACK_SWEEP     1024
#define STACK_TELEMETRY 2048

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    panic("stack overflow in %s", name);
}

/* Every call is one unit of idle time. Comparing the rate under load against
 * the rate on a quiet line gives the true CPU cost of the traffic, including
 * the exception entry and exit that a cycle counter inside the handler cannot
 * see. */
void vApplicationIdleHook(void)
{
    engine_idle_tick();
}

void vApplicationMallocFailedHook(void)
{
    panic("malloc failed");
}

int main(void)
{
    stdio_init_all();

    engine_init();
    sweep_init();

    /* Internal loopback by default: the PL011 feeds its own transmit path
     * back to its own receive path, still at the configured baud rate, so
     * every number this project reports is identical to the wired case
     * without depending on a jumper staying in place. */
    port_init(LOOPBACK_INTERNAL);

    xTaskCreate(engine_protocol_task, "proto", STACK_PROTOCOL,  NULL,
                PRIO_PROTOCOL,  NULL);
    xTaskCreate(engine_sensor_task,   "sensor", STACK_SENSOR,   NULL,
                PRIO_SENSOR,    NULL);
    xTaskCreate(sweep_task,           "sweep",  STACK_SWEEP,    NULL,
                PRIO_SWEEP,     NULL);
    xTaskCreate(telemetry_task,       "telem",  STACK_TELEMETRY, NULL,
                PRIO_TELEMETRY, NULL);

    /* The sensor task primes its ring and then starts the ticks, so the line
     * does not begin transmitting into an empty buffer. */
    vTaskStartScheduler();

    panic("scheduler returned");
    return 0;
}
