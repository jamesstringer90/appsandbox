/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-001 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-002 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-003 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-013 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-004 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-005 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-008 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-012 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-002::REQ-013 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-006 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-007 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-008 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-009 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-010 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-014 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-003 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-015 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-016 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-017 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-006::REQ-018 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-019 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-020 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-018 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-019 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-020 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-008::REQ-021 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-020 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-021 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-022 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023 */
/* @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VGD_CTRL_NO_WIN32 1
#include "../tools/vgd/vgd_shm.h"
#include "../tools/vgd/vgd_xusb_codec.h"
#include "../tools/vgd/vgd_hid_codec.h"
#include "../tools/agent/vgd_controller.h"

static int g_failures = 0;

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_EQ_U32(a, b) do { \
    uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d: %s (%u) != %s (%u)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        g_failures++; \
    } \
} while (0)

#define EXPECT_EQ_I32(a, b) do { \
    int32_t _a = (int32_t)(a), _b = (int32_t)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d: %s (%d) != %s (%d)\n", __FILE__, __LINE__, #a, _a, #b, _b); \
        g_failures++; \
    } \
} while (0)

static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t rd_i16_le(const uint8_t *p)
{
    return (int16_t)rd_u16_le(p);
}

static void test_shm_contract(void)
{
    VgdShmBlock blk;
    GamepadState in_state, out_state;
    uint32_t gen = 0, rgen = 0;
    uint8_t lm = 0, sm = 0;
    char shm[64], inevt[64], outevt[64];
    char xhwid[64], hhwid[64];

    EXPECT_EQ_U32((uint32_t)sizeof(VgdShmBlock), VGD_SHM_BLOCK_SIZE);

    EXPECT_EQ_I32(vgd_shm_format_names(0, shm, sizeof(shm), inevt, sizeof(inevt), outevt, sizeof(outevt)), 0);
    EXPECT_TRUE(strcmp(shm, "Global\\AppSandboxGamepad0") == 0);
    EXPECT_TRUE(strcmp(inevt, "Global\\AppSandboxGamepadIn0") == 0);
    EXPECT_TRUE(strcmp(outevt, "Global\\AppSandboxGamepadOut0") == 0);

    EXPECT_EQ_I32(vgd_shm_format_names(3, shm, sizeof(shm), inevt, sizeof(inevt), outevt, sizeof(outevt)), 0);
    EXPECT_TRUE(strcmp(shm, "Global\\AppSandboxGamepad3") == 0);
    EXPECT_EQ_I32(vgd_shm_format_names(4, shm, sizeof(shm), inevt, sizeof(inevt), outevt, sizeof(outevt)), -1);

    EXPECT_EQ_I32(vgd_format_hwids(2, xhwid, sizeof(xhwid), hhwid, sizeof(hhwid)), 0);
    EXPECT_TRUE(strcmp(xhwid, "root\\AppSandboxVGD_XUSB_2") == 0);
    EXPECT_TRUE(strcmp(hhwid, "root\\AppSandboxVGD_HID_2") == 0);
    EXPECT_EQ_I32(vgd_parse_slot_from_hwid("root\\AppSandboxVGD_XUSB_2"), 2);
    EXPECT_EQ_I32(vgd_parse_slot_from_hwid("ROOT\\APPSANDBOXVGD_HID_3\\0000"), 3);
    EXPECT_EQ_I32(vgd_parse_slot_from_hwid("root\\AppSandboxVGD_XUSB_9"), -1);

    EXPECT_EQ_I32(vgd_shm_init(&blk, 1), 0);
    EXPECT_EQ_I32(vgd_shm_is_valid(&blk), 1);
    EXPECT_EQ_U32(blk.magic, VGD_SHM_MAGIC);
    EXPECT_EQ_U32(blk.version, VGD_SHM_VERSION);
    EXPECT_EQ_U32(blk.slot, 1);
    EXPECT_EQ_U32(blk.connected, 1);

    memset(&in_state, 0, sizeof(in_state));
    in_state.buttons = (uint16_t)(GAMEPAD_BTN_A | GAMEPAD_BTN_GUIDE | GAMEPAD_BTN_DPAD_UP);
    in_state.left_trigger = 180;
    in_state.right_trigger = 45;
    in_state.thumb_lx = 12345;
    in_state.thumb_ly = -23456;
    in_state.thumb_rx = -32768;
    in_state.thumb_ry = 32767;

    EXPECT_EQ_I32(vgd_shm_write_state(&blk, &in_state), 0);
    EXPECT_EQ_I32(vgd_shm_read_state(&blk, &out_state, &gen), 0);
    EXPECT_EQ_U32(gen, 1u);
    EXPECT_EQ_U32(out_state.buttons, in_state.buttons);
    EXPECT_EQ_U32(out_state.left_trigger, 180u);
    EXPECT_EQ_U32(out_state.right_trigger, 45u);
    EXPECT_EQ_I32(out_state.thumb_lx, 12345);
    EXPECT_EQ_I32(out_state.thumb_ly, -23456);
    EXPECT_EQ_I32(out_state.thumb_rx, -32768);
    EXPECT_EQ_I32(out_state.thumb_ry, 32767);

    EXPECT_EQ_I32(vgd_shm_write_rumble(&blk, 210, 95), 0);
    EXPECT_EQ_I32(vgd_shm_read_rumble(&blk, &lm, &sm, &rgen), 0);
    EXPECT_EQ_U32(rgen, 1u);
    EXPECT_EQ_U32(lm, 210u);
    EXPECT_EQ_U32(sm, 95u);
}

static void test_xusb_codec(void)
{
    uint8_t buf[64];
    size_t written = 0;
    GamepadState st;
    uint8_t lm = 0, sm = 0;

    /* GET_INFORMATION */
    EXPECT_EQ_I32(vgd_xusb_build_get_information(buf, 11, &written), VGD_CODEC_ERR_SMALL_BUF);
    EXPECT_EQ_I32(vgd_xusb_build_get_information(buf, sizeof(buf), &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_INFO_SIZE);
    EXPECT_EQ_U32(rd_u16_le(&buf[0]), 0x0103u);
    EXPECT_EQ_U32(buf[2], 0x01u);
    EXPECT_EQ_U32(rd_u16_le(&buf[8]), VGD_XBOX360_VID);
    EXPECT_EQ_U32(rd_u16_le(&buf[10]), VGD_XBOX360_PID);

    /* GET_CAPABILITIES (both 24-byte 0101 and 36-byte 0102) */
    EXPECT_EQ_I32(vgd_xusb_build_get_capabilities(0, buf, 23, &written), VGD_CODEC_ERR_SMALL_BUF);
    EXPECT_EQ_I32(vgd_xusb_build_get_capabilities(0, buf, 24, &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_CAPS_0101_SIZE);
    EXPECT_EQ_U32(rd_u16_le(&buf[0]), 0x0103u);
    EXPECT_EQ_U32(buf[3], 0x01u); /* Gamepad subtype */

    EXPECT_EQ_I32(vgd_xusb_build_get_capabilities(2, buf, 36, &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_CAPS_0102_SIZE);
    EXPECT_EQ_U32(buf[3], 0x01u);
    EXPECT_EQ_U32(rd_u16_le(&buf[6]), VGD_XBOX360_VID);
    EXPECT_EQ_U32(rd_u16_le(&buf[8]), VGD_XBOX360_PID);
    EXPECT_EQ_U32(buf[12], 2u); /* slot */

    /* GET_LED_STATE */
    EXPECT_EQ_I32(vgd_xusb_build_get_led_state(1, buf, 2, &written), VGD_CODEC_ERR_SMALL_BUF);
    EXPECT_EQ_I32(vgd_xusb_build_get_led_state(1, buf, sizeof(buf), &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_LED_STATE_SIZE);
    EXPECT_EQ_U32(buf[2], 0x07u); /* Quadrant 2 */

    /* GET_BATTERY_INFO */
    EXPECT_EQ_I32(vgd_xusb_build_get_battery_info(buf, 3, &written), VGD_CODEC_ERR_SMALL_BUF);
    EXPECT_EQ_I32(vgd_xusb_build_get_battery_info(buf, sizeof(buf), &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_BATTERY_SIZE);
    EXPECT_EQ_U32(buf[2], 0x01u); /* BATTERY_TYPE_WIRED */
    EXPECT_EQ_U32(buf[3], 0x03u); /* BATTERY_LEVEL_FULL */

    /* GET_STATE */
    memset(&st, 0, sizeof(st));
    st.buttons = (uint16_t)(GAMEPAD_BTN_B | GAMEPAD_BTN_START | GAMEPAD_BTN_GUIDE);
    st.left_trigger = 200;
    st.right_trigger = 255;
    st.thumb_lx = -16384;
    st.thumb_ly = 16384;
    st.thumb_rx = 32767;
    st.thumb_ry = -32768;

    EXPECT_EQ_I32(vgd_xusb_build_get_state(&st, 42u, buf, 28, &written), VGD_CODEC_ERR_SMALL_BUF);
    EXPECT_EQ_I32(vgd_xusb_build_get_state(&st, 42u, buf, sizeof(buf), &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_XUSB_STATE_SIZE);
    EXPECT_EQ_U32(rd_u16_le(&buf[0]), 0x0103u);
    EXPECT_EQ_U32(buf[2], 0x01u);
    EXPECT_EQ_U32(buf[4], 0xFFu);
    EXPECT_EQ_U32(rd_u32_le(&buf[5]), 42u);
    EXPECT_EQ_U32(buf[10], 0x14u);
    EXPECT_EQ_U32(rd_u16_le(&buf[11]), st.buttons);
    EXPECT_EQ_U32(buf[13], 200u);
    EXPECT_EQ_U32(buf[14], 255u);
    EXPECT_EQ_I32(rd_i16_le(&buf[15]), -16384);
    EXPECT_EQ_I32(rd_i16_le(&buf[17]), 16384);
    EXPECT_EQ_I32(rd_i16_le(&buf[19]), 32767);
    EXPECT_EQ_I32(rd_i16_le(&buf[21]), -32768);

    /* SET_STATE (5-byte XUSB rumble, LED-only, 4-byte vibration, short buffer) */
    {
        uint8_t set_rumble[5] = { 0x00, 0x00, 0xCC, 0x33, 0x02 };
        uint8_t set_led[5]    = { 0x00, 0x06, 0x00, 0x00, 0x01 };
        uint8_t set_raw4[4]   = { 0x00, 0x80, 0x00, 0x40 };
        uint8_t too_short[3]  = { 0x00, 0x00, 0x00 };

        EXPECT_EQ_I32(vgd_xusb_parse_set_state(set_rumble, sizeof(set_rumble), &lm, &sm), 1);
        EXPECT_EQ_U32(lm, 0xCCu);
        EXPECT_EQ_U32(sm, 0x33u);

        EXPECT_EQ_I32(vgd_xusb_parse_set_state(set_led, sizeof(set_led), &lm, &sm), 0);
        EXPECT_EQ_I32(vgd_xusb_parse_set_state(set_raw4, sizeof(set_raw4), &lm, &sm), 1);
        EXPECT_EQ_U32(lm, 0x80u);
        EXPECT_EQ_U32(sm, 0x40u);

        EXPECT_EQ_I32(vgd_xusb_parse_set_state(too_short, sizeof(too_short), &lm, &sm), VGD_CODEC_ERR_SMALL_BUF);
    }
}

static void test_hid_codec(void)
{
    size_t desc_len = 0, written = 0;
    const uint8_t *desc = vgd_hid_get_report_descriptor(&desc_len);
    uint8_t rep[32];
    GamepadState st;

    EXPECT_TRUE(desc != NULL);
    EXPECT_TRUE(desc_len >= 90);
    /* Starts with Usage Page (Generic Desktop) 05 01, Usage (Gamepad) 09 05, Collection (App) A1 01 */
    EXPECT_EQ_U32(desc[0], 0x05u);
    EXPECT_EQ_U32(desc[1], 0x01u);
    EXPECT_EQ_U32(desc[2], 0x09u);
    EXPECT_EQ_U32(desc[3], 0x05u);
    EXPECT_EQ_U32(desc[4], 0xA1u);
    EXPECT_EQ_U32(desc[5], 0x01u);
    /* Ends with End Collection C0 */
    EXPECT_EQ_U32(desc[desc_len - 1], 0xC0u);

    /* Hat encoding */
    EXPECT_EQ_U32(vgd_hid_encode_hat(0), 0u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_UP), 1u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_RIGHT), 2u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_RIGHT), 3u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_RIGHT), 4u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_DOWN), 5u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_LEFT), 6u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_LEFT), 7u);
    EXPECT_EQ_U32(vgd_hid_encode_hat(GAMEPAD_BTN_DPAD_UP | GAMEPAD_BTN_DPAD_LEFT), 8u);

    /* Input report bounds & retail 14-byte xusb22 field layout */
    memset(&st, 0, sizeof(st));
    EXPECT_EQ_I32(vgd_hid_build_input_report(&st, rep, VGD_HID_INPUT_REPORT_SIZE - 1, &written),
                  VGD_CODEC_ERR_SMALL_BUF);

    st.buttons = (uint16_t)(GAMEPAD_BTN_A | GAMEPAD_BTN_Y | GAMEPAD_BTN_RIGHT_THUMB |
                            GAMEPAD_BTN_DPAD_DOWN | GAMEPAD_BTN_DPAD_LEFT);
    st.left_trigger = 255;
    st.right_trigger = 0;
    st.thumb_lx = 32767;
    st.thumb_ly = 32767;  /* XInput up -> HID Y = 0 */
    st.thumb_rx = -32768;
    st.thumb_ry = -32768; /* XInput down -> HID Ry = 65535 */

    EXPECT_EQ_I32(vgd_hid_build_input_report(&st, rep, sizeof(rep), &written), VGD_CODEC_OK);
    EXPECT_EQ_U32((uint32_t)written, VGD_HID_INPUT_REPORT_SIZE);
    EXPECT_EQ_U32(rd_u16_le(&rep[0]), 65535u);  /* LX */
    EXPECT_EQ_U32(rd_u16_le(&rep[2]), 0u);      /* LY inverted */
    EXPECT_EQ_U32(rd_u16_le(&rep[4]), 0u);      /* RX */
    EXPECT_EQ_U32(rd_u16_le(&rep[6]), 65535u);  /* RY inverted */
    EXPECT_TRUE(rd_u16_le(&rep[8]) > 0xF000u);  /* Combined Z leans LT high */
    /* Buttons (bits 0..9) + Hat (bits 10..13) in rep[10..11] */
    {
        uint16_t btn_hat = rd_u16_le(&rep[10]);
        /* A=bit0, Y=bit3, RThumb=bit9 => 0x0209; Hat SW=6 => 6 << 10 */
        EXPECT_EQ_U32(btn_hat & 0x03FFu, (uint32_t)((1u << 0) | (1u << 3) | (1u << 9)));
        EXPECT_EQ_U32((btn_hat >> 10) & 0x0Fu, 6u);
    }
}

static char *read_text_file(const char *path)
{
    FILE *f = NULL;
    long sz;
    char *buf;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void test_umdf2_driver_infs_and_package_outputs(void)
{
    char *xusb_inf = read_text_file("tools\\vgd\\xusb\\AppSandboxVGD_XUSB.inf");
    char *hid_inf  = read_text_file("tools\\vgd\\hid\\AppSandboxVGD_HID.inf");
    char *xusb_src = read_text_file("tools\\vgd\\xusb\\vgd_xusb.c");
    char *hid_src  = read_text_file("tools\\vgd\\hid\\vgd_hid.c");

    EXPECT_TRUE(xusb_inf != NULL);
    if (xusb_inf) {
        EXPECT_TRUE(strstr(xusb_inf, "Class       = System") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "\"UpperFilters\"") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "\"xinputhid\"") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "WUDFRD.inf") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "ProcessSharingDisabled") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "PriorityHigh") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "root\\AppSandboxVGD_XUSB_0") != NULL);
        EXPECT_TRUE(strstr(xusb_inf, "root\\AppSandboxVGD_XUSB_3") != NULL);
        free(xusb_inf);
    }

    EXPECT_TRUE(hid_inf != NULL);
    if (hid_inf) {
        EXPECT_TRUE(strstr(hid_inf, "Class       = HIDClass") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "MsHidUmdf.inf") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "WUDFRD_LowerFilter.NT") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "ProcessSharingDisabled") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "PriorityHigh") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "root\\AppSandboxVGD_HID_0") != NULL);
        EXPECT_TRUE(strstr(hid_inf, "root\\AppSandboxVGD_HID_3") != NULL);
        /* Must NOT attach xinputhid UpperFilter on HID INF */
        EXPECT_TRUE(strstr(hid_inf, "xinputhid") == NULL);
        free(hid_inf);
    }

    EXPECT_TRUE(xusb_src != NULL);
    if (xusb_src) {
        EXPECT_TRUE(strstr(xusb_src, "GUID_DEVINTERFACE_XUSB") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "IOCTL_XUSB_SET_STATE") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "IOCTL_XUSB_WAIT_FOR_INPUT") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "WaitForMultipleObjects") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "WdfFileObjectCanBeOptional") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "XUSB_HANDLE_CTX") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "vgd_shm_format_doorbells_w") != NULL);
        EXPECT_TRUE(strstr(xusb_src, "vgd_shm_read_next_state") != NULL);
        free(xusb_src);
    }

    EXPECT_TRUE(hid_src != NULL);
    if (hid_src) {
        EXPECT_TRUE(strstr(hid_src, "IOCTL_HID_GET_DEVICE_DESCRIPTOR") != NULL);
        EXPECT_TRUE(strstr(hid_src, "IOCTL_HID_GET_REPORT_DESCRIPTOR") != NULL);
        EXPECT_TRUE(strstr(hid_src, "IOCTL_HID_READ_REPORT") != NULL);
        EXPECT_TRUE(strstr(hid_src, "WaitForMultipleObjects") != NULL);
        EXPECT_TRUE(strstr(hid_src, "WdfFileObjectCanBeOptional") != NULL);
        EXPECT_TRUE(strstr(hid_src, "vgd_shm_format_doorbells_w") != NULL);
        EXPECT_TRUE(strstr(hid_src, "vgd_shm_read_next_state") != NULL);
        free(hid_src);
    }

    {
        char *host_src = read_text_file("src\\backend_win\\vm_gamepad.c");
        EXPECT_TRUE(host_src != NULL);
        if (host_src) {
            EXPECT_TRUE(strstr(host_src, "timeBeginPeriod(1)") != NULL);
            EXPECT_TRUE(strstr(host_src, "timeEndPeriod(1)") != NULL);
            EXPECT_TRUE(strstr(host_src, "Sleep(2)") != NULL);
            free(host_src);
        }
    }
}

void test_shm_xusb_and_hid_codecs(void)
{
    test_shm_contract();
    test_xusb_codec();
    test_hid_codec();
}

typedef struct MockVgdBackend {
    int plug_calls[GAMEPAD_MAX_SLOTS];
    int unplug_calls[GAMEPAD_MAX_SLOTS];
    int state_calls[GAMEPAD_MAX_SLOTS];
    int name_calls[GAMEPAD_MAX_SLOTS];
    char last_name[GAMEPAD_MAX_SLOTS][GAMEPAD_NAME_MAX_LEN];
    VgdShmBlock shm[GAMEPAD_MAX_SLOTS];
} MockVgdBackend;

static int mock_plug(void *ctx, uint8_t slot)
{
    MockVgdBackend *m = (MockVgdBackend *)ctx;
    m->plug_calls[slot]++;
    if (vgd_shm_init(&m->shm[slot], slot) != 0) return 0;
    if (m->last_name[slot][0] != '\0') {
        (void)vgd_shm_set_product_name_utf8(&m->shm[slot], m->last_name[slot]);
    }
    return 1;
}

static int mock_unplug(void *ctx, uint8_t slot)
{
    MockVgdBackend *m = (MockVgdBackend *)ctx;
    m->unplug_calls[slot]++;
    m->shm[slot].connected = 0u;
    return 1;
}

static int mock_write_state(void *ctx, uint8_t slot, const GamepadState *state)
{
    MockVgdBackend *m = (MockVgdBackend *)ctx;
    m->state_calls[slot]++;
    return vgd_shm_write_state(&m->shm[slot], state) == 0 ? 1 : 0;
}

static void mock_set_slot_name(void *ctx, uint8_t slot, const char *utf8_name)
{
    MockVgdBackend *m = (MockVgdBackend *)ctx;
    m->name_calls[slot]++;
    memset(m->last_name[slot], 0, sizeof(m->last_name[slot]));
    if (utf8_name) {
        size_t len = strlen(utf8_name);
        if (len >= sizeof(m->last_name[slot])) len = sizeof(m->last_name[slot]) - 1u;
        memcpy(m->last_name[slot], utf8_name, len);
    }
    if (vgd_shm_is_valid(&m->shm[slot])) {
        (void)vgd_shm_set_product_name_utf8(&m->shm[slot], m->last_name[slot]);
    }
}

static void test_vgd_controller_slot_lifecycle_and_rumble(void)
{
    VgdControllerMgr mgr;
    MockVgdBackend mock;
    VgdBackendOps ops;
    GamepadPacket pkt, rumble_pkt;
    GamepadState st, read_back;
    uint32_t gen = 0;

    memset(&mock, 0, sizeof(mock));
    memset(&ops, 0, sizeof(ops));
    vgd_ctrl_init(&mgr);
    ops.ctx = &mock;
    ops.plug_slot = mock_plug;
    ops.unplug_slot = mock_unplug;
    ops.write_state = mock_write_state;
    ops.set_slot_name = mock_set_slot_name;

    /* Plug slot 0 (idempotent on duplicate plug) */
    asb_gamepad_make_plug(0, 1, &pkt);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    EXPECT_EQ_I32(mock.plug_calls[0], 1);
    EXPECT_EQ_U32(mgr.plugged[0], 1u);

    /* State update writes through to shared memory */
    memset(&st, 0, sizeof(st));
    st.buttons = GAMEPAD_BTN_X | GAMEPAD_BTN_RIGHT_SHOULDER;
    st.right_trigger = 199;
    st.thumb_lx = 9000;
    asb_gamepad_make_state(0, &st, 2, &pkt);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    EXPECT_EQ_I32(mock.state_calls[0], 1);
    EXPECT_EQ_I32(vgd_shm_read_state(&mock.shm[0], &read_back, &gen), 0);
    EXPECT_EQ_U32(gen, 1u);
    EXPECT_EQ_U32(read_back.buttons, st.buttons);
    EXPECT_EQ_U32(read_back.right_trigger, 199u);

    /* Plug slot 2 as well, then unplug slot 0, then unplug_all cleans slot 2 */
    asb_gamepad_make_plug(2, 3, &pkt);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    asb_gamepad_make_unplug(0, 4, &pkt);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    EXPECT_EQ_I32(mock.unplug_calls[0], 1);
    EXPECT_EQ_U32(mgr.plugged[0], 0u);
    EXPECT_EQ_U32(mgr.plugged[2], 1u);

    vgd_ctrl_unplug_all(&mgr, &ops);
    EXPECT_EQ_I32(mock.unplug_calls[2], 1);
    EXPECT_EQ_U32(mgr.plugged[2], 0u);

    /* Rumble packet encoding */
    EXPECT_EQ_I32(vgd_ctrl_encode_rumble(1, 240, 80, 7, &rumble_pkt), 1);
    EXPECT_EQ_U32(rumble_pkt.type, GAMEPAD_PKT_RUMBLE);
    EXPECT_EQ_U32(rumble_pkt.slot, 1u);
    EXPECT_EQ_U32(rumble_pkt.large_motor, 240u);
    EXPECT_EQ_U32(rumble_pkt.small_motor, 80u);
    EXPECT_EQ_I32(vgd_ctrl_encode_rumble(4, 10, 10, 8, &rumble_pkt), 0);
}

static void test_pnp_identity_and_retail_hid_layout(void)
{
    char xinst[96], hinst[96];
    char msz[160];
    size_t len = 0;
    const char *compat;
    const char *second;

    EXPECT_EQ_I32(vgd_format_pnp_instance_ids(0, xinst, sizeof(xinst), hinst, sizeof(hinst)), 0);
    EXPECT_TRUE(strcmp(xinst, "ROOT\\VID_045E&PID_028E&XI_00\\ASB_SLOT_0") == 0);
    EXPECT_TRUE(strcmp(hinst, "ROOT\\VID_045E&PID_028E&IG_00\\ASB_SLOT_0") == 0);
    EXPECT_EQ_I32(vgd_format_pnp_instance_ids(3, xinst, sizeof(xinst), hinst, sizeof(hinst)), 0);
    EXPECT_TRUE(strcmp(xinst, "ROOT\\VID_045E&PID_028E&XI_00\\ASB_SLOT_3") == 0);
    EXPECT_TRUE(strcmp(hinst, "ROOT\\VID_045E&PID_028E&IG_00\\ASB_SLOT_3") == 0);
    EXPECT_EQ_I32(vgd_format_pnp_instance_ids(4, xinst, sizeof(xinst), hinst, sizeof(hinst)), -1);

    /* Multi-SZ Hardware IDs carry VID_045E&PID_028E first and slot-specific ID second */
    EXPECT_EQ_I32(vgd_format_pnp_hwids_msz(2, 1, msz, sizeof(msz), &len), 0);
    EXPECT_TRUE(strcmp(msz, "root\\VID_045E&PID_028E&XI_00") == 0);
    second = msz + strlen(msz) + 1;
    EXPECT_TRUE(strcmp(second, "root\\AppSandboxVGD_XUSB_2") == 0);
    EXPECT_EQ_I32(vgd_parse_slot_from_hwid(second), 2);
    EXPECT_TRUE(len == strlen(msz) + 1 + strlen(second) + 2);
    EXPECT_EQ_U32((uint32_t)msz[len - 1], 0u);
    EXPECT_EQ_U32((uint32_t)msz[len - 2], 0u);

    EXPECT_EQ_I32(vgd_format_pnp_hwids_msz(1, 0, msz, sizeof(msz), &len), 0);
    EXPECT_TRUE(strcmp(msz, "root\\VID_045E&PID_028E&IG_00") == 0);
    second = msz + strlen(msz) + 1;
    EXPECT_TRUE(strcmp(second, "root\\AppSandboxVGD_HID_1") == 0);
    EXPECT_EQ_I32(vgd_parse_slot_from_hwid(second), 1);

    /* Compatible IDs carry USB\MS_COMP_XUSB10 */
    compat = vgd_get_compat_ids_msz(1, &len);
    EXPECT_TRUE(compat != NULL && len > 20);
    EXPECT_TRUE(strcmp(compat, "USB\\MS_COMP_XUSB10") == 0);
    compat = vgd_get_compat_ids_msz(0, &len);
    EXPECT_TRUE(compat != NULL && len > 20);
    EXPECT_TRUE(strcmp(compat, "USB\\MS_COMP_XUSB10") == 0);
}

static void test_shm_ring_preserves_short_taps_and_split_doorbells(void)
{
    VgdShmBlock blk;
    GamepadState s, out;
    uint32_t cur = 0, gen = 0;
    uint16_t last_btn = 0;
    char xusb_in[64], hid_in[64];
    int i;

    /* Dedicated per-driver doorbell names (REQ-019) */
    EXPECT_EQ_I32(vgd_shm_format_doorbells(0, xusb_in, sizeof(xusb_in), hid_in, sizeof(hid_in)), 0);
    EXPECT_TRUE(strcmp(xusb_in, "Global\\AppSandboxGamepadXusbIn0") == 0);
    EXPECT_TRUE(strcmp(hid_in, "Global\\AppSandboxGamepadHidIn0") == 0);
    EXPECT_TRUE(strcmp(xusb_in, hid_in) != 0);
    EXPECT_EQ_I32(vgd_shm_format_doorbells(4, xusb_in, sizeof(xusb_in), hid_in, sizeof(hid_in)), -1);

    EXPECT_EQ_I32(vgd_shm_init(&blk, 0), 0);
    EXPECT_EQ_U32(blk.version, 1u);
    EXPECT_EQ_U32(blk.ring_magic, VGD_RING_MAGIC);

    {
        char *agent_src = read_text_file("tools\\agent\\agent.c");
        EXPECT_TRUE(agent_src != NULL);
        if (agent_src) {
            EXPECT_TRUE(strstr(agent_src, "/delete-driver") != NULL);
            EXPECT_TRUE(strstr(agent_src, "AppSandboxVGD_") != NULL);
            free(agent_src);
        }
    }

    /* Simulate rapid short button tap (B press -> B release) while stick also moves:
     * reader MUST observe B=1 first and B=0 second, never coalescing away the press (REQ-020). */
    memset(&s, 0, sizeof(s));
    s.buttons = GAMEPAD_BTN_B;
    s.thumb_lx = 5000;
    EXPECT_EQ_I32(vgd_shm_write_state(&blk, &s), 0);

    s.buttons = 0;
    s.thumb_lx = 6000;
    EXPECT_EQ_I32(vgd_shm_write_state(&blk, &s), 0);

    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 1);
    EXPECT_EQ_U32(out.buttons, GAMEPAD_BTN_B);
    EXPECT_EQ_I32(out.thumb_lx, 5000);
    EXPECT_EQ_U32(cur, 1u);

    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 1);
    EXPECT_EQ_U32(out.buttons, 0u);
    EXPECT_EQ_I32(out.thumb_lx, 6000);
    EXPECT_EQ_U32(cur, 2u);

    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 0);

    /* Pure analog stick burst (no button edge): coalesces straight to latest stick position (zero lag) */
    for (i = 1; i <= 5; i++) {
        memset(&s, 0, sizeof(s));
        s.thumb_lx = (int16_t)(i * 1000);
        EXPECT_EQ_I32(vgd_shm_write_state(&blk, &s), 0);
    }
    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 1);
    EXPECT_EQ_I32(out.thumb_lx, 5000);
    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 0);

    /* Overflow (>16 writes without reading) resyncs cleanly without hanging */
    for (i = 0; i < 24; i++) {
        memset(&s, 0, sizeof(s));
        s.thumb_ry = (int16_t)(i * 100);
        if (i == 22) s.buttons = GAMEPAD_BTN_A;
        EXPECT_EQ_I32(vgd_shm_write_state(&blk, &s), 0);
    }
    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 1);
    EXPECT_EQ_U32(out.buttons, GAMEPAD_BTN_A);
    EXPECT_EQ_I32(vgd_shm_read_next_state(&blk, &cur, &last_btn, &out, &gen), 1);
    EXPECT_EQ_U32(out.buttons, 0u);
    EXPECT_EQ_I32(out.thumb_ry, 2300);
}

static void test_dynamic_controller_name_chunking_and_shm(void)
{
    const char *host_name = "Controller (Xbox One For Windows)";
    size_t nlen = strlen(host_name);
    uint8_t total_chunks = (uint8_t)((nlen + GAMEPAD_NAME_CHUNK_BYTES - 1u) / GAMEPAD_NAME_CHUNK_BYTES);
    VgdControllerMgr mgr;
    MockVgdBackend mock;
    VgdBackendOps ops;
    GamepadPacket pkt;
    uint16_t name_w[VGD_PRODUCT_NAME_MAX];
    size_t wlen, i;
    uint8_t ci;

    EXPECT_EQ_U32((uint32_t)sizeof(GamepadPacket), 24u);
    EXPECT_EQ_U32((uint32_t)sizeof(VgdShmBlock), 448u);
    EXPECT_EQ_U32(total_chunks, 3u);

    /* Reject malformed chunk metadata */
    EXPECT_EQ_I32(asb_gamepad_make_name_chunk(0, 0, 0, "abc", 1, &pkt), 0);
    EXPECT_EQ_I32(asb_gamepad_make_name_chunk(0, 3, 3, "abc", 1, &pkt), 0);
    EXPECT_EQ_I32(asb_gamepad_make_name_chunk(0, 0, 6, "abc", 1, &pkt), 0);
    EXPECT_EQ_I32(asb_gamepad_make_name_chunk(4, 0, 1, "abc", 1, &pkt), 0);

    memset(&mock, 0, sizeof(mock));
    memset(&ops, 0, sizeof(ops));
    vgd_ctrl_init(&mgr);
    ops.ctx = &mock;
    ops.plug_slot = mock_plug;
    ops.unplug_slot = mock_unplug;
    ops.write_state = mock_write_state;
    ops.set_slot_name = mock_set_slot_name;

    /* Send GAMEPAD_PKT_NAME chunks before GAMEPAD_PKT_PLUG (REQ-023 / REQ-024) */
    for (ci = 0; ci < total_chunks; ci++) {
        char slice[GAMEPAD_NAME_CHUNK_BYTES + 1];
        size_t off = (size_t)ci * GAMEPAD_NAME_CHUNK_BYTES;
        size_t rem = nlen - off;
        size_t take = rem > GAMEPAD_NAME_CHUNK_BYTES ? GAMEPAD_NAME_CHUNK_BYTES : rem;
        memset(slice, 0, sizeof(slice));
        memcpy(slice, host_name + off, take);
        EXPECT_EQ_I32(asb_gamepad_make_name_chunk(0, ci, total_chunks, slice, (uint32_t)(10 + ci), &pkt), 1);
        EXPECT_EQ_I32(asb_gamepad_packet_validate(&pkt), 1);
        EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    }

    EXPECT_EQ_I32(mock.name_calls[0], 1);
    EXPECT_TRUE(strcmp(mgr.product_name[0], host_name) == 0);
    EXPECT_TRUE(strcmp(mock.last_name[0], host_name) == 0);

    /* Plug slot 0 -> SHM block stores UTF-16LE product_name */
    asb_gamepad_make_plug(0, 20, &pkt);
    EXPECT_EQ_I32(vgd_ctrl_handle_packet(&mgr, &pkt, &ops), 1);
    memset(name_w, 0, sizeof(name_w));
    wlen = vgd_shm_get_product_name_w(&mock.shm[0], name_w, VGD_PRODUCT_NAME_MAX);
    EXPECT_EQ_U32((uint32_t)wlen, (uint32_t)nlen);
    for (i = 0; i < nlen; i++) {
        EXPECT_EQ_U32((uint32_t)name_w[i], (uint32_t)(unsigned char)host_name[i]);
    }
    EXPECT_EQ_U32((uint32_t)name_w[nlen], 0u);

    /* Clearing name returns 0 length so HID falls back to default Xbox 360 label */
    EXPECT_EQ_I32(vgd_shm_set_product_name_utf8(&mock.shm[0], ""), 0);
    EXPECT_EQ_U32((uint32_t)vgd_shm_get_product_name_w(&mock.shm[0], name_w, VGD_PRODUCT_NAME_MAX), 0u);

    /* Verify HID driver queries vgd_shm_get_product_name_w and vgd_controller sets FriendlyName + OEMName */
    {
        char *hid_src = read_text_file("tools\\vgd\\hid\\vgd_hid.c");
        char *ctrl_src = read_text_file("tools\\agent\\vgd_controller.c");
        EXPECT_TRUE(hid_src != NULL);
        if (hid_src) {
            EXPECT_TRUE(strstr(hid_src, "vgd_shm_get_product_name_w") != NULL);
            free(hid_src);
        }
        EXPECT_TRUE(ctrl_src != NULL);
        if (ctrl_src) {
            EXPECT_TRUE(strstr(ctrl_src, "SPDRP_FRIENDLYNAME") != NULL);
            EXPECT_TRUE(strstr(ctrl_src, "OEMName") != NULL);
            EXPECT_TRUE(strstr(ctrl_src, "OEM Name") != NULL);
            free(ctrl_src);
        }
    }
}

int main(void)
{
    test_shm_xusb_and_hid_codecs();
    test_umdf2_driver_infs_and_package_outputs();
    test_vgd_controller_slot_lifecycle_and_rumble();
    test_pnp_identity_and_retail_hid_layout();
    test_shm_ring_preserves_short_taps_and_split_doorbells();
    test_dynamic_controller_name_chunking_and_shm();
    if (g_failures > 0) {
        fprintf(stderr, "FAILED: %d assertion(s) failed\n", g_failures);
        return 1;
    }
    printf("PASS: test_vgd_codecs\n");
    return 0;
}
