#ifndef ASB_VM_GAMEPAD_H
#define ASB_VM_GAMEPAD_H

#include <stdint.h>
#include "../core/gamepad_protocol.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VM_GAMEPAD_KEEPALIVE_MS 500u

typedef void (*VmGamepadLogFn)(void *ctx, const wchar_t *msg);

typedef struct VmGamepadOps {
    void *ctx;
    int      (*read_slot)(void *ctx, uint8_t slot, GamepadState *out_state);
    int      (*send_packet)(void *ctx, const GamepadPacket *pkt);
    void     (*set_rumble)(void *ctx, uint8_t slot, uint8_t large_motor, uint8_t small_motor);
    uint64_t (*now_ms)(void *ctx);
    void     (*on_log)(void *ctx, const wchar_t *msg);
    int      (*read_name)(void *ctx, uint8_t slot, char *utf8_out, size_t cap);
} VmGamepadOps;

typedef struct VmGamepadCore {
    uint8_t      connected[GAMEPAD_MAX_SLOTS];
    GamepadState last_sent[GAMEPAD_MAX_SLOTS];
    uint64_t     last_send_ms[GAMEPAD_MAX_SLOTS];
    uint8_t      dirty[GAMEPAD_MAX_SLOTS];
    int          forwarding_enabled;
    uint32_t     seq;
} VmGamepadCore;

void vm_gamepad_core_init(VmGamepadCore *core, int forwarding_enabled);
void vm_gamepad_core_step(VmGamepadCore *core, const VmGamepadOps *ops);
void vm_gamepad_core_set_forwarding(VmGamepadCore *core, int enabled, const VmGamepadOps *ops);
int  vm_gamepad_core_handle_rx(VmGamepadCore *core, const GamepadPacket *pkt, const VmGamepadOps *ops);
void vm_gamepad_core_on_disconnect(VmGamepadCore *core, const VmGamepadOps *ops);

#ifdef _WIN32
typedef struct VmGamepad VmGamepad;

/* Start host-side gamepad bridge for a VM viewer.
 * vm_guid: HCS VM RuntimeId GUID.
 * os_type: L"Windows" or L"Linux" (selects HV service GUID).
 * initial_forwarding: initial (focused && share_gamepad) state.
 */
VmGamepad *vm_gamepad_start(const GUID *vm_guid, const wchar_t *os_type,
                            BOOL initial_forwarding, VmGamepadLogFn log_fn, void *log_ctx);

/* Update Focus-Gated Forwarding state (focused && share_gamepad).
 * Thread-safe; immediately neutralizes guest state & zeros rumble when set to FALSE.
 */
void vm_gamepad_set_forwarding(VmGamepad *gp, BOOL enabled);

/* Stop threads, zero host rumble, close socket, and free resources. */
void vm_gamepad_stop(VmGamepad *gp);

/* Runtime check helper: attempts dynamic load of xinput1_4.dll (returns TRUE if available). */
BOOL vm_gamepad_xinput_available(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ASB_VM_GAMEPAD_H */
