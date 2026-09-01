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

#include "port/port.h"

/* PL011 flags the overrun in the top bits of the data register read. */
#define DR_OVERRUN_BIT   (1u << 11)
#define DR_ERROR_BITS    (0xFu << 8)   /* OE, BE, PE, FE */

/* Occupancy is sampled every 12 byte ticks, which is 1041.6 us rather than a
 * round millisecond. We report the real number instead of rounding it. */
#define TRACE_TICK_DIVISOR   12u
#define TRACE_PERIOD_US      ((BYTE_TIME_NS * TRACE_TICK_DIVISOR) / 1000u)

_Static_assert(TRACE_PERIOD_US == 1041u, "trace period changed unexpectedly");

/* ------------------------------------------------------------------ state */

static uint8_t   s_rx_storage[RING_BYTES];
static uint8_t   s_bridge_storage[RING_BYTES];
static uint8_t   s_sensor_storage[256];

static ringbuf_t s_rx_ring;
static ringbuf_t s_bridge_ring;   /* the ring the 796 byte figure is about */
static ringbuf_t s_sensor_ring;

static frame_rx_t s_verifier;
static stat_t     s_isr;

static volatile uint32_t s_bytes_rx;
static volatile uint32_t s_bytes_tx_sensor;
static volatile uint32_t s_bytes_tx_bridge;
static volatile uint32_t s_idle_ticks;
static volatile uint32_t s_tx_stalls;
static volatile uint32_t s_uart_overrun;
static volatile uint32_t s_frames_built;

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

    s_bytes_rx = s_bytes_tx_sensor = s_bytes_tx_bridge = 0;
    s_idle_ticks = s_tx_stalls = s_uart_overrun = s_frames_built = 0;
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

void engine_on_rx_byte(uint8_t b, uint32_t dr_flags)
{
    if (dr_flags & DR_OVERRUN_BIT) {
        s_uart_overrun++;
    }
    rb_push(&s_rx_ring, b);
    s_bytes_rx++;
}

void engine_on_byte_tick(void)
{
    uint8_t b;

    /* Strict priority: bridge traffic takes the line whenever it has
     * anything to send. The sensor stream is what pays for the burst. */
    if (rb_pop(&s_bridge_ring, &b)) {
        if (port_uart_tx(b)) {
            s_bytes_tx_bridge++;
        } else {
            s_tx_stalls++;
        }
    } else if (rb_pop(&s_sensor_ring, &b)) {
        if (port_uart_tx(b)) {
            s_bytes_tx_sensor++;
        } else {
            s_tx_stalls++;
        }
    } else {
        s_idle_ticks++;
    }

    /* Burst profile sampling. */
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

void engine_on_can_slot(void)
{
    /* Filled in with the ISO-TP burst generator in the next step. */
}

/* Called by the port with the measured interrupt duration, after the port has
 * already taken its closing timestamp. The statistics update itself is
 * therefore outside the reported window; the GPIO probe covers the whole
 * interrupt including this call, so the two can be compared on a scope. */
void engine_record_isr(uint32_t cycles)
{
    stat_add(&s_isr, cycles);
}

/* ------------------------------------------------------------------ tasks */

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
        xTaskDelayUntil(&wake, 1);
    }
}

void engine_sensor_task(void *arg)
{
    (void)arg;
    uint16_t counter = 0;
    uint8_t  frame[FRAME_LEN];

    /* Prime the ring before the line starts, so the first byte tick has
     * something to send and the idle counter means what it says. */
    while (rb_free(&s_sensor_ring) >= FRAME_LEN) {
        frame_build(frame, counter++);
        for (uint32_t i = 0; i < FRAME_LEN; i++) {
            rb_push(&s_sensor_ring, frame[i]);
        }
        s_frames_built++;
    }
    port_start_ticks();

    for (;;) {
        /* Keep the small sensor ring topped up. The byte tick drains it at
         * line rate, so this naturally paces itself. */
        while (rb_free(&s_sensor_ring) >= FRAME_LEN) {
            frame_build(frame, counter++);
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
     * exception is the 64 bit sum, which is read with interrupts off for a
     * handful of cycles - the one and only place the telemetry path touches
     * interrupt state. */
    uint32_t st = port_irq_save();
    out->isr = s_isr;
    port_irq_restore(st);

    out->rx_count       = rb_count(&s_rx_ring);
    out->rx_peak        = s_rx_ring.peak;
    out->rx_overflow    = s_rx_ring.overflow;
    out->bridge_count   = rb_count(&s_bridge_ring);
    out->bridge_peak    = s_bridge_ring.peak;
    out->bridge_overflow= s_bridge_ring.overflow;

    out->bytes_rx        = s_bytes_rx;
    out->bytes_tx_sensor = s_bytes_tx_sensor;
    out->bytes_tx_bridge = s_bytes_tx_bridge;
    out->idle_ticks      = s_idle_ticks;
    out->tx_stalls       = s_tx_stalls;
    out->uart_overrun    = s_uart_overrun;
    out->uart_overrun_hw = port_uart_overrun();

    out->frames_ok    = s_verifier.frames_ok;
    out->crc_errors   = s_verifier.crc_errors;
    out->counter_gaps = s_verifier.counter_gaps;
    out->resyncs      = s_verifier.resyncs;
    out->frames_built = s_frames_built;

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
