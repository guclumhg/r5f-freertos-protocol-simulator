#include "engine/ringbuf.h"

void rb_init(ringbuf_t *rb, uint8_t *storage, uint32_t size_pow2)
{
    rb->buf      = storage;
    rb->mask     = size_pow2 - 1u;
    rb->head     = 0;
    rb->tail     = 0;
    rb->peak     = 0;
    rb->overflow = 0;
}
