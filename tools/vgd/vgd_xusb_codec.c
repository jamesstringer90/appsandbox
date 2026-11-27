/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-002 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-013 */
#include "vgd_xusb_codec.h"
#include "vgd_shm.h"
#include <string.h>

static void wr_u16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_i16_le(uint8_t *p, int16_t v)
{
    wr_u16_le(p, (uint16_t)v);
}

int vgd_xusb_build_get_information(uint8_t *out, size_t out_len, size_t *written)
{
    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_INFO_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    memset(out, 0, VGD_XUSB_INFO_SIZE);
    wr_u16_le(&out[0], 0x0103u); /* XUSB version 0103 */
    out[2] = 0x01u;              /* 1 controller attached */
    wr_u16_le(&out[8], VGD_XBOX360_VID);
    wr_u16_le(&out[10], VGD_XBOX360_PID);

    if (written) *written = VGD_XUSB_INFO_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_get_capabilities(uint8_t slot, uint8_t *out, size_t out_len, size_t *written)
{
    static const uint8_t k_caps_tail[20] = {
        0xFF, 0xF7, 0xFF, 0xFF, 0xC0, 0xFF, 0xC0, 0xFF,
        0xC0, 0xFF, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0xFF, 0xFF
    };

    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_CAPS_0101_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    if (out_len >= VGD_XUSB_CAPS_0102_SIZE) {
        /* 36-byte GamepadCapabilities0102 */
        memset(out, 0, VGD_XUSB_CAPS_0102_SIZE);
        wr_u16_le(&out[0], 0x0103u);
        out[2] = 0x01u; /* XINPUT_DEVTYPE_GAMEPAD */
        out[3] = 0x01u; /* XINPUT_DEVSUBTYPE_GAMEPAD */
        wr_u16_le(&out[4], 0x0000u); /* wired standard gamepad flags */
        wr_u16_le(&out[6], VGD_XBOX360_VID);
        wr_u16_le(&out[8], VGD_XBOX360_PID);
        out[10] = 0x10u;
        out[11] = 0x01u;
        out[12] = (uint8_t)(slot & 0x03u);
        out[13] = 0xFAu;
        out[14] = 0x34u;
        out[15] = 0x22u;
        memcpy(&out[16], k_caps_tail, sizeof(k_caps_tail));
        if (written) *written = VGD_XUSB_CAPS_0102_SIZE;
        return VGD_CODEC_OK;
    }

    /* 24-byte GamepadCapabilities0101 */
    memset(out, 0, VGD_XUSB_CAPS_0101_SIZE);
    wr_u16_le(&out[0], 0x0103u);
    out[2] = 0x00u;
    out[3] = 0x01u; /* XINPUT_DEVSUBTYPE_GAMEPAD */
    memcpy(&out[4], k_caps_tail, sizeof(k_caps_tail));
    if (written) *written = VGD_XUSB_CAPS_0101_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_get_led_state(uint8_t slot, uint8_t *out, size_t out_len, size_t *written)
{
    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_LED_STATE_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    out[0] = 0x00u;
    out[1] = 0x00u;
    out[2] = (uint8_t)(0x06u + (slot & 0x03u)); /* Quadrant 1..4 LED */
    if (written) *written = VGD_XUSB_LED_STATE_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_get_state(const GamepadState *state, uint32_t packet_no,
                             uint8_t *out, size_t out_len, size_t *written)
{
    if (!state || !out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_STATE_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    memset(out, 0, VGD_XUSB_STATE_SIZE);
    wr_u16_le(&out[0], 0x0103u);
    out[2] = 0x01u; /* Connected */
    out[4] = 0xFFu;
    wr_u32_le(&out[5], packet_no);
    out[10] = 0x14u; /* 20-byte report body marker */
    wr_u16_le(&out[11], state->buttons);
    out[13] = state->left_trigger;
    out[14] = state->right_trigger;
    wr_i16_le(&out[15], state->thumb_lx);
    wr_i16_le(&out[17], state->thumb_ly);
    wr_i16_le(&out[19], state->thumb_rx);
    wr_i16_le(&out[21], state->thumb_ry);

    if (written) *written = VGD_XUSB_STATE_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_get_battery_info(uint8_t *out, size_t out_len, size_t *written)
{
    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_BATTERY_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    out[0] = 0x00u;
    out[1] = 0x00u;
    out[2] = 0x01u; /* BATTERY_TYPE_WIRED */
    out[3] = 0x03u; /* BATTERY_LEVEL_FULL */
    if (written) *written = VGD_XUSB_BATTERY_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_get_information_ex(uint8_t *out, size_t out_len, size_t *written)
{
    size_t n;
    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_INFO_EX_MIN_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    n = (out_len < VGD_XUSB_INFO_EX_MAX_SIZE) ? out_len : VGD_XUSB_INFO_EX_MAX_SIZE;
    memset(out, 0, n);
    out[0] = 0x03u;
    out[1] = 0x01u;
    out[2] = 0x01u;
    out[3] = 0x01u;
    wr_u16_le(&out[8], VGD_XBOX360_VID);
    wr_u16_le(&out[10], VGD_XBOX360_PID);

    if (written) *written = n;
    return VGD_CODEC_OK;
}

int vgd_xusb_build_wait_resumed(uint8_t *out, size_t out_len, size_t *written)
{
    if (!out) return VGD_CODEC_ERR_INVALID;
    if (out_len < VGD_XUSB_STATE_SIZE) return VGD_CODEC_ERR_SMALL_BUF;

    memset(out, 0, VGD_XUSB_STATE_SIZE);
    wr_u16_le(&out[0], 0x0103u);
    out[2] = 0x03u; /* Focus resumed */
    if (written) *written = VGD_XUSB_STATE_SIZE;
    return VGD_CODEC_OK;
}

int vgd_xusb_parse_set_state(const uint8_t *in, size_t in_len,
                             uint8_t *out_large, uint8_t *out_small)
{
    if (!in || !out_large || !out_small) return VGD_CODEC_ERR_INVALID;
    if (in_len < 4u) return VGD_CODEC_ERR_SMALL_BUF;

    if (in_len >= 5u) {
        /* Standard 5-byte XUSB SET_STATE: [port, led, large, small, flags] */
        if ((in[4] & 0x02u) == 0u) {
            return 0; /* Valid non-rumble request (e.g. LED update only) */
        }
        *out_large = in[2];
        *out_small = in[3];
        return 1;
    }

    /* 4-byte raw XINPUT_VIBRATION fallback: [left_lo, left_hi, right_lo, right_hi] */
    *out_large = in[1];
    *out_small = in[3];
    return 1;
}
