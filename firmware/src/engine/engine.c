/*
 * engine.c - The protocol bridge. Portable C over port.h and FreeRTOS.
 *
 * FreeRTOS is a dependency on purpose: the target R5F runs FreeRTOS too
 * (AMD ship a freertos10_xilinx BSP for the Cortex-R5), so depending on it
 * does not weaken the portability claim the way depending on the Pico SDK
 * would.
 */
#include "engine/engine.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "engine/isotp.h"
#include "port/port.h"

/* PL011 flags the overrun in the top bits of the data register read. */
#define DR_OVERRUN_BIT   (1u << 11)

/* Occupancy is sampled every 12 byte ticks, which is 1041.6 us rather than a
 * round millisecond. We report the real number instead of rounding it. */
#define TRACE_TICK_DIVISOR   12u
#define TRACE_PERIOD_US      ((BYTE_TIME_NS * TRACE_TICK_DIVISOR) / 1000u)

_Static_assert(TRACE_PERIOD_US == 1041u, "trace period changed unexpectedly");

/* One request roughly every this many idle CAN slots, chosen by a small
 * generator so the arrival times are not periodic with anything else. */
#define REQUEST_ODDS   400u

/* ------------------------------------------------------------------ state */

static uint8_t   s_rx_storage[RING_BYTES];
static uint8_t   s_bridge_storage[RING_BYTES];
static uint8_t   s_sensor_storage[256];

static ringbuf_t s_rx_ring;
static ringbuf_t s_bridge_ring;   /* the ring the 796 byte figure is about */
static ringbuf_t s_sensor_ring;

static frame_rx_t s_verifier;
static stat_t     s_isr;
static isotp_t    s_tp;

static volatile uint32_t s_bytes_rx;
static volatile uint32_t s_bytes_tx_sensor;
static volatile uint32_t s_bytes_tx_bridge;
static volatile uint32_t s_idle_ticks;
static volatile uint32_t s_tx_stalls;
static volatile uint32_t s_uart_overrun;
static volatile uint32_t s_frames_built;

/* Transmit arbiter. The line is handed out in whole 64 byte units, never
 * switched mid unit - see the note in engine.h. */
static uint32_t s_tx_remaining;
static bool     s_tx_from_bridge;

/* Request / response. The interrupt only notes that a request arrived; the
 * protocol task answers it, so nothing reads the snapshot concurrently. */
static volatile bool     s_req_pending;
static volatile uint32_t s_req_cycle;
static volatile uint32_t s_requests;
static volatile uint32_t s_responses;
static volatile uint32_t s_response_failures;
static stat_t            s_resp_latency;
static uint32_t          s_rng = 0x1234567u;

/* Burst trace, double buffered so telemetry never has to lock against the
 * interrupt that fills it. */
static uint16_t s_trace_fill[BURST_TRACE_SAMPLES];
static uint16_t s_trace_done[BURST_TRACE_SAMPLES];
static volatile uint16_t s_trace_fill_len;
static volatile uint16_t s_trace_done_len;
static volatile uint32_t s_trace_seq;
static volatile bool     s_trace_active;
static uint32_t          s_tick_count;

/* --------------------------------------------------------------- lifetime */

void engine_init(void)
{
    rb_init(&s_rx_ring,     s_rx_storage,     RING_BYTES);
    rb_init(&s_bridge_ring, s_bridge_storage, RING_BYTES);
    rb_init(&s_sensor_ring, s_sensor_storage, sizeof(s_sensor_storage));

    frame_rx_init(&s_verifier);
    stat_reset(&s_isr);
    stat_reset(&s_resp_latency);
    isotp_init(&s_tp);

    s_bytes_rx = s_bytes_tx_sensor = s_bytes_tx_bridge = 0;
    s_idle_ticks = s_tx_stalls = s_uart_overrun = s_frames_built = 0;
    s_tx_remaining = 0;
    s_tx_from_bridge = false;
    s_req_pending = false;
    s_requests = s_responses = s_response_failures = 0;
    s_trace_fill_len = s_trace_done_len = 0;
    s_trace_seq = 0;
    s_trace_active = false;
    s_tick_count = 0;
}

/* ------------------------------------------------------- interrupt context
 *
 * Nothing below this line may call a FreeRTOS API. These run above
 * configMAX_SYSCALL_INTERRUPT_PRIORITY, which is exactly why the receive
 * interrupt can never be delayed by the kernel.
 */

void PORT_HOT(engine_on_rx_byte)(uint8_t b, uint32_t dr_flags)
{
    if (dr_flags & DR_OVERRUN_BIT) {
        s_uart_overrun++;
    }
    rb_push(&s_rx_ring, b);
    s_bytes_rx++;
}

void PORT_HOT(engine_on_byte_tick)(void)
{
    uint8_t b;

    if (s_tx_remaining == 0u) {
        /* Start of a unit: bridge traffic takes the line whenever it has a
         * whole unit ready. The sensor stream is what pays for the burst. */
        if (rb_count(&s_bridge_ring) >= FRAME_LEN) {
            s_tx_from_bridge = true;
            s_tx_remaining   = FRAME_LEN;
        } else if (rb_count(&s_sensor_ring) >= FRAME_LEN) {
            s_tx_from_bridge = false;
            s_tx_remaining   = FRAME_LEN;
        } else {
            s_idle_ticks++;
            goto sample;
        }
    }

    if (rb_pop(s_tx_from_bridge ? &s_bridge_ring : &s_sensor_ring, &b)) {
        s_tx_remaining--;
        if (port_uart_tx(b)) {
            if (s_tx_from_bridge) { s_bytes_tx_bridge++; }
            else                  { s_bytes_tx_sensor++; }
        } else {
            s_tx_stalls++;
        }
    } else {
        s_tx_remaining = 0u;      /* cannot happen; recover rather than wedge */
        s_idle_ticks++;
    }

sample:
    if (++s_tick_count >= TRACE_TICK_DIVISOR) {
        s_tick_count = 0;
        if (s_trace_active) {
            if (s_trace_fill_len < BURST_TRACE_SAMPLES) {
                uint32_t n = rb_count(&s_bridge_ring);
                s_trace_fill[s_trace_fill_len++] =
                    (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
            } else {
                memcpy(s_trace_done, s_trace_fill, sizeof(s_trace_done));
                s_trace_done_len = s_trace_fill_len;
                s_trace_fill_len = 0;
                s_trace_active   = false;
                s_trace_seq++;
            }
        }
    }
}

void PORT_HOT(engine_on_can_slot)(void)
{
    uint8_t  payload[CAN_FRAME_PAYLOAD];
    bool     started = false;

    isotp_slot_kind_t kind = isotp_slot(&s_tp, payload, &started);

    if (started) {
        port_probe_burst(true);
        /* Arm the high rate trace. Ten hertz telemetry would put a single
         * sample inside an 89 ms event; this captures the shape. */
        s_trace_fill_len = 0;
        s_trace_active   = true;
    }

    if (kind == ISOTP_SLOT_CONSECUTIVE) {
        for (uint32_t i = 0; i < CAN_FRAME_PAYLOAD; i++) {
            rb_push(&s_bridge_ring, payload[i]);
        }
    } else if (kind == ISOTP_SLOT_END) {
        port_probe_burst(false);
    } else if (kind == ISOTP_SLOT_IDLE) {
        /* Between bursts the CAN side occasionally asks for the most recent
         * sensor frame. The interrupt only records the request. */
        s_rng = s_rng * 1664525u + 1013904223u;
        if (!s_req_pending && (s_rng >> 8) % REQUEST_ODDS == 0u) {
            s_req_cycle   = port_cycles();
            s_requests++;
            s_req_pending = true;
        }
    }
}

/* Called by the port with the measured interrupt duration, after the port has
 * already taken its closing timestamp. The statistics update itself is
 * therefore outside the reported window; the GPIO probe covers the whole
 * interrupt including this call, so the two can be compared on a scope. */
void PORT_HOT(engine_record_isr)(uint32_t cycles)
{
    stat_add(&s_isr, cycles);
}

/* ------------------------------------------------------------------ tasks */

static void answer_request(void)
{
    /* Answer from the snapshot of the last good sensor frame. Nothing is
     * dequeued and no ring index moves: the queue is not disturbed. */
    uint16_t want = frame_crc16(s_verifier.snapshot, FRAME_LEN - 2u);
    uint16_t got  = (uint16_t)s_verifier.snapshot[FRAME_LEN - 2u] |
                    (uint16_t)((uint16_t)s_verifier.snapshot[FRAME_LEN - 1u] << 8);

    if (s_verifier.snapshot_seq == 0u || want != got) {
        s_response_failures++;
    } else {
        stat_add(&s_resp_latency, port_cycles() - s_req_cycle);
        s_responses++;
    }
    s_req_pending = false;
}

void engine_protocol_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        uint8_t b;
        /* Drain everything the interrupt has left us. At line rate this is
         * about 12 bytes per wake-up. */
        while (rb_pop(&s_rx_ring, &b)) {
            frame_rx_byte(&s_verifier, b);
        }
        if (s_req_pending) {
            answer_request();
        }
        xTaskDelayUntil(&wake, 1);
    }
}

void engine_sensor_task(void *arg)
{
    (void)arg;
    uint8_t  counter = 0;
    uint8_t  frame[FRAME_LEN];

    /* Prime the ring before the line starts, so the first byte tick has
     * something to send and the idle counter means what it says. */
    while (rb_free(&s_sensor_ring) >= FRAME_LEN) {
        frame_build_sensor(frame, counter++);
        for (uint32_t i = 0; i < FRAME_LEN; i++) {
            rb_push(&s_sensor_ring, frame[i]);
        }
        s_frames_built++;
    }
    port_start_ticks();

    for (;;) {
        /* Keep the small sensor ring topped up. The byte tick drains it at
         * line rate, so this naturally paces itself. During a burst the ring
         * stays full and this task simply has nothing to do - which is the
         * sensor stream giving up the line. */
        while (rb_free(&s_sensor_ring) >= FRAME_LEN) {
            frame_build_sensor(frame, counter++);
            for (uint32_t i = 0; i < FRAME_LEN; i++) {
                rb_push(&s_sensor_ring, frame[i]);
            }
            s_frames_built++;
        }
        vTaskDelay(1);
    }
}

/* -------------------------------------------------------------- telemetry */

void engine_get_stats(engine_stats_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Every scalar below is a single naturally aligned word written by one
     * writer, so it can be read without stopping anything. The only
     * exception is the 64 bit sums, read with interrupts off for a handful of
     * cycles - the one and only place the telemetry path touches interrupt
     * state. */
    uint32_t st = port_irq_save();
    out->isr = s_isr;
    out->response_latency = s_resp_latency;
    port_irq_restore(st);

    out->rx_count        = rb_count(&s_rx_ring);
    out->rx_peak         = s_rx_ring.peak;
    out->rx_overflow     = s_rx_ring.overflow;
    out->bridge_count    = rb_count(&s_bridge_ring);
    out->bridge_peak     = s_bridge_ring.peak;
    out->bridge_overflow = s_bridge_ring.overflow;

    out->bytes_rx        = s_bytes_rx;
    out->bytes_tx_sensor = s_bytes_tx_sensor;
    out->bytes_tx_bridge = s_bytes_tx_bridge;
    out->idle_ticks      = s_idle_ticks;
    out->tx_stalls       = s_tx_stalls;
    out->uart_overrun    = s_uart_overrun;
    out->uart_overrun_hw = port_uart_overrun();

    out->frames_ok       = s_verifier.frames_ok;
    out->crc_errors      = s_verifier.crc_errors;
    out->counter_gaps    = s_verifier.counter_gaps;
    out->resyncs         = s_verifier.resyncs;
    out->frames_built    = s_frames_built;
    out->units_ok        = s_verifier.units_ok;
    out->unit_crc_errors = s_verifier.unit_crc_errors;
    out->isotp_errors    = s_verifier.isotp_errors;

    out->can_bursts     = s_tp.bursts;
    out->can_frames     = s_tp.frames;
    out->can_data_bytes = s_tp.data_bytes;
    out->burst_active   = s_tp.active;

    out->requests          = s_requests;
    out->responses         = s_responses;
    out->response_failures = s_response_failures;

    out->trace_seq       = s_trace_seq;
    out->trace_len       = s_trace_done_len;
    out->trace_period_us = TRACE_PERIOD_US;
    if (s_trace_done_len) {
        memcpy(out->trace, s_trace_done,
               (size_t)s_trace_done_len * sizeof(uint16_t));
    }
}

void engine_reset_window(void)
{
    stat_clear_extremes(&s_isr);
    rb_clear_peak(&s_rx_ring);
    rb_clear_peak(&s_bridge_ring);
}
