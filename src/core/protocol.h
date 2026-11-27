#ifndef ASB_PROTOCOL_H
#define ASB_PROTOCOL_H

#include <stdint.h>

#define INPUT_MAGIC           0x4E495341u /* ASIN, little-endian */
#define INPUT_READY_MAGIC     0x59445249u /* IRDY */
#define INPUT_MOUSE_MOVE      0
#define INPUT_MOUSE_BUTTON    1
#define INPUT_MOUSE_WHEEL     2
#define INPUT_KEY             3
#define INPUT_KEYBOARD_QUERY  4
#define INPUT_KEYBOARD_REPLY  5
#define INPUT_KEY_PHYSICAL    6
#define INPUT_KEYBOARD_VERSION 2
#define INPUT_MOUSE_QUERY     7
#define INPUT_MOUSE_REPLY     8
#define INPUT_MOUSE_RELATIVE  9
#define INPUT_MOUSE_POSITION_QUERY 10
#define INPUT_MOUSE_POSITION_REPLY 11
#define INPUT_UNICODE          12
#define INPUT_MOUSE_VERSION   1
#define INPUT_MOUSE_NORMALIZED_VERSION 2
#define INPUT_MOUSE_NORMALIZED 1u
#define INPUT_MOUSE_ABSOLUTE_MAX 65535u

#define INPUT_BTN_LEFT        0
#define INPUT_BTN_RIGHT       1
#define INPUT_BTN_MIDDLE      2
#define INPUT_KEY_EXTENDED    1
#define INPUT_KEY_UP          2

/* All packets are 20 little-endian bytes, following the IRDY greeting.
 * QUERY/REPLY: param1 = keyboard version, param2 = echoed request ID, param3 = 0.
 * Without a matching reply, senders retain INPUT_KEY's legacy VK semantics.
 * KEY_PHYSICAL: param2 = set-1 make byte, EXTENDED = E0, UP = release.
 * With scan zero only, param1 identifies a Windows VK non-text function key
 * (Pause/Break, Print Screen, Sleep, or browser/media/launch keys).
 * MOUSE_QUERY/REPLY: param1 = mouse version, param2 = echoed request ID, param3 = 0.
 * Without a matching mouse reply, senders retain absolute mouse movement.
 * MOUSE_RELATIVE: param1/param2 = signed int32 X/Y deltas, param3 = 0.
 * POSITION_QUERY: param1 = request ID, param2/param3 = 0.
 * POSITION_REPLY: param1/param2 = physical guest-screen X/Y, param3 = request ID.
 * Both coordinates are INT32_MIN if the cursor position cannot be read.
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
} InputPacket;
#pragma pack(pop)

#define DISPLAY_GPU_MAGIC     0x50475341u
#define DISPLAY_GPU_VERSION   3
#define DISPLAY_GPU_QUERY     1
#define DISPLAY_GPU_CAPS      2
#define DISPLAY_GPU_OFFER     3
#define DISPLAY_GPU_READY     4
#define DISPLAY_GPU_RESIZE    5
#define DISPLAY_GPU_RESIZED   6
#define DISPLAY_GPU_SNAPSHOT  7
#define DISPLAY_GPU_SLOTS     3
#define DISPLAY_GPU_CAP_RESIZE 1u
#define DISPLAY_GPU_CAP_IVSHMEM 2u
#define DISPLAY_GPU_CAP_FENCE 4u
#define DISPLAY_GPU_CAP_REFRESH 8u
#define DISPLAY_GPU_CAP_SNAPSHOT 16u
#define DISPLAY_GPU_FENCE_SLOTS (DISPLAY_GPU_SLOTS * 2)
#define DISPLAY_SIZE_GUEST    0u
#define DISPLAY_SIZE_WINDOW   1u
#define DISPLAY_MIN_WIDTH     1024u
#define DISPLAY_MIN_HEIGHT    768u
#define DISPLAY_MAX_WIDTH     7680u
#define DISPLAY_MAX_HEIGHT    4320u
#define DISPLAY_MIN_REFRESH   24u
#define DISPLAY_MAX_REFRESH   120u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t type;
    uint64_t session;
    uint64_t handle;
    uint64_t reserved;
    uint32_t process_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t status;
    uint32_t version;
    uint32_t slot;
} DisplayGpuPacket;
#pragma pack(pop)

/* ---- Gamepad wire protocol (ASB_CH_GAMEPAD = 8) ----
 * Bidirectional fixed 24-byte little-endian frames, following the GRDY greeting.
 * OS-agnostic Xbox 360 / XInput layout (usable by Windows UMDF2 VGD, Linux uinput, macOS).
 */
#define GAMEPAD_MAGIC            0x44475341u /* ASGD, little-endian */
#define GAMEPAD_READY_MAGIC      0x59445247u /* GRDY, little-endian */
#define GAMEPAD_PROTOCOL_VERSION 1u
#define GAMEPAD_MAX_SLOTS        4u

#define GAMEPAD_PKT_HELLO        1u
#define GAMEPAD_PKT_PLUG         2u
#define GAMEPAD_PKT_UNPLUG       3u
#define GAMEPAD_PKT_STATE        4u
#define GAMEPAD_PKT_RUMBLE       5u
#define GAMEPAD_PKT_NAME         6u

#define GAMEPAD_NAME_CHUNK_BYTES 12u
#define GAMEPAD_NAME_MAX_CHUNKS  5u
#define GAMEPAD_NAME_MAX_LEN     64u

#define GAMEPAD_BTN_DPAD_UP        0x0001u
#define GAMEPAD_BTN_DPAD_DOWN      0x0002u
#define GAMEPAD_BTN_DPAD_LEFT      0x0004u
#define GAMEPAD_BTN_DPAD_RIGHT     0x0008u
#define GAMEPAD_BTN_START          0x0010u
#define GAMEPAD_BTN_BACK           0x0020u
#define GAMEPAD_BTN_LEFT_THUMB     0x0040u
#define GAMEPAD_BTN_RIGHT_THUMB    0x0080u
#define GAMEPAD_BTN_LEFT_SHOULDER  0x0100u
#define GAMEPAD_BTN_RIGHT_SHOULDER 0x0200u
#define GAMEPAD_BTN_GUIDE          0x0400u
#define GAMEPAD_BTN_A              0x1000u
#define GAMEPAD_BTN_B              0x2000u
#define GAMEPAD_BTN_X              0x4000u
#define GAMEPAD_BTN_Y              0x8000u

#pragma pack(push, 1)
typedef struct {
    uint16_t buttons;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    int16_t  thumb_lx;
    int16_t  thumb_ly;
    int16_t  thumb_rx;
    int16_t  thumb_ry;
} GamepadState;

typedef struct {
    uint32_t     magic;
    uint8_t      type;
    uint8_t      slot;
    uint8_t      large_motor;
    uint8_t      small_motor;
    GamepadState state;
    uint32_t     seq;
} GamepadPacket;
#pragma pack(pop)

#endif
