/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-003 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-013 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-003 */
#include "vgd_hid_codec.h"
#include "vgd_xusb_codec.h"
#include <string.h>

/* Retail Microsoft xusb22 Xbox 360 Controller for Windows HID Report Descriptor
 * (114 bytes, 14-byte input report, matching inbox xusb22.sys xusb_gamepad2).
 * Layout:
 *   [0..1]   X  (Left Stick X, 0..65535)
 *   [2..3]   Y  (Left Stick Y, 0..65535, up = 0)
 *   [4..5]   Rx (Right Stick X, 0..65535)
 *   [6..7]   Ry (Right Stick Y, 0..65535, up = 0)
 *   [8..9]   Z  (Combined triggers, center = 32768; LT > 32768, RT < 32768)
 *   [10..11] Bits 0..9 = Buttons 1..10 (A,B,X,Y,LB,RB,Back,Start,LThumb,RThumb)
 *            Bits 10..13 = 8-way Hat Switch (0=null, 1=N..8=NW)
 *            Bits 14..15 = 2-bit padding
 *   [12..13] 2-byte constant padding
 */
static const uint8_t k_xbox360_hid_report_desc[] = {
    0x05, 0x01,                   /* Usage Page (Generic Desktop) */
    0x09, 0x05,                   /* Usage (Game Pad) */
    0xA1, 0x01,                   /* Collection (Application) */

    0xA1, 0x00,                   /*   Collection (Physical) - Left Stick */
    0x09, 0x30,                   /*     Usage (X) */
    0x09, 0x31,                   /*     Usage (Y) */
    0x15, 0x00,                   /*     Logical Minimum (0) */
    0x26, 0xFF, 0xFF,             /*     Logical Maximum (65535) */
    0x35, 0x00,                   /*     Physical Minimum (0) */
    0x46, 0xFF, 0xFF,             /*     Physical Maximum (65535) */
    0x95, 0x02,                   /*     Report Count (2) */
    0x75, 0x10,                   /*     Report Size (16) */
    0x81, 0x02,                   /*     Input (Data, Var, Abs) */
    0xC0,                         /*   End Collection */

    0xA1, 0x00,                   /*   Collection (Physical) - Right Stick */
    0x09, 0x33,                   /*     Usage (Rx) */
    0x09, 0x34,                   /*     Usage (Ry) */
    0x15, 0x00,                   /*     Logical Minimum (0) */
    0x26, 0xFF, 0xFF,             /*     Logical Maximum (65535) */
    0x35, 0x00,                   /*     Physical Minimum (0) */
    0x46, 0xFF, 0xFF,             /*     Physical Maximum (65535) */
    0x95, 0x02,                   /*     Report Count (2) */
    0x75, 0x10,                   /*     Report Size (16) */
    0x81, 0x02,                   /*     Input (Data, Var, Abs) */
    0xC0,                         /*   End Collection */

    0xA1, 0x00,                   /*   Collection (Physical) - Combined Z Trigger */
    0x09, 0x32,                   /*     Usage (Z) */
    0x15, 0x00,                   /*     Logical Minimum (0) */
    0x26, 0xFF, 0xFF,             /*     Logical Maximum (65535) */
    0x35, 0x00,                   /*     Physical Minimum (0) */
    0x46, 0xFF, 0xFF,             /*     Physical Maximum (65535) */
    0x95, 0x01,                   /*     Report Count (1) */
    0x75, 0x10,                   /*     Report Size (16) */
    0x81, 0x02,                   /*     Input (Data, Var, Abs) */
    0xC0,                         /*   End Collection */

    0x05, 0x09,                   /*   Usage Page (Button) */
    0x19, 0x01,                   /*   Usage Minimum (Button 1) */
    0x29, 0x0A,                   /*   Usage Maximum (Button 10) */
    0x95, 0x0A,                   /*   Report Count (10) */
    0x75, 0x01,                   /*   Report Size (1) */
    0x81, 0x02,                   /*   Input (Data, Var, Abs) */

    0x05, 0x01,                   /*   Usage Page (Generic Desktop) */
    0x09, 0x39,                   /*   Usage (Hat switch) */
    0x15, 0x01,                   /*   Logical Minimum (1) */
    0x25, 0x08,                   /*   Logical Maximum (8) */
    0x35, 0x00,                   /*   Physical Minimum (0) */
    0x46, 0x3B, 0x10,             /*   Physical Maximum (xusb22 0x103B) */
    0x66, 0x0E, 0x00,             /*   Unit */
    0x75, 0x04,                   /*   Report Size (4) */
    0x95, 0x01,                   /*   Report Count (1) */
    0x81, 0x42,                   /*   Input (Data, Var, Abs, Null State) */

    0x75, 0x02,                   /*   Report Size (2) */
    0x95, 0x01,                   /*   Report Count (1) */
    0x81, 0x03,                   /*   Input (Const, Var, Abs) */

    0x75, 0x08,                   /*   Report Size (8) */
    0x95, 0x02,                   /*   Report Count (2) */
    0x81, 0x03,                   /*   Input (Const, Var, Abs) */

    0xC0                          /* End Collection */
};

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

const uint8_t *vgd_hid_get_report_descriptor(size_t *out_len)
{
    if (out_len) *out_len = sizeof(k_xbox360_hid_report_desc);
    return k_xbox360_hid_report_desc;
}

uint8_t vgd_hid_encode_hat(uint16_t buttons)
{
    int up    = (buttons & GAMEPAD_BTN_DPAD_UP)    != 0;
    int down  = (buttons & GAMEPAD_BTN_DPAD_DOWN)  != 0;
    int left  = (buttons & GAMEPAD_BTN_DPAD_LEFT)  != 0;
    int right = (buttons & GAMEPAD_BTN_DPAD_RIGHT) != 0;

    if (up && down) { up = 0; down = 0; }
    if (left && right) { left = 0; right = 0; }

    if (up) {
        if (right) return 2u;
        if (left)  return 8u;
        return 1u;
    }
    if (down) {
        if (right) return 4u;
        if (left)  return 6u;
        return 5u;
    }
    if (right) return 3u;
    if (left)  return 7u;
    return 0u;
}

int vgd_hid_build_input_report(const GamepadState *state,
                               uint8_t *out, size_t out_len, size_t *written)
{
    uint16_t lx, ly, rx, ry, z;
    int32_t z_calc;
    uint16_t btn_bits = 0u;
    uint8_t hat;
    uint16_t btn_hat;

    if (!state || !out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_HID_INPUT_REPORT_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    memset(out, 0, VGD_HID_INPUT_REPORT_SIZE);

    /* XInput signed -32768..32767 (+Y up) -> HID unsigned 0..65535 (+Y down) */
    lx = (uint16_t)((int32_t)state->thumb_lx + 32768);
    ly = (uint16_t)(65535 - ((int32_t)state->thumb_ly + 32768));
    rx = (uint16_t)((int32_t)state->thumb_rx + 32768);
    ry = (uint16_t)(65535 - ((int32_t)state->thumb_ry + 32768));

    z_calc = 32768 + ((int32_t)state->left_trigger - (int32_t)state->right_trigger) * 128;
    if (z_calc < 0) z_calc = 0;
    if (z_calc > 65535) z_calc = 65535;
    z = (uint16_t)z_calc;

    if (state->buttons & GAMEPAD_BTN_A)              btn_bits |= (1u << 0);
    if (state->buttons & GAMEPAD_BTN_B)              btn_bits |= (1u << 1);
    if (state->buttons & GAMEPAD_BTN_X)              btn_bits |= (1u << 2);
    if (state->buttons & GAMEPAD_BTN_Y)              btn_bits |= (1u << 3);
    if (state->buttons & GAMEPAD_BTN_LEFT_SHOULDER)  btn_bits |= (1u << 4);
    if (state->buttons & GAMEPAD_BTN_RIGHT_SHOULDER) btn_bits |= (1u << 5);
    if (state->buttons & GAMEPAD_BTN_BACK)           btn_bits |= (1u << 6);
    if (state->buttons & GAMEPAD_BTN_START)          btn_bits |= (1u << 7);
    if (state->buttons & GAMEPAD_BTN_LEFT_THUMB)     btn_bits |= (1u << 8);
    if (state->buttons & GAMEPAD_BTN_RIGHT_THUMB)    btn_bits |= (1u << 9);

    hat = vgd_hid_encode_hat(state->buttons);
    btn_hat = (uint16_t)(btn_bits | ((uint16_t)(hat & 0x0Fu) << 10));

    wr_u16_le(&out[0], lx);
    wr_u16_le(&out[2], ly);
    wr_u16_le(&out[4], rx);
    wr_u16_le(&out[6], ry);
    wr_u16_le(&out[8], z);
    wr_u16_le(&out[10], btn_hat);
    out[12] = 0u;
    out[13] = 0u;

    if (written) *written = VGD_HID_INPUT_REPORT_SIZE;
    return VGD_CODEC_OK;
}
