#include "engine/frame.h"

#include <string.h>

enum { ST_SYNC0 = 0, ST_KIND, ST_COLLECT };

uint16_t frame_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void seal(uint8_t *out)
{
    uint16_t crc = frame_crc16(out, FRAME_LEN - 2u);
    out[FRAME_LEN - 2u] = (uint8_t)(crc & 0xFFu);
    out[FRAME_LEN - 1u] = (uint8_t)(crc >> 8);
}

void frame_build_sensor(uint8_t *out, uint16_t counter)
{
    out[0] = FRAME_SYNC0;
    out[1] = FRAME_KIND_SENSOR;
    out[2] = (uint8_t)(counter & 0xFFu);
    out[3] = (uint8_t)(counter >> 8);

    /* Masked to 7 bits so 0xA5 can never appear in the payload. */
    for (uint32_t i = 0; i < FRAME_LEN - 6u; i++) {
        out[4 + i] = (uint8_t)((counter + i) & 0x7Fu);
    }
    seal(out);
}

void frame_build_bridge(uint8_t *out, uint8_t unit_index, uint16_t burst_seq)
{
    out[0] = FRAME_SYNC0;
    out[1] = FRAME_KIND_BRIDGE;
    out[2] = unit_index;
    out[3] = (uint8_t)(burst_seq & 0x7Fu);

    for (uint32_t i = 0; i < FRAME_LEN - 6u; i++) {
        out[4 + i] = (uint8_t)((burst_seq + unit_index + i) & 0x7Fu);
    }
    seal(out);
}

void frame_rx_init(frame_rx_t *fr)
{
    memset(fr, 0, sizeof(*fr));
    fr->state = ST_SYNC0;
}

static void check_sensor(frame_rx_t *fr)
{
    uint16_t counter = (uint16_t)fr->buf[2] |
                       (uint16_t)((uint16_t)fr->buf[3] << 8);

    if (fr->have_last) {
        uint16_t expected = (uint16_t)(fr->last_counter + 1u);
        if (counter != expected) {
            /* Unsigned wrap gives the right gap even across 0xFFFF. */
            fr->counter_gaps += (uint16_t)(counter - expected);
        }
    }
    fr->have_last    = true;
    fr->last_counter = counter;
    fr->frames_ok++;

    memcpy(fr->snapshot, fr->buf, FRAME_LEN);
    fr->snapshot_seq++;
}

static void check_bridge(frame_rx_t *fr)
{
    uint8_t unit = fr->buf[2];
    uint8_t seq  = fr->buf[3];

    if (fr->have_last_unit) {
        bool ok;
        if (fr->last_unit_index + 1u < CAN_BURST_BYTES / FRAME_LEN) {
            /* still inside a burst: the next unit must follow */
            ok = (unit == fr->last_unit_index + 1u) && (seq == fr->last_burst_seq);
        } else {
            /* end of a burst: the next unit must open the next one */
            ok = (unit == 0u) && (seq == (uint8_t)((fr->last_burst_seq + 1u) & 0x7Fu));
        }
        if (!ok) {
            fr->isotp_errors++;
        }
    }
    fr->have_last_unit  = true;
    fr->last_unit_index = unit;
    fr->last_burst_seq  = seq;
    fr->units_ok++;
}

bool frame_rx_byte(frame_rx_t *fr, uint8_t b)
{
    switch (fr->state) {
    case ST_SYNC0:
        if (b == FRAME_SYNC0) {
            fr->buf[0] = b;
            fr->state  = ST_KIND;
        }
        return false;

    case ST_KIND:
        if (b == FRAME_KIND_SENSOR || b == FRAME_KIND_BRIDGE) {
            fr->buf[1] = b;
            fr->kind   = b;
            fr->fill   = 2;
            fr->state  = ST_COLLECT;
        } else if (b == FRAME_SYNC0) {
            /* stay here: A5 A5 5A is still a valid opening */
        } else {
            fr->state = ST_SYNC0;
        }
        return false;

    case ST_COLLECT:
    default:
        fr->buf[fr->fill++] = b;
        if (fr->fill < FRAME_LEN) {
            return false;
        }
        fr->state = ST_SYNC0;
        break;
    }

    uint16_t want = frame_crc16(fr->buf, FRAME_LEN - 2u);
    uint16_t got  = (uint16_t)fr->buf[FRAME_LEN - 2u] |
                    (uint16_t)((uint16_t)fr->buf[FRAME_LEN - 1u] << 8);

    if (want != got) {
        if (fr->kind == FRAME_KIND_BRIDGE) {
            fr->unit_crc_errors++;
        } else {
            fr->crc_errors++;
        }
        fr->resyncs++;
        return false;
    }

    if (fr->kind == FRAME_KIND_BRIDGE) {
        check_bridge(fr);
    } else {
        check_sensor(fr);
    }
    return true;
}
