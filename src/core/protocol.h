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

#endif
