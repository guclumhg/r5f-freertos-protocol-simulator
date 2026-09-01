/*
 * engine.h - The protocol bridge itself.
 *
 * Portable C on top of port.h. This is the code the project claims would move
 * to the Cortex-R5F unchanged.
 *
 * Three rings, each with exactly one writer and one reader:
 *
 *   sensor_ring  (256 B)   sensor task            -> byte tick ISR
 *   bridge_ring  (4096 B)  CAN slot ISR           -> byte tick ISR
 *   rx_ring      (4096 B)  UART RX ISR            -> protocol task
 *
 * The transmit line is one byte every 86.8 us and the byte tick ISR is the
 * arbiter for it. Bridge traffic wins every slot it wants; the sensor stream
 * fills what is left. The line is oversubscribed on purpose - 11520 B/s of
 * sensor plus 1024 B/s of CAN against 11520 B/s of capacity - so during a
 * burst the bridge backlog is what grows, and the sensor gives up exactly 16
 * of its 180 frames per second to pay for it.
 */
#ifndef R5F_ENGINE_H
#define R5F_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "engine/frame.h"
#include "engine/metrics.h"
#include "engine/ringbuf.h"

/* Everything the telemetry task needs, copied out under a short critical
 * section so a burst cannot tear the numbers apart mid-read. */
typedef struct {
    /* receive interrupt cost, in core clock cycles */
    stat_t   isr;

    /* rings */
    uint32_t rx_count,     rx_peak,     rx_overflow;
    uint32_t bridge_count, bridge_peak, bridge_overflow;

    /* the wire */
    uint32_t bytes_rx;
    uint32_t bytes_tx_sensor;
    uint32_t bytes_tx_bridge;
    uint32_t idle_ticks;
    uint32_t tx_stalls;
    uint32_t uart_overrun;    /* PL011 overrun flag seen per received byte */
    uint32_t uart_overrun_hw; /* same thing read from the status register,
                               * as an independent cross-check */

    /* frames */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t counter_gaps;
    uint32_t resyncs;
    uint32_t frames_built;

    /* burst trace: bridge_ring occupancy, one sample every 12 byte ticks */
    uint16_t trace[BURST_TRACE_SAMPLES];
    uint32_t trace_seq;       /* increments once per captured burst */
    uint16_t trace_len;
    uint16_t trace_period_us; /* actual period, not the nominal 1000 */
} engine_stats_t;

void engine_init(void);

/* The two FreeRTOS tasks. Priorities are set in main.c. */
void engine_protocol_task(void *arg);
void engine_sensor_task(void *arg);

/* Copies a consistent snapshot for telemetry. */
void engine_get_stats(engine_stats_t *out);

/* Clears the per-window extremes (ring peaks, ISR min/max). */
void engine_reset_window(void);

#endif /* R5F_ENGINE_H */
