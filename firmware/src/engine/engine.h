/*
 * engine.h - The protocol bridge itself.
 *
 * Portable C on top of port.h and FreeRTOS. This is the code the project
 * claims would move to the Cortex-R5F unchanged.
 *
 * Three rings, each with exactly one writer and one reader:
 *
 *   sensor_ring  (256 B)   sensor task   -> byte tick interrupt
 *   bridge_ring  (4096 B)  CAN slot IRQ  -> byte tick interrupt
 *   rx_ring      (4096 B)  UART RX IRQ   -> protocol task
 *
 * The transmit line is one byte every 86.8 us and the byte tick interrupt is
 * the arbiter for it. The sensor stream is continuous, so it fills the line by
 * construction; the bridge takes the line at the next frame boundary whenever
 * it has anything, and keeps it until it has nothing. The line is
 * oversubscribed on purpose - a full sensor stream plus 1024 B/s of CAN
 * against 11520 B/s of capacity - so during a burst the bridge backlog is what
 * grows, and the sensor gives up 1024 bytes worth of its rate to pay for it.
 *
 * The arbiter changes its mind only at a frame boundary. It has to: the UART
 * is in loopback, so everything transmitted comes straight back to the
 * receiver, and neither a sensor frame nor a bridge run could be reassembled
 * if the other stream cut it in half. That atomicity costs up to one frame -
 * 68 byte times - at the start of a burst, which is why the measured peak
 * backlog sits at or a little above the arithmetic 796 rather than exactly on
 * it.
 */
#ifndef R5F_ENGINE_H
#define R5F_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "engine/frame.h"
#include "engine/metrics.h"
#include "engine/ringbuf.h"

/* Everything the telemetry task needs. */
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

    /* sensor stream, verified after a full trip round the loopback */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t counter_gaps;
    uint32_t resyncs;
    uint32_t frames_built;

    /* bridge stream, carried unframed and checked by its counting pattern */
    uint32_t bridge_bursts;   /* complete 1024 byte runs reassembled */
    uint32_t isotp_errors;

    /* CAN side */
    uint32_t can_bursts;
    uint32_t can_frames;
    uint32_t can_data_bytes;
    bool     burst_active;

    /* request / response: the CAN side asks for the last sensor frame and the
     * engine answers from a snapshot, dequeuing nothing */
    uint32_t requests;
    uint32_t responses;
    uint32_t response_failures;
    stat_t   response_latency;   /* cycles from request to answer */

    /* burst trace: bridge ring occupancy, one sample every 12 byte ticks */
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
