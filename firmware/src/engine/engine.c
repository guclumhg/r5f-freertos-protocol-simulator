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

/* Aligned to its own size because the DMA receive mode wraps the write
 * address inside it, and the hardware ring only works on an aligned buffer. */
static uint8_t   s_rx_storage[RING_BYTES] __attribute__((aligned(RING_BYTES)));
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

/* Transmit arbiter. Sensor frames go out whole; a bridge run goes out whole.
 * See the note in engine.h. */
static uint32_t s_tx_remaining;
static bool     s_tx_from_bridge;

/* Request / response. The interrupt only notes that a request arrived; the
 * protocol task answers it, so nothing reads the snapshot concurrently. */
static volatile bool     s_req_pending;
static volatile uint32_t s_req_cycle;
static volatile uint32_t s_requests;
static volatile uint32_t s_responses;
static volatile uint32_t s_response_failures;
static volatile uint32_t s_response_bytes;
/* Where the answer is assembled. The CAN side asked for a sensor reading, so
 * it gets the 64 bytes of reading - not an acknowledgement. */
static uint8_t s_response[FRAME_PAYLOAD_BYTES];
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

/* Interrupt duration histogram, for the sweep's percentile and the
 * distribution chart. Updated outside the measured window. */
static uint32_t s_hist[ISR_HIST_BINS];

static volatile bool s_paused;

/* Idle loop counter and its quiet-line reference. */
static volatile uint32_t s_idle_spins;
static uint32_t          s_idle_ref_per_ms;

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
    s_response_bytes = 0;
    s_trace_fill_len = s_trace_done_len = 0;
    s_trace_seq = 0;
    s_trace_active = false;
    s_tick_count = 0;
    s_idle_spins = 0;
    s_idle_ref_per_ms = 0;
}

void engine_idle_tick(void)
{
    s_idle_spins++;
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
    bool    sent = false;

    /* The arbiter only ever changes its mind at a frame boundary. A sensor
     * frame cut in half could not be reassembled at the far end, and a bridge
     * run cut in half could not either - so once one of them has the line, it
     * keeps it until it is finished. */
    if (s_tx_remaining == 0u) {
        s_tx_from_bridge = !rb_empty(&s_bridge_ring);
    }

    if (s_tx_from_bridge) {
        if (rb_pop(&s_bridge_ring, &b)) {
            sent = true;
            if (port_uart_tx(b)) { s_bytes_tx_bridge++; }
            else                 { s_tx_stalls++; }
        } else {
            /* Drained. The sensor may start a frame in this same tick. */
            s_tx_from_bridge = false;
        }
    }

    if (!sent) {
        if (s_tx_remaining == 0u &&
            rb_count(&s_sensor_ring) >= SENSOR_FRAME_BYTES) {
            s_tx_remaining = SENSOR_FRAME_BYTES;
        }
        if (s_tx_remaining > 0u && rb_pop(&s_sensor_ring, &b)) {
            s_tx_remaining--;
            sent = true;
            if (port_uart_tx(b)) { s_bytes_tx_sensor++; }
            else                 { s_tx_stalls++; }
        }
    }

    if (!sent) {
        s_idle_ticks++;
    }

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

    uint32_t bin = cycles / ISR_HIST_SCALE;
    s_hist[bin < ISR_HIST_BINS - 1u ? bin : ISR_HIST_BINS - 1u]++;
}

void engine_hist_reset(void)
{
    memset(s_hist, 0, sizeof(s_hist));
}

const uint32_t *engine_hist(void)
{
    return s_hist;
}

/* ------------------------------------------------------- sweep plumbing */

void engine_rx_ring_storage(uint8_t **buf, uint32_t *size_pow2)
{
    *buf       = s_rx_storage;
    *size_pow2 = RING_BYTES;
}

/* The DMA receive mode writes straight into the ring, so all the interrupt
 * has to do is say how far it got. That is the whole point of the mode: the
 * per-byte cost disappears rather than being batched. */
void PORT_HOT(engine_on_rx_block)(uint32_t bytes)
{
    uint32_t head  = s_rx_ring.head;
    uint32_t count = head - s_rx_ring.tail;

    if (count + bytes > s_rx_ring.mask + 1u) {
        s_rx_ring.overflow += bytes;    /* consumer fell behind */
    }
    s_rx_ring.head = head + bytes;
    if (count + bytes > s_rx_ring.peak) {
        s_rx_ring.peak = count + bytes;
    }
    s_bytes_rx += bytes;
}

/* Exactly the work the real receive interrupt does, driven by a timer so it
 * can be pushed past any rate a PL011 can produce. */
void PORT_HOT(engine_on_synth_tick)(void)
{
    static uint8_t synth;
    rb_push(&s_rx_ring, synth++);
    s_bytes_rx++;
}

void engine_rx_resync(void)
{
    s_verifier.have_last    = false;
    s_verifier.state        = 0;      /* hunt for a header again */
    s_verifier.fill         = 0;
    s_verifier.bridge_bytes = 0;

}

void engine_pause(void)
{
    s_paused = true;
    port_stop_ticks();
}

void engine_resume(void)
{
    s_paused = false;
    port_start_ticks();
}

/* ------------------------------------------------------------------ tasks */

static void answer_request(void)
{
    /* Answer from the snapshot of the last good sensor frame. Nothing is
     * dequeued and no ring index moves: the queue is not disturbed. */
    uint16_t want = frame_crc16(s_verifier.snapshot, FRAME_LEN - FRAME_CRC_BYTES);
    uint16_t got  = (uint16_t)s_verifier.snapshot[FRAME_LEN - 2u] |
                    (uint16_t)((uint16_t)s_verifier.snapshot[FRAME_LEN - 1u] << 8);

    if (s_verifier.snapshot_seq == 0u || want != got) {
        s_response_failures++;
        s_req_pending = false;
        return;
    }

    /* Hand over the reading itself: the 64 bytes of payload, checked against
     * what the frame counter says they should be. An answer that is merely
     * well formed is not an answer. */
    const uint8_t *payload = frame_payload(s_verifier.snapshot);
    const uint8_t  counter = s_verifier.snapshot[2];
    memcpy(s_response, payload, FRAME_PAYLOAD_BYTES);

    for (uint32_t i = 0; i < FRAME_PAYLOAD_BYTES; i++) {
        if (s_response[i] != frame_payload_byte(counter, i)) {
            s_response_failures++;
            s_req_pending = false;
            return;
        }
    }

    stat_add(&s_resp_latency, port_cycles() - s_req_cycle);
    s_responses++;
    s_response_bytes += FRAME_PAYLOAD_BYTES;
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
    /* Calibrate the idle counter before anything starts moving on the line.
     * Everything else is either blocked or polling at a millisecond, so this
     * is as close to a quiet machine as the system ever gets. */
    s_idle_spins = 0;
    vTaskDelay(pdMS_TO_TICKS(250));
    s_idle_ref_per_ms = s_idle_spins / 250u;

    port_start_ticks();

    for (;;) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
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
    out->payload_errors  = s_verifier.payload_errors;
    out->frames_built    = s_frames_built;
    out->bridge_bursts   = s_verifier.bridge_bursts;
    out->isotp_errors    = s_verifier.isotp_errors;

    out->can_bursts     = s_tp.bursts;
    out->can_frames     = s_tp.frames;
    out->can_data_bytes = s_tp.data_bytes;
    out->burst_active   = s_tp.active;

    out->requests          = s_requests;
    out->responses         = s_responses;
    out->response_failures = s_response_failures;
    out->response_bytes    = s_response_bytes;

    out->idle_spins      = s_idle_spins;
    out->idle_ref_per_ms = s_idle_ref_per_ms;

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
