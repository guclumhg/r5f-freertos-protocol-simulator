/*
 * ringbuf.h - Single-producer / single-consumer byte ring.
 *
 * Portable C. No hardware, no RTOS, no locks.
 *
 * Every ring in this system has exactly one writer and exactly one reader,
 * and the writer either is an interrupt that preempts the reader or is a
 * task that the reader's interrupt preempts. On a single core that means the
 * two never truly overlap - they interleave - so program order plus a
 * compiler barrier is sufficient and no critical section is needed. This is
 * why the receive interrupt can stay at the highest priority in the system
 * without ever masking anything.
 *
 * head and tail are free-running and only masked when indexing, so the
 * difference between them is the occupancy even across the 2^32 wrap.
 */
#ifndef R5F_RINGBUF_H
#define R5F_RINGBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RB_BARRIER() __asm volatile("" ::: "memory")

typedef struct {
    uint8_t  *buf;
    uint32_t  mask;              /* size - 1; size must be a power of two */
    volatile uint32_t head;      /* written by the producer only */
    volatile uint32_t tail;      /* written by the consumer only */
    volatile uint32_t peak;      /* high water mark, producer only */
    volatile uint32_t overflow;  /* bytes dropped, producer only */
} ringbuf_t;

void rb_init(ringbuf_t *rb, uint8_t *storage, uint32_t size_pow2);

/* Occupancy. Safe to call from either side; may be a moment out of date. */
static inline uint32_t rb_count(const ringbuf_t *rb)
{
    return rb->head - rb->tail;
}

static inline uint32_t rb_free(const ringbuf_t *rb)
{
    return (rb->mask + 1u) - (rb->head - rb->tail);
}

static inline bool rb_empty(const ringbuf_t *rb)
{
    return rb->head == rb->tail;
}

/* Producer side. Returns false and bumps the overflow counter if full. */
static inline bool rb_push(ringbuf_t *rb, uint8_t b)
{
    uint32_t head  = rb->head;
    uint32_t count = head - rb->tail;

    if (count > rb->mask) {          /* count == size means full */
        rb->overflow++;
        return false;
    }

    rb->buf[head & rb->mask] = b;
    RB_BARRIER();                    /* byte must land before it is visible */
    rb->head = head + 1u;

    if (count + 1u > rb->peak) {
        rb->peak = count + 1u;
    }
    return true;
}

/* Consumer side. Returns false if empty. */
static inline bool rb_pop(ringbuf_t *rb, uint8_t *out)
{
    uint32_t tail = rb->tail;

    if (rb->head == tail) {
        return false;
    }

    *out = rb->buf[tail & rb->mask];
    RB_BARRIER();
    rb->tail = tail + 1u;
    return true;
}

/* Clears the high water mark so the dashboard can show a peak since reset
 * as well as a peak since boot. Consumer side. */
static inline void rb_clear_peak(ringbuf_t *rb)
{
    rb->peak = rb_count(rb);
}

#endif /* R5F_RINGBUF_H */
