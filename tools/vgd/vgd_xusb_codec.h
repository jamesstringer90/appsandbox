/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-002 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-013 */
#ifndef APPSANDBOX_VGD_XUSB_CODEC_H
#define APPSANDBOX_VGD_XUSB_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "../../src/core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard XUSB IOCTL codes used by xinput1_4.dll and Windows.Gaming.Input */
#define IOCTL_XUSB_GET_INFORMATION    0x80006000u
#define IOCTL_XUSB_GET_CAPABILITIES   0x8000E004u
#define IOCTL_XUSB_GET_LED_STATE      0x8000E008u
#define IOCTL_XUSB_GET_STATE          0x8000E00Cu
#define IOCTL_XUSB_SET_STATE          0x8000A010u
#define IOCTL_XUSB_WAIT_GUIDE         0x8000E014u
#define IOCTL_XUSB_GET_BATTERY_INFO   0x8000E018u
#define IOCTL_XUSB_GET_INFORMATION_EX 0x8000E3FCu
#define IOCTL_XUSB_WAIT_FOR_INPUT     0x8000E3ACu
#define IOCTL_XUSB_POWER_INFO         0x80006380u

#define VGD_XUSB_INFO_SIZE            12u
#define VGD_XUSB_CAPS_0101_SIZE       24u
#define VGD_XUSB_CAPS_0102_SIZE       36u
#define VGD_XUSB_LED_STATE_SIZE       3u
#define VGD_XUSB_STATE_SIZE           29u
#define VGD_XUSB_BATTERY_SIZE         4u
#define VGD_XUSB_INFO_EX_MIN_SIZE     12u
#define VGD_XUSB_INFO_EX_MAX_SIZE     64u

/* Return codes for codec helpers */
#define VGD_CODEC_OK                  0
#define VGD_CODEC_ERR_INVALID        -1
#define VGD_CODEC_ERR_SMALL_BUF      -2

int vgd_xusb_build_get_information(uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_get_capabilities(uint8_t slot, uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_get_led_state(uint8_t slot, uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_get_state(const GamepadState *state, uint32_t packet_no,
                             uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_get_battery_info(uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_get_information_ex(uint8_t *out, size_t out_len, size_t *written);
int vgd_xusb_build_wait_resumed(uint8_t *out, size_t out_len, size_t *written);

/* Parse IOCTL_XUSB_SET_STATE input buffer.
 * Returns 1 if rumble motor bytes were decoded, 0 if valid non-rumble (e.g. LED-only),
 * or VGD_CODEC_ERR_SMALL_BUF / VGD_CODEC_ERR_INVALID on error. */
int vgd_xusb_parse_set_state(const uint8_t *in, size_t in_len,
                             uint8_t *out_large, uint8_t *out_small);

#ifdef __cplusplus
}
#endif

#endif /* APPSANDBOX_VGD_XUSB_CODEC_H */
