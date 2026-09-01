#include "sweep.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "config.h"
#include "engine/engine.h"
#include "engine/frame.h"

/* The nine real points. The last one is the PL011's ceiling at a 150 MHz
 * peripheral clock: divider exactly 1, 160 core cycles per byte, and there is
 * no tenth point because the hardware has nothing above it. */
static const uint32_t BAUDS[] = {
    115200u, 230400u, 460800u, 921600u,
    2000000u, 4000000u, 6000000u, 8000000u, 9375000u,
};
#define N_BAUDS (sizeof(BAUDS) / sizeof(BAUDS[0]))

/* Core cycles between synthetic interrupts. Starts far below the UART's floor
 * of 160 and walks down past where the handler can possibly keep up. */
static const uint16_t SYNTH_CYCLES[] = {
    1500, 1000, 700, 500, 350, 250, 200, 160, 140, 120, 105, 90, 78, 68, 60,
};
#define N_SYNTH (sizeof(SYNTH_CYCLES) / sizeof(SYNTH_CYCLES[0]))

static const rx_mode_t MODES[] = {
    RX_MODE_PER_BYTE, RX_MODE_FIFO_TH, RX_MODE_DMA,
};
#define N_MODES (sizeof(MODES) / sizeof(MODES[0]))

/* How many bytes the hardware gathers before it interrupts, per mode. */
static const uint32_t BYTES_PER_IRQ[] = { 1u, 16u, 256u };

#define SETTLE_MS   200u    /* discarded: a divider change puts noise on the line */
#define MEASURE_MS  5000u
#define SYNTH_MS    2000u

#define LOAD_LIMIT_PPM  700000u   /* the 70 % rule, in parts per million */

/* One full cycle of frame counters, so the transmit DMA can loop the buffer
 * forever without the counter jumping at the seam. 256 x 69 bytes. */
#define TX_FRAMES  256u
static uint8_t s_tx_buf[TX_FRAMES * SENSOR_FRAME_BYTES];

static volatile sweep_kind_t s_request;
static volatile bool         s_abort;
static sweep_status_t        s_status;
static sweep_row_t           s_pending;
static volatile bool         s_have_pending;

void sweep_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_request = SWEEP_NONE;
    s_abort = false;
    s_have_pending = false;

    frame_crc_init();
    for (uint32_t f = 0; f < TX_FRAMES; f++) {
        frame_build_sensor(&s_tx_buf[f * SENSOR_FRAME_BYTES], (uint8_t)f);
    }
}

void sweep_request(sweep_kind_t kind)
{
    if (!s_status.running) {
        s_request = kind;
    }
}

void sweep_abort(void) { s_abort = true; }

void sweep_status(sweep_status_t *out) { *out = s_status; }

bool sweep_take_row(sweep_row_t *out)
{
    if (!s_have_pending) {
        return false;
    }
    *out = s_pending;
    s_have_pending = false;
    return true;
}

static void publish(const sweep_row_t *row)
{
    /* One row is produced every few seconds and consumed at 10 Hz, so a
     * single slot is plenty and a queue would be ceremony. */
    s_pending = *row;
    s_have_pending = true;
}

static uint32_t percentile_999(void)
{
    const uint32_t *h = engine_hist();
    uint64_t total = 0;
    for (uint32_t i = 0; i < ISR_HIST_BINS; i++) {
        total += h[i];
    }
    if (total == 0) {
        return 0;
    }
    uint64_t want = (total * 999u) / 1000u;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < ISR_HIST_BINS; i++) {
        seen += h[i];
        if (seen >= want) {
            return i * ISR_HIST_SCALE;
        }
    }
    return (ISR_HIST_BINS - 1u) * ISR_HIST_SCALE;
}

/* Turns two stat snapshots into a row. `window_us` is how long the window
 * really was, which for the synthetic sweep is computed from the interrupt
 * budget rather than from the clock - past saturation the scheduler tick has
 * stopped and wall time is not available to ask. */
static void finish(sweep_row_t *row, const engine_stats_t *ap,
                   const engine_stats_t *bp, uint32_t window_us,
                   uint32_t irq_period_cyc)
{
    const engine_stats_t a = *ap, b = *bp;
    const uint32_t clk = port_clk_hz();
    const uint32_t ms  = window_us / 1000u ? window_us / 1000u : 1u;

    const uint32_t irqs = b.isr.count - a.isr.count;
    const uint64_t cyc  = b.isr.sum - a.isr.sum;

    row->irq_period_cyc = irq_period_cyc;
    row->isr_min  = (b.isr.min == UINT32_MAX) ? 0u : b.isr.min;
    row->isr_max  = b.isr.max;
    row->isr_mean = irqs ? (uint32_t)(cyc / irqs) : 0u;
    row->isr_p999 = percentile_999();
    row->irq_per_s = (uint32_t)(((uint64_t)irqs * 1000u) / ms);

    /* Load the way the cycle counter sees it: how much of the core's time
     * went into handler bodies. */
    row->load_isr_ppm = (uint32_t)((cyc * 1000000u) / ((uint64_t)clk * ms / 1000u));

    /* Load the way the idle loop sees it, which is the honest one: it also
     * contains exception entry and exit, bus contention, and the tasks. */
    if (b.idle_ref_per_ms) {
        uint64_t expect = (uint64_t)b.idle_ref_per_ms * ms;
        uint64_t got    = b.idle_spins - a.idle_spins;
        row->load_total_ppm = (got >= expect) ? 0u
            : (uint32_t)(((expect - got) * 1000000u) / expect);
    }

    /* And the one that decides whether it broke: the worst single interrupt
     * against the time available before the next one. */
    row->worst_load_ppm = irq_period_cyc
        ? (uint32_t)(((uint64_t)row->isr_max * 1000000u) / irq_period_cyc)
        : 0u;

    row->overruns   = (b.uart_overrun - a.uart_overrun) +
                      (b.uart_overrun_hw - a.uart_overrun_hw);
    row->missed     = (b.rx_overflow - a.rx_overflow) +
                      (b.counter_gaps - a.counter_gaps);
    row->crc_errors = b.crc_errors - a.crc_errors;
    row->ring_peak  = b.rx_peak;

    row->broke = 0;
    if (row->overruns || row->missed) {
        row->broke |= BREAK_LOST_DATA;
    }
    if (row->worst_load_ppm > LOAD_LIMIT_PPM) {
        row->broke |= BREAK_LOAD_70;
    }
    if (irq_period_cyc && row->isr_max > irq_period_cyc) {
        row->broke |= BREAK_OVER_BUDGET;
    }
}

/* The real sweep: settle out the divider change, then watch for a while. */
static void measure_uart(sweep_row_t *row, uint32_t ms, uint32_t irq_period_cyc)
{
    engine_stats_t a, b;

    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    engine_hist_reset();
    engine_get_stats(&a);
    vTaskDelay(pdMS_TO_TICKS(ms));
    engine_get_stats(&b);

    finish(row, &a, &b, ms * 1000u, irq_period_cyc);
}

/* The synthetic sweep: hand the interrupt a fixed number of firings and let
 * it switch itself off. The window is then exactly budget x period, known
 * without consulting a clock that may have stopped advancing. */
static void measure_synth(sweep_row_t *row, uint32_t cycles)
{
    engine_stats_t a, b;
    const uint32_t clk = port_clk_hz();

    uint32_t budget = (uint32_t)(((uint64_t)clk * SYNTH_MS / 1000u) / cycles);
    if (budget > 2000000u) { budget = 2000000u; }
    if (budget < 20000u)   { budget = 20000u; }

    const uint32_t window_us =
        (uint32_t)(((uint64_t)budget * cycles) / (clk / 1000000u));

    engine_hist_reset();
    engine_get_stats(&a);
    port_synth_start(cycles, budget);

    /* Above saturation this task gets no CPU at all until the interrupt has
     * spent its budget, so the wait has to be patient rather than clever. */
    for (uint32_t i = 0; i < 400u && !port_synth_done(); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    port_synth_stop();
    engine_get_stats(&b);

    finish(row, &a, &b, window_us, cycles);
}

static void run_uart_sweep(uint16_t *point)
{
    const uint32_t clk = port_clk_hz();

    for (uint32_t m = 0; m < N_MODES && !s_abort; m++) {
        for (uint32_t i = 0; i < N_BAUDS && !s_abort; i++) {
            port_tx_dma_stop();
            uint32_t actual = port_set_baud(BAUDS[i]);
            port_set_rx_mode(MODES[m]);
            port_tx_dma_start(s_tx_buf, sizeof(s_tx_buf));

            s_status.point = (*point)++;
            s_status.baud  = actual;
            s_status.mode  = (uint8_t)MODES[m];

            sweep_row_t row;
            memset(&row, 0, sizeof(row));
            row.index       = s_status.point;
            row.mode        = (uint8_t)MODES[m];
            row.target_baud = BAUDS[i];
            row.actual_baud = actual;
            row.deviation_ppm = (int32_t)
                (((int64_t)actual - (int64_t)BAUDS[i]) * 1000000 / (int64_t)BAUDS[i]);
            row.byte_cycles = actual ? (uint32_t)(((uint64_t)clk * 10u) / actual) : 0u;

            measure_uart(&row, MEASURE_MS, row.byte_cycles * BYTES_PER_IRQ[m]);
            publish(&row);
        }
    }
    port_tx_dma_stop();
}

static void run_handler_sweep(uint16_t *point)
{
    for (uint32_t i = 0; i < N_SYNTH && !s_abort; i++) {
        s_status.point = (*point)++;
        s_status.baud  = 0;
        s_status.mode  = 0xFFu;

        sweep_row_t row;
        memset(&row, 0, sizeof(row));
        row.index       = s_status.point;
        row.mode        = 0xFFu;
        row.byte_cycles = SYNTH_CYCLES[i];
        /* What line rate would deliver a byte this often. */
        row.actual_baud = (uint32_t)(((uint64_t)port_clk_hz() * 10u)
                                     / SYNTH_CYCLES[i]);

        measure_synth(&row, SYNTH_CYCLES[i]);
        publish(&row);
    }
    port_synth_stop();
}

void sweep_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (s_request == SWEEP_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const sweep_kind_t kind = s_request;
        s_abort = false;
        s_status.running = 1;
        s_status.kind    = (uint8_t)kind;
        s_status.point   = 0;
        s_status.total   =
            (uint16_t)((kind == SWEEP_UART    ? N_MODES * N_BAUDS : 0u) +
                       (kind == SWEEP_HANDLER ? N_SYNTH : 0u) +
                       (kind == SWEEP_BOTH    ? N_MODES * N_BAUDS + N_SYNTH : 0u));

        /* The sweep owns the line while it runs. The normal measurement is
         * meaningless during it, and the dashboard says so. */
        engine_pause();

        uint16_t point = 0;
        if (kind == SWEEP_UART || kind == SWEEP_BOTH) {
            run_uart_sweep(&point);
        }
        if ((kind == SWEEP_HANDLER || kind == SWEEP_BOTH) && !s_abort) {
            run_handler_sweep(&point);
        }

        /* Put everything back the way the normal measurement needs it. */
        port_synth_stop();
        port_tx_dma_stop();
        port_set_baud(UART_BAUD);
        port_set_rx_mode(RX_MODE_PER_BYTE);
        engine_resume();

        s_status.running = 0;
        s_request = SWEEP_NONE;
    }
}
