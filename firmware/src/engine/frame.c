#include "engine/frame.h"

#include <string.h>

enum { ST_SYNC0 = 0, ST_SYNC1, ST_COLLECT };

/* Table driven, and the table lives in RAM.
 *
 * The bitwise version cost about forty cycles per byte. That is invisible at
 * 115200 baud and fatal above about two megabaud: the verifier would saturate
 * a whole task before the receive interrupt came anywhere near its limit, and
 * the checksum errors that followed would look like a protocol failure rather
 * than what they were - the checker failing to keep up. Four cycles per byte
 * keeps the thing being measured the thing being measured. */
static uint16_t s_crc_table[256];
static bool     s_crc_ready;

void frame_crc_init(void)
{
    for (uint32_t i = 0; i < 256u; i++) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
        s_crc_table[i] = crc;
    }
    s_crc_ready = true;
}

uint16_t frame_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFu;

    for (uint32_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc << 8) ^
                         s_crc_table[((crc >> 8) ^ data[i]) & 0xFFu]);
    }
    return crc;
}

void frame_build_sensor(uint8_t *out, uint8_t counter)
{
    out[0] = FRAME_SYNC0;
    out[1] = FRAME_SYNC1;
    out[2] = counter;

    /* Masked to 7 bits, so 0xA5 can never occur inside the data and no data
     * pattern can forge the header. */
    for (uint32_t i = 0; i < FRAME_PAYLOAD_BYTES; i++) {
        out[FRAME_HEADER_BYTES + FRAME_COUNTER_BYTES + i] =
            frame_payload_byte(counter, i);
    }

    uint16_t crc = frame_crc16(out, FRAME_LEN - FRAME_CRC_BYTES);
    out[FRAME_LEN - 2u] = (uint8_t)(crc & 0xFFu);
    out[FRAME_LEN - 1u] = (uint8_t)(crc >> 8);
}

void frame_rx_init(frame_rx_t *fr)
{
    if (!s_crc_ready) {
        frame_crc_init();
    }
    memset(fr, 0, sizeof(*fr));
    fr->state = ST_SYNC0;
}

/* A byte that is not part of a sensor frame belongs to the bridge. Check it
 * against the pattern the CAN side is sending, and count a completed burst
 * every 1024 of them. */
static void bridge_byte(frame_rx_t *fr, uint8_t b)
{
    if (b != bridge_byte_at(fr->bridge_bytes)) {
        fr->isotp_errors++;
        /* Resynchronise on the byte we actually got, so one bad byte is one
         * error rather than a thousand. */
        fr->bridge_bytes = (fr->bridge_bytes & ~0x7Fu) | ((b + 1u) & 0x7Fu);
        return;
    }
    if (++fr->bridge_bytes >= CAN_BURST_BYTES) {
        fr->bridge_bytes = 0;
        fr->bridge_bursts++;
    }
}

static void check_frame(frame_rx_t *fr)
{
    uint16_t want = frame_crc16(fr->buf, FRAME_LEN - FRAME_CRC_BYTES);
    uint16_t got  = (uint16_t)fr->buf[FRAME_LEN - 2u] |
                    (uint16_t)((uint16_t)fr->buf[FRAME_LEN - 1u] << 8);

    if (want != got) {
        fr->crc_errors++;
        fr->resyncs++;
        return;
    }

    uint8_t counter = fr->buf[2];

    /* Read the 64 bytes, do not merely accept them. The checksum would have
     * caught corruption; this catches a frame that is structurally perfect
     * and semantically wrong, and it is the check that says the receiver
     * understood the payload rather than just stored it. */
    const uint8_t *payload = frame_payload(fr->buf);
    for (uint32_t i = 0; i < FRAME_PAYLOAD_BYTES; i++) {
        if (payload[i] != frame_payload_byte(counter, i)) {
            fr->payload_errors++;
            break;
        }
    }

    if (fr->have_last) {
        uint8_t expected = (uint8_t)(fr->last_counter + 1u);
        if (counter != expected) {
            /* Unsigned wrap gives the right gap even across 0xFF. */
            fr->counter_gaps += (uint8_t)(counter - expected);
        }
    }
    fr->have_last    = true;
    fr->last_counter = counter;
    fr->frames_ok++;

    memcpy(fr->snapshot, fr->buf, FRAME_LEN);
    fr->snapshot_seq++;
}

bool frame_rx_byte(frame_rx_t *fr, uint8_t b)
{
    switch (fr->state) {
    case ST_SYNC0:
        if (b == FRAME_SYNC0) {
            fr->buf[0] = b;
            fr->state  = ST_SYNC1;
        } else {
            bridge_byte(fr, b);
        }
        return false;

    case ST_SYNC1:
        if (b == FRAME_SYNC1) {
            fr->buf[1] = b;
            fr->fill   = 2;
            fr->state  = ST_COLLECT;
        } else if (b == FRAME_SYNC0) {
            /* stay here: A5 A5 5A is still a valid opening */
        } else {
            /* The 0xA5 was not a header after all. It cannot have been a
             * bridge byte either - those are all <= 0x7F - so something is
             * wrong on the line. */
            fr->resyncs++;
            fr->state = ST_SYNC0;
            bridge_byte(fr, b);
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

    /* A frame arriving part way through a burst means the transmit arbiter
     * cut a bridge run in half, which the far end could not reassemble. */
    if (fr->bridge_bytes != 0u) {
        fr->isotp_errors++;
        fr->bridge_bytes = 0;
    }

    uint32_t before = fr->frames_ok;
    check_frame(fr);
    return fr->frames_ok != before;
}
