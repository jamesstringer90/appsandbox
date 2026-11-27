#ifndef ASB_GAMEPAD_PROTOCOL_H
#define ASB_GAMEPAD_PROTOCOL_H

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

int  asb_gamepad_packet_validate(const GamepadPacket *pkt);
void asb_gamepad_make_plug(uint8_t slot, uint32_t seq, GamepadPacket *out);
void asb_gamepad_make_unplug(uint8_t slot, uint32_t seq, GamepadPacket *out);
void asb_gamepad_make_state(uint8_t slot, const GamepadState *state, uint32_t seq, GamepadPacket *out);
void asb_gamepad_make_neutral(uint8_t slot, GamepadPacket *out);
void asb_gamepad_make_rumble(uint8_t slot, uint8_t large_motor, uint8_t small_motor, uint32_t seq, GamepadPacket *out);
int  asb_gamepad_make_name_chunk(uint8_t slot, uint8_t chunk_idx, uint8_t total_chunks,
                                 const char *utf8_12, uint32_t seq, GamepadPacket *out);
int  asb_gamepad_state_equal(const GamepadState *a, const GamepadState *b);

#ifdef __cplusplus
}
#endif

#endif /* ASB_GAMEPAD_PROTOCOL_H */
