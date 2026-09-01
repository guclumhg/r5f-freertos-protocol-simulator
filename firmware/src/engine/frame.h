/*
 * frame.h - The 64 byte sensor frame: build it, and verify it coming back.
 *
 * Portable C. This is the format the bridge carries, so it is the thing that
 * proves nothing was lost or corrupted on the way round.
 *
 *   offset  size  meaning
 *   0       1     0xA5  sync
 *   1       1     0x5A  sync
 *   2       2     frame counter, little endian, wraps at 2^16
 *   4       58    payload, derived from the counter so it is checkable
 *   62      2     CRC-16/CCITT-FALSE over bytes 0..61, little endian
 *
 * Payload bytes are always <= 0x7F, so the byte 0xA5 can never occur inside
 * a payload and the two sync bytes can never be forged by payload data.
 */
#ifndef R5F_FRAME_H
#define R5F_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define FRAME_SYNC0     0xA5u
#define FRAME_SYNC1     0x5Au
#define FRAME_LEN       SENSOR_FRAME_BYTES

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. */
uint16_t frame_crc16(const uint8_t *data, uint32_t len);

/* Writes one complete frame with the given counter value. */
void frame_build(uint8_t *out, uint16_t counter);

/* Streaming verifier. Feed it every received byte in order; it resynchronises
 * on the sync pair after an error. */
typedef struct {
    uint8_t  buf[FRAME_LEN];
    uint32_t fill;
    uint8_t  state;

    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t counter_gaps;   /* frames the counter says we never saw */
    uint32_t resyncs;

    bool     have_last;
    uint16_t last_counter;

    /* Most recent good frame, kept so the request/response path can answer
     * without touching the queue. */
    uint8_t  snapshot[FRAME_LEN];
    uint32_t snapshot_seq;
} frame_rx_t;

void frame_rx_init(frame_rx_t *fr);

/* Returns true when this byte completed a valid frame. */
bool frame_rx_byte(frame_rx_t *fr, uint8_t b);

#endif /* R5F_FRAME_H */
