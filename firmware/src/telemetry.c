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
                    "\"budget_cycles\":%u,\"byte_time_ns\":%u},",
            (unsigned long)((unsigned long)xTaskGetTickCount() * portTICK_PERIOD_MS),
            (unsigned)seq++,
            (unsigned)port_clk_hz(), (unsigned)port_actual_baud(),
            port_loopback_mode() == LOOPBACK_INTERNAL ? "internal" : "external",
            port_cycles_ok() ? 1 : 0,
            (unsigned)BYTE_TIME_CYCLES, (unsigned)BYTE_TIME_NS);

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
                       "\"built\":%u},",
            (unsigned)s_stats.frames_ok, (unsigned)s_stats.crc_errors,
            (unsigned)s_stats.counter_gaps, (unsigned)s_stats.resyncs,
            (unsigned)s_stats.frames_built);

        cpu_json(&w);

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

        engine_reset_window();
    }
}
