/*
 * frame.h - The 64 byte units the wire carries, and the receiver that checks
 * them.
 *
 * Portable C. Two kinds of unit share the line, told apart by the second sync
 * byte:
 *
 *   A5 5A   sensor frame  - the 180 Hz stream that fills the line
 *   A5 C3   bridge unit   - 64 bytes of the CAN side's ISO-TP payload
 *
 * Both kinds are the same length and both are CRC protected. They need to be
 * distinguishable because the UART is in loopback: everything the transmitter
 * sends comes back to the receiver, so the receiver sees the two streams
 * interleaved and has to demultiplex them the way the far end of a real link
 * would.
 *
 *   offset  size  sensor frame              bridge unit
 *   0       1     0xA5                      0xA5
 *   1       1     0x5A                      0xC3
 *   2       2     frame counter (LE)        unit index | burst sequence
 *   4       58    payload from the counter  payload from burst and unit
 *   62      2     CRC-16/CCITT-FALSE, little endian, over bytes 0..61
 *
 * Payload bytes are always <= 0x7F, so 0xA5 can never appear inside a payload
 * and no payload can forge a sync pair.
 */
#ifndef R5F_FRAME_H
#define R5F_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#define FRAME_SYNC0        0xA5u
#define FRAME_KIND_SENSOR  0x5Au
#define FRAME_KIND_BRIDGE  0xC3u
#define FRAME_LEN          SENSOR_FRAME_BYTES

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. */
uint16_t frame_crc16(const uint8_t *data, uint32_t len);

void frame_build_sensor(uint8_t *out, uint16_t counter);
void frame_build_bridge(uint8_t *out, uint8_t unit_index, uint16_t burst_seq);

/* Streaming demultiplexer and verifier. Feed it every received byte in order;
 * it resynchronises on a sync pair after an error. */
typedef struct {
    uint8_t  buf[FRAME_LEN];
    uint32_t fill;
    uint8_t  state;
    uint8_t  kind;

    /* sensor stream */
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t counter_gaps;
    bool     have_last;
    uint16_t last_counter;

    /* bridge stream: a gap here is an ISO-TP reassembly failure */
    uint32_t units_ok;
    uint32_t unit_crc_errors;
    uint32_t isotp_errors;
    bool     have_last_unit;
    uint8_t  last_unit_index;
    uint8_t  last_burst_seq;

    uint32_t resyncs;

    /* Most recent good sensor frame, so the request/response path can answer
     * without disturbing any queue. */
    uint8_t  snapshot[FRAME_LEN];
    uint32_t snapshot_seq;
} frame_rx_t;

void frame_rx_init(frame_rx_t *fr);

/* Returns true when this byte completed a valid unit of either kind. */
bool frame_rx_byte(frame_rx_t *fr, uint8_t b);

#endif /* R5F_FRAME_H */
