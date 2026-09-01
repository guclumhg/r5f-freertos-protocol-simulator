#include "engine/isotp.h"

#include <string.h>

void isotp_init(isotp_t *tp)
{
    memset(tp, 0, sizeof(*tp));
    /* Start the first burst almost immediately rather than after a full
     * second of silence, so the dashboard has something to draw at boot. */
    tp->since_burst = CAN_BURST_PERIOD_SLOTS - ISOTP_FRAMES_TOTAL;
}

isotp_slot_kind_t isotp_slot(isotp_t *tp, uint8_t *out, bool *started)
{
    *started = false;

    if (!tp->active) {
        if (++tp->since_burst < CAN_BURST_PERIOD_SLOTS) {
            return ISOTP_SLOT_IDLE;
        }
        tp->active      = true;
        tp->slot        = 0;
        tp->since_burst = 0;
        tp->burst_pos   = 0;
        tp->burst_seq++;
        tp->bursts++;
        *started = true;
    }

    const uint32_t slot = tp->slot++;
    tp->frames++;

    if (tp->slot >= ISOTP_FRAMES_TOTAL) {
        tp->active = false;          /* this is the last slot of the burst */
    }

    if (slot == 0) {
        return ISOTP_SLOT_FIRST_FRAME;
    }
    if (slot == ISOTP_FRAMES_TOTAL - 2u) {
        return ISOTP_SLOT_ACK;
    }
    if (slot == ISOTP_FRAMES_TOTAL - 1u) {
        return ISOTP_SLOT_END;
    }

    const uint32_t idx = slot - 1u;                     /* 0..143 */
    const uint32_t block = idx / ISOTP_SLOTS_PER_BLOCK; /* 0..15 */
    const uint32_t pos   = idx % ISOTP_SLOTS_PER_BLOCK; /* 0..8  */

    if (pos == 0) {
        (void)block;                 /* the receiver says go ahead */
        return ISOTP_SLOT_FLOW_CONTROL;
    }

    /* A consecutive frame carries eight bytes of the burst's payload. */
    for (uint32_t i = 0; i < CAN_FRAME_PAYLOAD; i++) {
        out[i] = bridge_byte_at(tp->burst_pos + i);
    }
    tp->burst_pos  += CAN_FRAME_PAYLOAD;
    tp->data_bytes += CAN_FRAME_PAYLOAD;
    return ISOTP_SLOT_CONSECUTIVE;
}
