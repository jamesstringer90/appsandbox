#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/gamepad_protocol.h"
#include "../src/backend_win/vm_gamepad.h"
#include "../tools/transport/asb_transport.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
        g_failures++; \
    } \
} while (0)

// @verifies spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-001
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-002
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-PROTO-001::REQ-012
static void test_packet_validation_and_neutral_state(void)
{
    GamepadPacket pkt;
    GamepadState s1, s2;

    CHECK(ASB_CH_GAMEPAD == 8, "ASB_CH_GAMEPAD must be channel 8");
    CHECK(sizeof(GamepadState) == 12, "GamepadState must be packed 12 bytes");
    CHECK(sizeof(GamepadPacket) == 24, "GamepadPacket must be packed 24 bytes");
    CHECK(GAMEPAD_MAGIC != DISPLAY_GPU_MAGIC, "GAMEPAD_MAGIC must not collide with DISPLAY_GPU_MAGIC");
    CHECK(GAMEPAD_READY_MAGIC == 0x59445247u, "GAMEPAD_READY_MAGIC must be GRDY");

    asb_gamepad_make_plug(2, 42, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 1, "valid PLUG packet must validate");
    CHECK(pkt.magic == GAMEPAD_MAGIC, "PLUG magic must match");
    CHECK(pkt.type == GAMEPAD_PKT_PLUG, "PLUG type must match");
    CHECK(pkt.slot == 2, "PLUG slot must match");
    CHECK(pkt.seq == 42, "PLUG seq must match");

    asb_gamepad_make_unplug(3, 43, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 1, "valid UNPLUG packet must validate");
    CHECK(pkt.type == GAMEPAD_PKT_UNPLUG && pkt.slot == 3, "UNPLUG fields must match");

    memset(&s1, 0, sizeof(s1));
    s1.buttons = GAMEPAD_BTN_A | GAMEPAD_BTN_GUIDE | GAMEPAD_BTN_LEFT_SHOULDER;
    s1.left_trigger = 128;
    s1.right_trigger = 255;
    s1.thumb_lx = -32768;
    s1.thumb_ly = 32767;
    s1.thumb_rx = 1024;
    s1.thumb_ry = -1024;
    s2 = s1;
    CHECK(asb_gamepad_state_equal(&s1, &s2) == 1, "identical states must compare equal");
    s2.thumb_ry = -1023;
    CHECK(asb_gamepad_state_equal(&s1, &s2) == 0, "differing states must not compare equal");

    asb_gamepad_make_state(0, &s1, 99, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 1, "valid STATE packet must validate");
    CHECK(pkt.state.buttons == s1.buttons, "STATE buttons preserved");
    CHECK(pkt.state.right_trigger == 255, "STATE right_trigger preserved");
    CHECK(pkt.state.thumb_lx == -32768, "STATE thumb_lx preserved");

    asb_gamepad_make_rumble(1, 200, 80, 7, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 1, "valid RUMBLE packet must validate");
    CHECK(pkt.large_motor == 200 && pkt.small_motor == 80, "RUMBLE motors preserved");

    asb_gamepad_make_neutral(1, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 1, "neutral packet must validate");
    CHECK(pkt.type == GAMEPAD_PKT_STATE && pkt.slot == 1, "neutral packet type and slot");
    CHECK(pkt.state.buttons == 0 && pkt.state.left_trigger == 0 && pkt.state.right_trigger == 0 &&
          pkt.state.thumb_lx == 0 && pkt.state.thumb_ly == 0 &&
          pkt.state.thumb_rx == 0 && pkt.state.thumb_ry == 0,
          "neutral packet must zero all controls");

    CHECK(asb_gamepad_packet_validate(NULL) == 0, "NULL packet rejected");

    asb_gamepad_make_plug(0, 1, &pkt);
    pkt.magic = 0xDEADBEEFu;
    CHECK(asb_gamepad_packet_validate(&pkt) == 0, "invalid magic rejected");

    asb_gamepad_make_plug(0, 1, &pkt);
    pkt.type = 99;
    CHECK(asb_gamepad_packet_validate(&pkt) == 0, "unknown packet type rejected");

    asb_gamepad_make_plug(4, 1, &pkt);
    CHECK(asb_gamepad_packet_validate(&pkt) == 0, "slot >= 4 rejected");
}

typedef struct MockHost {
    int          present[GAMEPAD_MAX_SLOTS];
    GamepadState hw_state[GAMEPAD_MAX_SLOTS];
    char         hw_name[GAMEPAD_MAX_SLOTS][GAMEPAD_NAME_MAX_LEN];
    GamepadPacket sent[64];
    int          sent_count;
    uint8_t      rumble_l[GAMEPAD_MAX_SLOTS];
    uint8_t      rumble_r[GAMEPAD_MAX_SLOTS];
    int          rumble_calls;
    uint64_t     clock_ms;
    int          log_calls;
} MockHost;

static int mock_read_name(void *ctx, uint8_t slot, char *utf8_out, size_t cap)
{
    MockHost *m = (MockHost *)ctx;
    size_t len;
    if (slot >= GAMEPAD_MAX_SLOTS || !m->hw_name[slot][0] || !utf8_out || cap == 0)
        return 0;
    len = strlen(m->hw_name[slot]);
    if (len >= cap) len = cap - 1u;
    memcpy(utf8_out, m->hw_name[slot], len);
    utf8_out[len] = '\0';
    return (int)len;
}

static int mock_read_slot(void *ctx, uint8_t slot, GamepadState *out_state)
{
    MockHost *m = (MockHost *)ctx;
    if (slot >= GAMEPAD_MAX_SLOTS || !m->present[slot]) return 0;
    *out_state = m->hw_state[slot];
    return 1;
}

static int mock_send_packet(void *ctx, const GamepadPacket *pkt)
{
    MockHost *m = (MockHost *)ctx;
    if (m->sent_count < 64)
        m->sent[m->sent_count++] = *pkt;
    return 1;
}

static void mock_set_rumble(void *ctx, uint8_t slot, uint8_t large_motor, uint8_t small_motor)
{
    MockHost *m = (MockHost *)ctx;
    if (slot < GAMEPAD_MAX_SLOTS) {
        m->rumble_l[slot] = large_motor;
        m->rumble_r[slot] = small_motor;
        m->rumble_calls++;
    }
}

static uint64_t mock_now_ms(void *ctx)
{
    return ((MockHost *)ctx)->clock_ms;
}

static void mock_on_log(void *ctx, const wchar_t *msg)
{
    MockHost *m = (MockHost *)ctx;
    if (msg && msg[0]) m->log_calls++;
}

// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-003
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-004
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-005
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-006
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-007
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-008
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-010
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-011
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-HOST-002::REQ-013
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-004::REQ-015
static void test_host_hotplug_focus_gating_and_rumble(void)
{
    MockHost mock;
    VmGamepadOps ops;
    VmGamepadCore core;
    GamepadPacket rumble_pkt;

    memset(&mock, 0, sizeof(mock));
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &mock;
    ops.read_slot = mock_read_slot;
    ops.send_packet = mock_send_packet;
    ops.set_rumble = mock_set_rumble;
    ops.now_ms = mock_now_ms;
    ops.on_log = mock_on_log;
    ops.read_name = mock_read_name;

    mock.clock_ms = 1000;
    vm_gamepad_core_init(&core, 1);

    /* Connect controller on slot 0 with button A pressed */
    mock.present[0] = 1;
    mock.hw_state[0].buttons = GAMEPAD_BTN_A;
    mock.hw_state[0].left_trigger = 200;
    vm_gamepad_core_step(&core, &ops);

    CHECK(mock.sent_count == 2, "hotplug while focused sends PLUG + initial STATE");
    CHECK(mock.sent[0].type == GAMEPAD_PKT_PLUG && mock.sent[0].slot == 0, "first packet is PLUG slot 0");
    CHECK(mock.sent[1].type == GAMEPAD_PKT_STATE && mock.sent[1].state.buttons == GAMEPAD_BTN_A,
          "second packet carries initial state");

    /* Unchanged poll within keepalive window sends nothing (delta filtering) */
    mock.sent_count = 0;
    mock.clock_ms = 1100;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 0, "unchanged state within keepalive sends 0 packets");

    /* Keepalive expiry sends state */
    mock.clock_ms = 1600;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].type == GAMEPAD_PKT_STATE, "keepalive triggers STATE");

    /* Rumble from guest while focused applies motors (REQ-006) */
    asb_gamepad_make_rumble(0, 180, 90, 1, &rumble_pkt);
    CHECK(vm_gamepad_core_handle_rx(&core, &rumble_pkt, &ops) == 1, "valid rumble accepted");
    CHECK(mock.rumble_l[0] == 180 && mock.rumble_r[0] == 90, "rumble applied to slot 0");

    /* Invalid rumble packet rejected (REQ-012) */
    rumble_pkt.slot = 5;
    CHECK(vm_gamepad_core_handle_rx(&core, &rumble_pkt, &ops) == 0, "out-of-range rumble rejected");

    /* Focus loss sends neutral state and zeros rumble (REQ-007, REQ-008) */
    mock.sent_count = 0;
    vm_gamepad_core_set_forwarding(&core, 0, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].type == GAMEPAD_PKT_STATE &&
          mock.sent[0].state.buttons == 0 && mock.sent[0].state.left_trigger == 0,
          "focus loss emits neutral STATE");
    CHECK(mock.rumble_l[0] == 0 && mock.rumble_r[0] == 0, "focus loss zeros rumble motors");

    /* State changes while unfocused are suppressed (REQ-011) */
    mock.sent_count = 0;
    mock.hw_state[0].buttons = GAMEPAD_BTN_B;
    mock.clock_ms = 2500;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 0, "unfocused input changes suppressed");

    /* Rumble while unfocused is ignored */
    asb_gamepad_make_rumble(0, 255, 255, 2, &rumble_pkt);
    vm_gamepad_core_handle_rx(&core, &rumble_pkt, &ops);
    CHECK(mock.rumble_l[0] == 0 && mock.rumble_r[0] == 0, "rumble suppressed while unfocused");

    /* Regaining focus flushes current physical state */
    mock.sent_count = 0;
    vm_gamepad_core_set_forwarding(&core, 1, &ops);
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].state.buttons == GAMEPAD_BTN_B,
          "refocus immediately flushes held state");

    /* Disconnect controller sends UNPLUG and zeros rumble (REQ-005, REQ-008) */
    asb_gamepad_make_rumble(0, 100, 100, 3, &rumble_pkt);
    vm_gamepad_core_handle_rx(&core, &rumble_pkt, &ops);
    mock.sent_count = 0;
    mock.present[0] = 0;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].type == GAMEPAD_PKT_UNPLUG && mock.sent[0].slot == 0,
          "disconnect emits UNPLUG");
    CHECK(mock.rumble_l[0] == 0 && mock.rumble_r[0] == 0, "disconnect zeros rumble");

    /* Channel disconnect resets connected slots and zeros rumble (REQ-008) */
    mock.present[2] = 1;
    mock.hw_state[2].buttons = GAMEPAD_BTN_X;
    vm_gamepad_core_step(&core, &ops);
    asb_gamepad_make_rumble(2, 220, 110, 4, &rumble_pkt);
    vm_gamepad_core_handle_rx(&core, &rumble_pkt, &ops);
    vm_gamepad_core_on_disconnect(&core, &ops);
    CHECK(mock.rumble_l[2] == 0 && mock.rumble_r[2] == 0, "channel disconnect zeros rumble");
    CHECK(core.connected[2] == 0, "channel disconnect clears connected state for reconnect");

    /* Dynamic XInput availability probe does not crash (REQ-003, REQ-013) */
    (void)vm_gamepad_xinput_available();
    CHECK(mock.log_calls >= 2, "hotplug connect/disconnect emitted diagnostic log callbacks (REQ-015)");
}

// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-007
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-008
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-009
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-010
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-011
// @verifies spec-ft-gamepad-protocol-host::TASK-GP-VIEWER-003::REQ-014
static void test_viewer_share_gamepad_default_and_toggle(void)
{
    MockHost mock;
    VmGamepadOps ops;
    VmGamepadCore core;
    int share_gamepad = 1; /* Default true per REQ-014 */
    int focused = 1;

    memset(&mock, 0, sizeof(mock));
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &mock;
    ops.read_slot = mock_read_slot;
    ops.send_packet = mock_send_packet;
    ops.set_rumble = mock_set_rumble;
    ops.now_ms = mock_now_ms;
    ops.on_log = mock_on_log;
    ops.read_name = mock_read_name;

    vm_gamepad_core_init(&core, share_gamepad && focused);
    mock.present[0] = 1;
    mock.hw_state[0].buttons = GAMEPAD_BTN_Y;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 2 && mock.sent[1].state.buttons == GAMEPAD_BTN_Y,
          "default shareGamepad=true forwards when focused");

    /* User toggles "Share Xbox Controllers" OFF while viewer remains focused */
    mock.sent_count = 0;
    share_gamepad = 0;
    vm_gamepad_core_set_forwarding(&core, share_gamepad && focused, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].state.buttons == 0,
          "toggling shareGamepad OFF neutralizes guest immediately");

    mock.sent_count = 0;
    mock.hw_state[0].buttons = GAMEPAD_BTN_START;
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 0, "toggled OFF suppresses inputs even while focused");

    /* User toggles back ON */
    share_gamepad = 1;
    vm_gamepad_core_set_forwarding(&core, share_gamepad && focused, &ops);
    vm_gamepad_core_step(&core, &ops);
    CHECK(mock.sent_count == 1 && mock.sent[0].state.buttons == GAMEPAD_BTN_START,
          "toggling shareGamepad ON resumes forwarding");
}

// @verifies spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-023
static void test_host_emits_name_chunks_before_plug(void)
{
    MockHost mock;
    VmGamepadOps ops;
    VmGamepadCore core;
    char reassembled[GAMEPAD_NAME_MAX_LEN];
    int i;

    memset(&mock, 0, sizeof(mock));
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &mock;
    ops.read_slot = mock_read_slot;
    ops.send_packet = mock_send_packet;
    ops.set_rumble = mock_set_rumble;
    ops.now_ms = mock_now_ms;
    ops.on_log = mock_on_log;
    ops.read_name = mock_read_name;

    vm_gamepad_core_init(&core, 1);
    mock.present[0] = 1;
    strcpy_s(mock.hw_name[0], sizeof(mock.hw_name[0]), "Controller (Xbox One For Windows)");
    vm_gamepad_core_step(&core, &ops);

    /* Expect 3 NAME chunks (33 bytes / 12 = 3), then PLUG, then initial STATE = 5 packets */
    CHECK(mock.sent_count == 5, "hotplug with 33-byte host name sends 3 NAME + PLUG + STATE");
    CHECK(mock.sent[0].type == GAMEPAD_PKT_NAME && mock.sent[0].large_motor == 0 && mock.sent[0].small_motor == 3,
          "packet 0 is NAME chunk 0/3");
    CHECK(mock.sent[1].type == GAMEPAD_PKT_NAME && mock.sent[1].large_motor == 1 && mock.sent[1].small_motor == 3,
          "packet 1 is NAME chunk 1/3");
    CHECK(mock.sent[2].type == GAMEPAD_PKT_NAME && mock.sent[2].large_motor == 2 && mock.sent[2].small_motor == 3,
          "packet 2 is NAME chunk 2/3");
    CHECK(mock.sent[3].type == GAMEPAD_PKT_PLUG && mock.sent[3].slot == 0,
          "packet 3 is PLUG after NAME chunks");
    CHECK(mock.sent[4].type == GAMEPAD_PKT_STATE && mock.sent[4].slot == 0,
          "packet 4 is initial STATE");

    memset(reassembled, 0, sizeof(reassembled));
    for (i = 0; i < 3; i++) {
        memcpy(reassembled + (size_t)i * GAMEPAD_NAME_CHUNK_BYTES,
               &mock.sent[i].state, GAMEPAD_NAME_CHUNK_BYTES);
    }
    CHECK(strcmp(reassembled, "Controller (Xbox One For Windows)") == 0,
          "reassembled NAME chunks match host controller name");
}

int main(void)
{
    test_packet_validation_and_neutral_state();
    test_host_hotplug_focus_gating_and_rumble();
    test_viewer_share_gamepad_default_and_toggle();
    test_host_emits_name_chunks_before_plug();
    if (g_failures == 0) {
        printf("PASS: test_gamepad_protocol\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertion(s)\n", g_failures);
    return 1;
}
