/*
 * telemetry.c - One line of JSON out of the USB serial port, ten times a
 * second, from the lowest priority task in the system.
 *
 * Everything is an integer. The firmware ships raw cycle counts plus the
 * clock rate it measured, and the host does the division into microseconds.
 * A wrong assumption about the clock then shows up as a wrong clock on the
 * dashboard instead of as quietly wrong timings.
 */
#include "telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "config.h"
#include "engine/engine.h"
#include "port/port.h"
#include "sweep.h"

#define MAX_TASKS   8
#define LINE_BYTES  3072

/* Bounded appender. Once the buffer is full it stops writing and records
 * that it truncated, so a long burst trace can never run off the end. */
typedef struct {
    char *buf;
    int   cap;
    int   len;
    bool  truncated;
} writer_t;

static void w_init(writer_t *w, char *buf, int cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->truncated = false;
    if (cap > 0) { buf[0] = '\0'; }
}

static void w_printf(writer_t *w, const char *fmt, ...)
{
    if (w->truncated || w->len >= w->cap - 1) {
        w->truncated = true;
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->buf + w->len, (size_t)(w->cap - w->len), fmt, ap);
    va_end(ap);

    if (n < 0 || n >= w->cap - w->len) {
        w->truncated = true;
        w->len = w->cap - 1;
        w->buf[w->len] = '\0';
    } else {
        w->len += n;
    }
}

static char           s_line[LINE_BYTES];
static engine_stats_t s_stats;

/* Previous run time counters, so CPU usage is per window rather than since
 * boot. */
static uint32_t    s_prev_run[MAX_TASKS];
static char        s_prev_name[MAX_TASKS][configMAX_TASK_NAME_LEN];
static uint32_t    s_prev_total;
static UBaseType_t s_prev_n;

static uint32_t s_last_trace_seq;

static void cpu_json(writer_t *w)
{
    static TaskStatus_t status[MAX_TASKS];
    uint32_t total = 0;

    UBaseType_t n = uxTaskGetSystemState(status, MAX_TASKS, &total);
    uint32_t d_total = total - s_prev_total;

    w_printf(w, "\"cpu\":[");

    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t prev = 0;
        for (UBaseType_t j = 0; j < s_prev_n; j++) {
            if (strncmp(s_prev_name[j], status[i].pcTaskName,
                        configMAX_TASK_NAME_LEN) == 0) {
                prev = s_prev_run[j];
                break;
            }
        }

        uint32_t d_task   = status[i].ulRunTimeCounter - prev;
        uint32_t permille = d_total
            ? (uint32_t)(((uint64_t)d_task * 1000u) / d_total)
            : 0u;

        w_printf(w, "%s{\"n\":\"%s\",\"p\":%u}",
                 i ? "," : "", status[i].pcTaskName, (unsigned)permille);
    }
    w_printf(w, "]");

    for (UBaseType_t i = 0; i < n && i < MAX_TASKS; i++) {
        s_prev_run[i] = status[i].ulRunTimeCounter;
        strncpy(s_prev_name[i], status[i].pcTaskName,
                configMAX_TASK_NAME_LEN - 1);
        s_prev_name[i][configMAX_TASK_NAME_LEN - 1] = '\0';
    }
    s_prev_n     = (n < MAX_TASKS) ? n : MAX_TASKS;
    s_prev_total = total;
}

void telemetry_task(void *arg)
{
    (void)arg;
    uint32_t   seq  = 0;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(1000u / TELEMETRY_HZ));

        engine_get_stats(&s_stats);

        writer_t w;
        w_init(&w, s_line, LINE_BYTES);

        w_printf(&w,
            "{\"v\":1,\"t\":%lu,\"seq\":%u,"
            "\"hw\":{\"clk\":%u,\"baud\":%u,\"loopback\":\"%s\",\"cyccnt\":%d,"
                    "\"budget_cycles\":%u,\"byte_time_ns\":%u},"
            /* The dashboard draws its reference lines from these rather than
             * from constants of its own, so config.h stays the single source
             * of truth all the way out to the browser. */
            "\"limits\":{\"ring\":%u,\"expect_peak\":%u,\"burst_ms_x10\":%u,"
                        "\"frame_bytes\":%u,"
                        "\"sensor_hz_x100\":%u,\"sensor_hz_burst_x100\":%u},",
            (unsigned long)((unsigned long)xTaskGetTickCount() * portTICK_PERIOD_MS),
            (unsigned)seq++,
            (unsigned)port_clk_hz(), (unsigned)port_actual_baud(),
            port_loopback_mode() == LOOPBACK_INTERNAL ? "internal" : "external",
            port_cycles_ok() ? 1 : 0,
            (unsigned)BYTE_TIME_CYCLES, (unsigned)BYTE_TIME_NS,
            (unsigned)RING_BYTES, (unsigned)EXPECTED_PEAK_BACKLOG,
            (unsigned)(CAN_BURST_NS / 100000u),
            (unsigned)SENSOR_FRAME_BYTES,
            (unsigned)SENSOR_HZ_X100_IDLE,
            (unsigned)SENSOR_HZ_X100_LOADED);

        w_printf(&w,
            "\"isr\":{\"last\":%u,\"min\":%u,\"mean\":%u,\"max\":%u,\"n\":%u},",
            (unsigned)s_stats.isr.last,
            (unsigned)(s_stats.isr.min == UINT32_MAX ? 0u : s_stats.isr.min),
            (unsigned)stat_mean(&s_stats.isr),
            (unsigned)s_stats.isr.max,
            (unsigned)s_stats.isr.count);

        w_printf(&w,
            "\"rx\":{\"cur\":%u,\"peak\":%u,\"ovf\":%u},"
            "\"bridge\":{\"cur\":%u,\"peak\":%u,\"ovf\":%u},",
            (unsigned)s_stats.rx_count, (unsigned)s_stats.rx_peak,
            (unsigned)s_stats.rx_overflow,
            (unsigned)s_stats.bridge_count, (unsigned)s_stats.bridge_peak,
            (unsigned)s_stats.bridge_overflow);

        w_printf(&w,
            "\"wire\":{\"rx\":%u,\"tx_sensor\":%u,\"tx_bridge\":%u,"
                     "\"idle\":%u,\"stall\":%u,\"ovr\":%u,\"ovr_hw\":%u},",
            (unsigned)s_stats.bytes_rx, (unsigned)s_stats.bytes_tx_sensor,
            (unsigned)s_stats.bytes_tx_bridge, (unsigned)s_stats.idle_ticks,
            (unsigned)s_stats.tx_stalls, (unsigned)s_stats.uart_overrun,
            (unsigned)s_stats.uart_overrun_hw);

        w_printf(&w,
            "\"frames\":{\"ok\":%u,\"crc\":%u,\"gaps\":%u,\"resync\":%u,"
                       "\"payload\":%u,\"built\":%u},",
            (unsigned)s_stats.frames_ok, (unsigned)s_stats.crc_errors,
            (unsigned)s_stats.counter_gaps, (unsigned)s_stats.resyncs,
            (unsigned)s_stats.payload_errors, (unsigned)s_stats.frames_built);

        w_printf(&w,
            "\"can\":{\"bursts\":%u,\"frames\":%u,\"data\":%u,\"active\":%d,"
                    "\"reassembled\":%u,\"isotp_err\":%u},",
            (unsigned)s_stats.can_bursts, (unsigned)s_stats.can_frames,
            (unsigned)s_stats.can_data_bytes, s_stats.burst_active ? 1 : 0,
            (unsigned)s_stats.bridge_bursts, (unsigned)s_stats.isotp_errors);

        w_printf(&w,
            "\"req\":{\"sent\":%u,\"answered\":%u,\"failed\":%u,"
                    "\"bytes\":%u,\"lat_mean\":%u,\"lat_max\":%u},",
            (unsigned)s_stats.requests, (unsigned)s_stats.responses,
            (unsigned)s_stats.response_failures,
            (unsigned)s_stats.response_bytes,
            (unsigned)stat_mean(&s_stats.response_latency),
            (unsigned)s_stats.response_latency.max);

        cpu_json(&w);

        /* Sweep progress rides along on every packet, so the sweep page can
         * show where it is without a second connection. */
        sweep_status_t sw;
        sweep_status(&sw);
        if (sw.running || sw.point) {
            w_printf(&w, ",\"sweep\":{\"run\":%u,\"kind\":%u,\"point\":%u,"
                         "\"total\":%u,\"baud\":%u,\"mode\":%u}",
                     sw.running, sw.kind, sw.point, sw.total,
                     (unsigned)sw.baud, sw.mode);
        }

        /* A completed measurement point, with the interrupt duration
         * histogram that produced its percentile. Only the populated part of
         * the histogram goes out. */
        sweep_row_t row;
        if (sweep_take_row(&row)) {
            w_printf(&w,
                ",\"row\":{\"i\":%u,\"mode\":%u,\"broke\":%u,"
                "\"target\":%u,\"baud\":%u,\"dev_ppm\":%d,"
                "\"byte_cyc\":%u,\"irq_cyc\":%u,"
                "\"isr\":[%u,%u,%u,%u],\"irq_s\":%u,"
                "\"load_isr\":%u,\"load_total\":%u,\"load_worst\":%u,"
                "\"ovr\":%u,\"missed\":%u,\"crc\":%u,\"peak\":%u,"
                "\"win_us\":%u,\"bytes\":%u,\"budget\":%u",
                row.index, row.mode, row.broke,
                (unsigned)row.target_baud, (unsigned)row.actual_baud,
                (int)row.deviation_ppm,
                (unsigned)row.byte_cycles, (unsigned)row.irq_period_cyc,
                (unsigned)row.isr_min, (unsigned)row.isr_mean,
                (unsigned)row.isr_max, (unsigned)row.isr_p999,
                (unsigned)row.irq_per_s,
                (unsigned)row.load_isr_ppm, (unsigned)row.load_total_ppm,
                (unsigned)row.worst_load_ppm,
                (unsigned)row.overruns, (unsigned)row.missed,
                (unsigned)row.crc_errors, (unsigned)row.ring_peak,
                (unsigned)row.window_us, (unsigned)row.bytes_rx,
                (unsigned)row.budget);

            const uint32_t *h = engine_hist();
            uint32_t lo = 0, hi = 0;
            for (uint32_t i = 0; i < ISR_HIST_BINS; i++) {
                if (h[i]) { if (!hi) { lo = i; } hi = i; }
            }
            w_printf(&w, ",\"hist\":{\"lo\":%u,\"scale\":%u,\"d\":[",
                     (unsigned)lo, (unsigned)ISR_HIST_SCALE);
            for (uint32_t i = lo; i <= hi; i++) {
                w_printf(&w, "%s%u", i > lo ? "," : "", (unsigned)h[i]);
            }
            w_printf(&w, "]}}");
        }

        /* One character from the host starts or stops a sweep. Non blocking:
         * if nothing is waiting we carry straight on. */
        int cmd = getchar_timeout_us(0);
        switch (cmd) {
        case 'U': sweep_request(SWEEP_UART);    break;
        case 'H': sweep_request(SWEEP_HANDLER); break;
        case 'B': sweep_request(SWEEP_BOTH);    break;
        case 'X': sweep_abort();                break;
        default:  break;
        }

        /* The burst trace only goes out on the packet after a burst finished,
         * so it does not repeat and does not bloat every line. */
        if (s_stats.trace_len && s_stats.trace_seq != s_last_trace_seq) {
            s_last_trace_seq = s_stats.trace_seq;
            w_printf(&w, ",\"trace\":{\"seq\":%u,\"us\":%u,\"d\":[",
                     (unsigned)s_stats.trace_seq,
                     (unsigned)s_stats.trace_period_us);
            for (uint16_t i = 0; i < s_stats.trace_len; i++) {
                w_printf(&w, "%s%u", i ? "," : "", (unsigned)s_stats.trace[i]);
            }
            w_printf(&w, "]}");
        }

        w_printf(&w, "}\n");

        /* Drop the line rather than block if nobody is listening, and drop it
         * if it got truncated rather than emit half a JSON object. The whole
         * point of running this at the bottom priority is that it can never
         * affect the numbers it reports. */
        if (!w.truncated && w.len > 0 && stdio_usb_connected()) {
            fwrite(s_line, 1, (size_t)w.len, stdout);
            fflush(stdout);
        }

        /* The sweep resets its own window and reads the extremes over it.
         * Clearing them here every 100 ms would hand it the wrong numbers. */
        if (!sw.running) {
            engine_reset_window();
        }
    }
}
