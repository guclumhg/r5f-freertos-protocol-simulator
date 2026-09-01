/*
 * frame.h - The sensor frame, and the receiver that checks what comes back.
 *
 * Portable C.
 *
 * The frame layout is the one given in the specification: two header bytes,
 * one counter byte, sixty four bytes of data, two checksum bytes. Sixty nine
 * bytes on the wire.
 *
 *   offset  size  meaning
 *   0       1     0xA5   header
 *   1       1     0x5A   header
 *   2       1     frame counter
 *   3..66   64    data, derived from the counter so it is checkable
 *   67..68  2     CRC-16/CCITT-FALSE, little endian, over bytes 0..66
 *
 * Two streams share the line, because the UART is in loopback and everything
 * transmitted comes straight back. They are told apart without any framing on
 * the bridge side at all:
 *
 *   - Every byte a sensor frame can contain after its header is <= 0x7F, and
 *     so is every byte the bridge sends. 0xA5 therefore only ever appears as
 *     the first header byte of a real frame.
 *   - So the receiver scans for 0xA5 0x5A, and simply walks over bridge bytes
 *     while it is looking. They cost the bridge nothing on the wire, which is
 *     why the 1024 bytes a burst delivers are exactly the 1024 bytes that sit
 *     in the ring - and why the 796 byte arithmetic holds unmodified.
 *
 * The bridge bytes are not ignored: they carry a counting pattern, and the
 * receiver checks it. A burst is 1024 of them in one contiguous run, so a lost
 * or corrupted byte breaks the count and is reported as an ISO-TP reassembly
 * error.
 *
 * The frame counter is one byte, so it wraps every 256 frames - about 1.5
 * seconds. Gap detection is unsigned 8 bit arithmetic and stays correct across
 * the wrap; the only loss it cannot see is exactly 256 consecutive frames,
 * which every other counter would have screamed about first.
 */
#ifndef R5F_FRAME_H
#define R5F_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define FRAME_SYNC0   0xA5u
#define FRAME_SYNC1   0x5Au
#define FRAME_LEN     SENSOR_FRAME_BYTES        /* 69 */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. */
uint16_t frame_crc16(const uint8_t *data, uint32_t len);

/* Builds one complete 69 byte frame. */
void frame_build_sensor(uint8_t *out, uint8_t counter);

/* The byte the bridge should be sending at this position of a burst. Used by
 * both the generator and the checker, so they cannot drift apart. */
static inline uint8_t bridge_byte_at(uint32_t pos)
{
    return (uint8_t)(pos & 0x7Fu);
}

typedef struct {
    uint8_t  buf[FRAME_LEN];
    uint32_t fill;
    uint8_t  state;

    /* sensor stream */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t counter_gaps;
    uint32_t resyncs;
    bool     have_last;
    uint8_t  last_counter;

    /* bridge stream, checked without any framing of its own */
    uint32_t bridge_bytes;      /* seen in the current burst, 0..1023 */
    uint32_t bridge_bursts;     /* complete 1024 byte runs reassembled */
    uint32_t isotp_errors;      /* a broken count, or a run cut short */

    /* Most recent good sensor frame, so the request/response path can answer
     * without disturbing any queue. */
    uint8_t  snapshot[FRAME_LEN];
    uint32_t snapshot_seq;
} frame_rx_t;

void frame_rx_init(frame_rx_t *fr);

/* Returns true when this byte completed a valid sensor frame. */
bool frame_rx_byte(frame_rx_t *fr, uint8_t b);

#endif /* R5F_FRAME_H */
