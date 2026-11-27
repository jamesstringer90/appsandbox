/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-006 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-007 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-008 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-009 */
#ifndef APPSANDBOX_VGD_CONTROLLER_H
#define APPSANDBOX_VGD_CONTROLLER_H

#include <stdint.h>
#include <stddef.h>
#include "../../src/core/gamepad_protocol.h"
#include "../vgd/vgd_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VgdBackendOps {
    void *ctx;
    int  (*plug_slot)(void *ctx, uint8_t slot);
    int  (*unplug_slot)(void *ctx, uint8_t slot);
    int  (*write_state)(void *ctx, uint8_t slot, const GamepadState *state);
    void (*set_slot_name)(void *ctx, uint8_t slot, const char *utf8_name);
} VgdBackendOps;

#define VGD_MIN_BTN_HOLD_MS 28u

typedef struct VgdControllerMgr {
    uint8_t      plugged[GAMEPAD_MAX_SLOTS];
    GamepadState last_state[GAMEPAD_MAX_SLOTS];
    GamepadState raw_state[GAMEPAD_MAX_SLOTS];
    uint64_t     btn_press_ms[GAMEPAD_MAX_SLOTS][16];
    uint16_t     held_buttons[GAMEPAD_MAX_SLOTS];
    char         product_name[GAMEPAD_MAX_SLOTS][GAMEPAD_NAME_MAX_LEN];
    uint8_t      name_chunks_mask[GAMEPAD_MAX_SLOTS];
    uint8_t      name_total_chunks[GAMEPAD_MAX_SLOTS];
} VgdControllerMgr;

/* Pure-C testable controller slot state machine */
void vgd_ctrl_init(VgdControllerMgr *mgr);
int  vgd_ctrl_handle_packet(VgdControllerMgr *mgr, const GamepadPacket *pkt, const VgdBackendOps *ops);
int  vgd_ctrl_apply_state_timed(VgdControllerMgr *mgr, uint8_t slot, const GamepadState *raw_state,
                                uint64_t now_ms, const VgdBackendOps *ops);
int  vgd_ctrl_tick(VgdControllerMgr *mgr, uint64_t now_ms, const VgdBackendOps *ops);
void vgd_ctrl_unplug_all(VgdControllerMgr *mgr, const VgdBackendOps *ops);
int  vgd_ctrl_encode_rumble(uint8_t slot, uint8_t large_motor, uint8_t small_motor,
                            uint32_t seq, GamepadPacket *out);

#if defined(_WIN32) && !defined(VGD_CTRL_NO_WIN32)
#include <windows.h>

typedef void (*VgdLogFn)(const char *fmt, ...);

typedef struct VgdWinSlot {
    HANDLE       shm_handle;
    VgdShmBlock *shm;
    HANDLE       xusb_in_evt;
    HANDLE       hid_in_evt;
    HANDLE       out_evt;
    uint32_t     last_rumble_gen;
    int          active;
    char         product_name_utf8[GAMEPAD_NAME_MAX_LEN];
    wchar_t      product_name_w[VGD_PRODUCT_NAME_MAX];
} VgdWinSlot;

typedef struct VgdWinBackend {
    wchar_t    xusb_inf_path[MAX_PATH];
    wchar_t    hid_inf_path[MAX_PATH];
    VgdLogFn   log_fn;
    VgdWinSlot slots[GAMEPAD_MAX_SLOTS];
} VgdWinBackend;

int  vgd_win_backend_init(VgdWinBackend *b, const wchar_t *drivers_dir, VgdLogFn log_fn);
void vgd_win_backend_cleanup_stale(VgdWinBackend *b);
int  vgd_win_plug_slot(void *ctx, uint8_t slot);
int  vgd_win_unplug_slot(void *ctx, uint8_t slot);
int  vgd_win_write_state(void *ctx, uint8_t slot, const GamepadState *state);
void vgd_win_set_slot_name(void *ctx, uint8_t slot, const char *utf8_name);
void vgd_win_sync_oem_names_if_needed(VgdWinBackend *b);

/* Blocks until rumble arrives on slot's doorbell event or stop_event is signaled.
 * Returns 1 if rumble updated, 0 if stopped/unplugged, -1 on error. */
int  vgd_win_wait_rumble(VgdWinBackend *b, uint8_t slot, HANDLE stop_event,
                         uint8_t *out_large, uint8_t *out_small);
#endif

#ifdef __cplusplus
}
#endif

#endif /* APPSANDBOX_VGD_CONTROLLER_H */
