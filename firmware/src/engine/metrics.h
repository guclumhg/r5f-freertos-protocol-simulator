/*
 * metrics.h - Running min / mean / max, cheap enough to update in an ISR.
 *
 * Values are in core clock cycles. Nothing here converts to microseconds:
 * the firmware ships raw cycles and the measured clock rate, and the host
 * does the division. That way a wrong assumption about the clock shows up as
 * a wrong clock in the telemetry rather than as quietly wrong microseconds.
 */
#ifndef R5F_METRICS_H
#define R5F_METRICS_H

#include <stdint.h>

typedef struct {
    uint32_t last;
    uint32_t min;
    uint32_t max;
    uint64_t sum;
    uint32_t count;
} stat_t;

static inline void stat_reset(stat_t *s)
{
    s->last  = 0;
    s->min   = UINT32_MAX;
    s->max   = 0;
    s->sum   = 0;
    s->count = 0;
}

static inline void stat_add(stat_t *s, uint32_t v)
{
    s->last = v;
    if (v < s->min) { s->min = v; }
    if (v > s->max) { s->max = v; }
    s->sum  += v;
    s->count++;
}

static inline uint32_t stat_mean(const stat_t *s)
{
    return s->count ? (uint32_t)(s->sum / s->count) : 0u;
}

/* Clears max/min without losing the lifetime mean, so the dashboard can show
 * "worst case since boot" next to "worst case in the last window". */
static inline void stat_clear_extremes(stat_t *s)
{
    s->min = UINT32_MAX;
    s->max = 0;
}

#endif /* R5F_METRICS_H */
