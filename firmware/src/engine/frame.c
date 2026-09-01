#include "engine/frame.h"

#include <string.h>

enum { ST_SYNC0 = 0, ST_SYNC1, ST_COLLECT };

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

void frame_build(uint8_t *out, uint16_t counter)
{
    out[0] = FRAME_SYNC0;
    out[1] = FRAME_SYNC1;
    out[2] = (uint8_t)(counter & 0xFFu);
    out[3] = (uint8_t)(counter >> 8);

    /* Masked to 7 bits so 0xA5 can never appear in the payload. */
    for (uint32_t i = 0; i < FRAME_LEN - 6u; i++) {
        out[4 + i] = (uint8_t)((counter + i) & 0x7Fu);
    }

    uint16_t crc = frame_crc16(out, FRAME_LEN - 2u);
    out[FRAME_LEN - 2u] = (uint8_t)(crc & 0xFFu);
    out[FRAME_LEN - 1u] = (uint8_t)(crc >> 8);
}

void frame_rx_init(frame_rx_t *fr)
{
    memset(fr, 0, sizeof(*fr));
    fr->state = ST_SYNC0;
}

bool frame_rx_byte(frame_rx_t *fr, uint8_t b)
{
    switch (fr->state) {
    case ST_SYNC0:
        if (b == FRAME_SYNC0) {
            fr->buf[0] = b;
            fr->state  = ST_SYNC1;
        }
        return false;

    case ST_SYNC1:
        if (b == FRAME_SYNC1) {
            fr->buf[1] = b;
            fr->fill   = 2;
            fr->state  = ST_COLLECT;
        } else if (b == FRAME_SYNC0) {
            /* stay here: 0xA5 0xA5 0x5A is still a valid opening */
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

    /* A full frame is in buf. */
    uint16_t want = frame_crc16(fr->buf, FRAME_LEN - 2u);
    uint16_t got  = (uint16_t)fr->buf[FRAME_LEN - 2u] |
                    (uint16_t)((uint16_t)fr->buf[FRAME_LEN - 1u] << 8);

    if (want != got) {
        fr->crc_errors++;
        fr->resyncs++;
        return false;
    }

    uint16_t counter = (uint16_t)fr->buf[2] | (uint16_t)((uint16_t)fr->buf[3] << 8);

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
    return true;
}
