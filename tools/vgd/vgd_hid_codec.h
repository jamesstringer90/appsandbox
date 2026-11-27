/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-003 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-013 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-003 */
#ifndef APPSANDBOX_VGD_HID_CODEC_H
#define APPSANDBOX_VGD_HID_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include "../../src/core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VGD_HID_INPUT_REPORT_SIZE  14u

/* Return pointer to static retail xusb22 Xbox 360 HID report descriptor and set *out_len. */
const uint8_t *vgd_hid_get_report_descriptor(size_t *out_len);

/* Translate GamepadState into the 14-byte retail xusb22 Xbox 360 HID input report.
 * Returns 0 on success, -2 (VGD_CODEC_ERR_SMALL_BUF) if out_len < 14, -1 on invalid args. */
int vgd_hid_build_input_report(const GamepadState *state,
                               uint8_t *out, size_t out_len, size_t *written);

/* Map GamepadState D-pad bits to 8-way HID hat value (0 = center, 1=N .. 8=NW). */
uint8_t vgd_hid_encode_hat(uint16_t buttons);

#ifdef __cplusplus
}
#endif

#endif /* APPSANDBOX_VGD_HID_CODEC_H */
