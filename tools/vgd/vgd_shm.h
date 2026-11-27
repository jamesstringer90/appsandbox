/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-001 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-019 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
#ifndef APPSANDBOX_VGD_SHM_H
#define APPSANDBOX_VGD_SHM_H

#include <stddef.h>
#include <stdint.h>
#include "../../src/core/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VGD_SHM_MAGIC        0x44475641u /* 'AVGD' little-endian */
#define VGD_SHM_VERSION      1u          /* Backward-compatible 64-byte header */
#define VGD_RING_MAGIC       0x52474456u /* 'VDGR' little-endian */
#define VGD_MAX_SLOTS        GAMEPAD_MAX_SLOTS
#define VGD_RING_CAP         16u
#define VGD_RING_MASK        (VGD_RING_CAP - 1u)
#define VGD_PRODUCT_NAME_MAX 64u
#define VGD_SHM_BLOCK_SIZE   448u

#define VGD_XBOX360_VID      0x045Eu
#define VGD_XBOX360_PID      0x028Eu

#pragma pack(push, 1)
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    uint32_t          slot;
    volatile uint32_t connected;
    volatile uint32_t input_seq;  /* seqlock: odd while writing, even when stable */
    GamepadState      state;      /* 12 bytes (latest snapshot) */
    volatile uint32_t rumble_seq; /* seqlock: odd while writing, even when stable */
    volatile uint8_t  large_motor;
    volatile uint8_t  small_motor;
    uint8_t           reserved[26];
    /* Extended lock-free 16-entry input ring preserving button edges (REQ-020) */
    volatile uint32_t ring_write_idx;
    uint32_t          ring_magic;
    GamepadState      ring[VGD_RING_CAP]; /* 16 * 12 = 192 bytes */
    uint8_t           tail_pad[56];       /* ring section = 64 + 8 + 192 + 56 = 320 bytes */
    /* Dynamic UTF-16LE NUL-terminated product name forwarded from host (REQ-024) */
    uint16_t          product_name[VGD_PRODUCT_NAME_MAX]; /* 128 bytes -> total = 448 bytes */
} VgdShmBlock;
#pragma pack(pop)

/* Format per-slot kernel object names (ASCII and UTF-16LE). Returns 0 on success, -1 on invalid slot/buf. */
int vgd_shm_format_names(uint32_t slot,
                         char *shm_name, size_t shm_cap,
                         char *in_evt_name, size_t in_cap,
                         char *out_evt_name, size_t out_cap);

int vgd_shm_format_names_w(uint32_t slot,
                           uint16_t *shm_name, size_t shm_cap,
                           uint16_t *in_evt_name, size_t in_cap,
                           uint16_t *out_evt_name, size_t out_cap);

/* Format dedicated per-driver input doorbell names so XUSB and HID never steal wakeups (REQ-019). */
int vgd_shm_format_doorbells(uint32_t slot,
                             char *xusb_in_name, size_t xusb_cap,
                             char *hid_in_name, size_t hid_cap);

int vgd_shm_format_doorbells_w(uint32_t slot,
                               uint16_t *xusb_in_name, size_t xusb_cap,
                               uint16_t *hid_in_name, size_t hid_cap);

/* Format Hardware IDs for a slot: root\AppSandboxVGD_XUSB_<slot> and root\AppSandboxVGD_HID_<slot>. */
int vgd_format_hwids(uint32_t slot,
                     char *xusb_hwid, size_t xusb_cap,
                     char *hid_hwid, size_t hid_cap);

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-015 */
/* Format deterministic PnP Instance IDs for a slot:
 *   xusb_inst: ROOT\VID_045E&PID_028E&XI_00\ASB_SLOT_<slot>
 *   hid_inst:  ROOT\VID_045E&PID_028E&IG_00\ASB_SLOT_<slot>
 */
int vgd_format_pnp_instance_ids(uint32_t slot,
                                char *xusb_inst, size_t xusb_cap,
                                char *hid_inst, size_t hid_cap);

/* Format double-NUL REG_MULTI_SZ Hardware ID lists (primary VID_045E&PID_028E + slot ID).
 * Writes total byte length including final double-NUL into *out_len. Returns 0 on success. */
int vgd_format_pnp_hwids_msz(uint32_t slot, int is_xusb,
                             char *out, size_t cap, size_t *out_len);

/* Return static double-NUL REG_MULTI_SZ Compatible ID list (USB\MS_COMP_XUSB10 ...) and byte length. */
const char *vgd_get_compat_ids_msz(int is_xusb, size_t *out_len);

/* Parse slot index 0..3 from a Hardware ID or Instance ID string. Returns 0..3 or -1 if invalid. */
int vgd_parse_slot_from_hwid(const char *hwid);

/* Initialize a shared-memory block for a Controller Slot (0..3). Returns 0 on success, -1 on error. */
int vgd_shm_init(VgdShmBlock *blk, uint32_t slot);

/* Validate header magic, version, and slot range. Returns 1 if valid, 0 otherwise. */
int vgd_shm_is_valid(const VgdShmBlock *blk);

/* Atomically write GamepadState using seqlock semantics and increment sequence. Returns 0 on success. */
int vgd_shm_write_state(VgdShmBlock *blk, const GamepadState *state);

/* Read a consistent snapshot of GamepadState and monotonic generation (seq/2). Returns 0 on success. */
int vgd_shm_read_state(const VgdShmBlock *blk, GamepadState *out_state, uint32_t *out_gen);

/* Read next unread state from the 16-entry ring buffer for a reader cursor, preserving every
 * button press/release edge while coalescing intermediate analog-only updates for zero lag.
 * Returns 1 if a new state was delivered (advancing *cursor_inout), 0 if already up to date,
 * or -1 on invalid block/args. */
int vgd_shm_read_next_state(const VgdShmBlock *blk, uint32_t *cursor_inout,
                            uint16_t *last_buttons_inout,
                            GamepadState *out_state, uint32_t *out_gen);

/* Atomically write guest-to-host dual-motor rumble bytes and increment rumble_seq. Returns 0 on success. */
int vgd_shm_write_rumble(VgdShmBlock *blk, uint8_t large_motor, uint8_t small_motor);

/* Read a consistent snapshot of rumble bytes and monotonic generation (rumble_seq/2). Returns 0 on success. */
int vgd_shm_read_rumble(const VgdShmBlock *blk, uint8_t *out_large, uint8_t *out_small, uint32_t *out_gen);

/* Store UTF-8 host product name as NUL-terminated UTF-16LE in blk->product_name. Returns 0 on success. */
int vgd_shm_set_product_name_utf8(VgdShmBlock *blk, const char *utf8_name);

/* Read NUL-terminated UTF-16LE product name from blk->product_name. Returns number of UTF-16 code units (0 if empty). */
size_t vgd_shm_get_product_name_w(const VgdShmBlock *blk, uint16_t *out_w, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* APPSANDBOX_VGD_SHM_H */
