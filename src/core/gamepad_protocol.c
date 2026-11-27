#include "gamepad_protocol.h"
#include <string.h>

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-001
// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-012
// @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023
int asb_gamepad_packet_validate(const GamepadPacket *pkt)
{
    if (!pkt)
        return 0;
    if (pkt->magic != GAMEPAD_MAGIC)
        return 0;
    switch (pkt->type) {
    case GAMEPAD_PKT_HELLO:
        return pkt->slot < GAMEPAD_MAX_SLOTS;
    case GAMEPAD_PKT_PLUG:
    case GAMEPAD_PKT_UNPLUG:
    case GAMEPAD_PKT_STATE:
    case GAMEPAD_PKT_RUMBLE:
        return pkt->slot < GAMEPAD_MAX_SLOTS;
    case GAMEPAD_PKT_NAME:
        return pkt->slot < GAMEPAD_MAX_SLOTS &&
               pkt->small_motor >= 1u &&
               pkt->small_motor <= GAMEPAD_NAME_MAX_CHUNKS &&
               pkt->large_motor < pkt->small_motor;
    default:
        return 0;
    }
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-001
// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
void asb_gamepad_make_plug(uint8_t slot, uint32_t seq, GamepadPacket *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_PLUG;
    out->slot = slot;
    out->seq = seq;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-001
// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
void asb_gamepad_make_unplug(uint8_t slot, uint32_t seq, GamepadPacket *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_UNPLUG;
    out->slot = slot;
    out->seq = seq;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-001
// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
void asb_gamepad_make_state(uint8_t slot, const GamepadState *state, uint32_t seq, GamepadPacket *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_STATE;
    out->slot = slot;
    if (state)
        out->state = *state;
    out->seq = seq;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
void asb_gamepad_make_neutral(uint8_t slot, GamepadPacket *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_STATE;
    out->slot = slot;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
void asb_gamepad_make_rumble(uint8_t slot, uint8_t large_motor, uint8_t small_motor, uint32_t seq, GamepadPacket *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_RUMBLE;
    out->slot = slot;
    out->large_motor = large_motor;
    out->small_motor = small_motor;
    out->seq = seq;
}

// @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023
int asb_gamepad_make_name_chunk(uint8_t slot, uint8_t chunk_idx, uint8_t total_chunks,
                                const char *utf8_12, uint32_t seq, GamepadPacket *out)
{
    if (!out || slot >= GAMEPAD_MAX_SLOTS ||
        total_chunks < 1u || total_chunks > GAMEPAD_NAME_MAX_CHUNKS ||
        chunk_idx >= total_chunks) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->magic = GAMEPAD_MAGIC;
    out->type = GAMEPAD_PKT_NAME;
    out->slot = slot;
    out->large_motor = chunk_idx;
    out->small_motor = total_chunks;
    out->seq = seq;
    if (utf8_12) {
        size_t len = 0;
        while (len < GAMEPAD_NAME_CHUNK_BYTES && utf8_12[len] != '\0') {
            len++;
        }
        if (len > 0) {
            memcpy(&out->state, utf8_12, len);
        }
    }
    return 1;
}

// @implements spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
int asb_gamepad_state_equal(const GamepadState *a, const GamepadState *b)
{
    if (!a || !b)
        return 0;
    return a->buttons == b->buttons &&
           a->left_trigger == b->left_trigger &&
           a->right_trigger == b->right_trigger &&
           a->thumb_lx == b->thumb_lx &&
           a->thumb_ly == b->thumb_ly &&
           a->thumb_rx == b->thumb_rx &&
           a->thumb_ry == b->thumb_ry;
}
