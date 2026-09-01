/*
 * isotp.h - The CAN side: one ISO-TP transfer per second, delivered as a
 * burst of 147 frames in 135 us slots.
 *
 * Portable C, no hardware. This is stimulus, not part of the system under
 * test - it stands in for whatever is really pushing traffic at the bridge.
 *
 * The frame accounting is fixed by the specification and checked in config.h:
 *
 *   slot 0          First Frame
 *   slots 1..144    16 blocks of (1 Flow Control + 8 Consecutive Frames)
 *   slot 145        closing Flow Control
 *   slot 146        end of block
 *   ------------------------------------------------------------------
 *   128 Consecutive Frames x 8 data bytes            = 1024 bytes
 *   19 protocol frames (1 FF + 16 FC + 1 FC + 1 end)
 *   147 frames x 135 us                              = 19.845 ms
 *
 * Those 1024 bytes are the bridge's payload for the second. They happen to be
 * laid out as 16 units of 64 bytes, which is what lets the far end of the
 * loopback tell them apart from sensor frames, and which is also why the
 * sensor stream gives up exactly 1024/64 = 16 of its 180 frames per second.
 */
#ifndef R5F_ISOTP_H
#define R5F_ISOTP_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "engine/frame.h"

#define ISOTP_UNITS_PER_BURST (CAN_BURST_BYTES / FRAME_LEN)   /* 16 */
#define ISOTP_SLOTS_PER_BLOCK (1u + ISOTP_BLOCK_SIZE)         /* FC + 8 CF */

typedef enum {
    ISOTP_SLOT_IDLE = 0,
    ISOTP_SLOT_FIRST_FRAME,
    ISOTP_SLOT_FLOW_CONTROL,
    ISOTP_SLOT_CONSECUTIVE,      /* carries CAN_FRAME_PAYLOAD data bytes */
    ISOTP_SLOT_ACK,
    ISOTP_SLOT_END,
} isotp_slot_kind_t;

typedef struct {
    bool     active;
    uint32_t slot;            /* 0..146 inside a burst */
    uint32_t since_burst;     /* slots since the last burst started */
    uint8_t  burst_seq;

    uint8_t  unit[FRAME_LEN]; /* the 64 byte unit currently being handed over */
    uint8_t  unit_index;

    uint32_t bursts;
    uint32_t frames;          /* CAN frames generated, all kinds */
    uint32_t data_bytes;      /* bytes handed to the bridge */
} isotp_t;

void isotp_init(isotp_t *tp);

/* Advances one 135 us slot.
 *
 * Returns what this slot carried. For ISOTP_SLOT_CONSECUTIVE it writes
 * CAN_FRAME_PAYLOAD bytes into out; every other kind writes nothing.
 * Sets *started when this slot is the first of a new burst. */
isotp_slot_kind_t isotp_slot(isotp_t *tp, uint8_t *out, bool *started);

#endif /* R5F_ISOTP_H */
