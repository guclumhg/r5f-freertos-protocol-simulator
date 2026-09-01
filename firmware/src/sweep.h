/*
 * sweep.h - Find where the per-byte interrupt architecture stops working.
 *
 * Two sweeps, because one of them cannot answer the question on its own.
 *
 * SWEEP A, the UART sweep, walks the real baud rates through the real
 * peripheral in three receive modes. It answers "does a UART bridge work at
 * this rate", and the answer on this chip is yes, everywhere - because a
 * PL011 oversamples by 16, so its ceiling is clk_peri/16 and a byte can never
 * arrive faster than one per 160 core clocks. Raising the system clock raises
 * both sides of that ratio. The UART physically cannot generate enough
 * interrupts to saturate this core.
 *
 * SWEEP B, the handler sweep, drives the same receive workload from a timer,
 * past any rate a UART can produce, until it genuinely breaks. That is the
 * number worth having, and it is reported in core cycles per byte rather than
 * in baud, because cycles per byte is independent of the clock and therefore
 * portable to the target:
 *
 *     cycles available per byte = core clock x 10 / line rate
 *
 * Below whatever that number turns out to be, this architecture is finished
 * and the work has to move - to DMA, then to hardware.
 */
#ifndef R5F_SWEEP_H
#define R5F_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "port/port.h"

typedef enum {
    SWEEP_NONE = 0,
    SWEEP_UART,        /* real bauds, three receive modes */
    SWEEP_HANDLER,     /* synthetic, past what the UART can do */
    SWEEP_BOTH,
} sweep_kind_t;

/* Which of the three criteria tripped. Any of them means broken. */
#define BREAK_LOST_DATA   0x1u   /* an overrun, a dropped byte, a gap */
#define BREAK_LOAD_70     0x2u   /* worst-case load over 70 % */
#define BREAK_OVER_BUDGET 0x4u   /* one interrupt longer than one byte time */

typedef struct {
    uint8_t  mode;            /* rx_mode_t, or 0xFF for the synthetic sweep */
    uint8_t  broke;           /* BREAK_* bitmask, 0 if it held */
    uint16_t index;

    uint32_t target_baud;     /* 0 in the synthetic sweep */
    uint32_t actual_baud;
    int32_t  deviation_ppm;
    uint32_t byte_cycles;     /* core cycles between bytes */
    uint32_t irq_period_cyc;  /* core cycles between interrupts */

    uint32_t isr_min, isr_mean, isr_max, isr_p999;
    uint32_t irq_per_s;

    uint32_t load_isr_ppm;    /* interrupts x mean duration, over the clock */
    uint32_t load_total_ppm;  /* from the idle deficit: misses nothing */
    uint32_t worst_load_ppm;  /* worst interrupt against its own period */

    uint32_t overruns;
    uint32_t missed;          /* ring overflow plus counter gaps */
    uint32_t crc_errors;
    uint32_t ring_peak;

    /* Raw, for diagnosis: how long the window really was and how many bytes
     * actually arrived in it. */
    uint32_t window_us;
    uint32_t bytes_rx;
    uint32_t budget;
} sweep_row_t;

void sweep_init(void);
void sweep_task(void *arg);

/* Asked for by the dashboard. Ignored while a sweep is already running. */
void sweep_request(sweep_kind_t kind);
void sweep_abort(void);

typedef struct {
    uint8_t  running;
    uint8_t  kind;
    uint16_t point;
    uint16_t total;
    uint32_t baud;
    uint8_t  mode;
} sweep_status_t;

void sweep_status(sweep_status_t *out);

/* Returns true and fills `out` once per completed point, then forgets it. */
bool sweep_take_row(sweep_row_t *out);

#endif /* R5F_SWEEP_H */
