/*
 * gamepad_provision.c -- Pure-C SHA-256 hex formatting and Pre-Boot Guest Sync
 * decision helpers for appsandbox-gamepad provisioning.
 */

#include "gamepad_provision.h"
#include <string.h>

// @implements spec-ft-gamepad-provisioning::TASK-GP-CACHE-001::REQ-001
// @implements spec-ft-gamepad-provisioning::TASK-GP-CACHE-001::REQ-002
void asb_sha256_to_hex(const uint8_t digest[32], char hex_out[65])
{
    static const char lut[] = "0123456789abcdef";
    int i;
    if (!hex_out) return;
    if (!digest) {
        hex_out[0] = '\0';
        return;
    }
    for (i = 0; i < 32; i++) {
        hex_out[i * 2]     = lut[(digest[i] >> 4) & 0x0Fu];
        hex_out[i * 2 + 1] = lut[digest[i] & 0x0Fu];
    }
    hex_out[64] = '\0';
}

// @implements spec-ft-gamepad-provisioning::TASK-GP-SYNC-002::REQ-005
// @implements spec-ft-gamepad-provisioning::TASK-GP-SYNC-002::REQ-007
// @implements spec-ft-gamepad-provisioning::TASK-GP-SYNC-002::REQ-009
// @implements spec-ft-gamepad-provisioning::TASK-GP-E2E-003::REQ-006
// @implements spec-ft-gamepad-provisioning::TASK-GP-E2E-003::REQ-009
int asb_gamepad_sync_needed(const char *recorded_hash, const char *current_hash)
{
    if (!current_hash || current_hash[0] == '\0')
        return 0;
    if (!recorded_hash || recorded_hash[0] == '\0')
        return 1;
    return strcmp(recorded_hash, current_hash) != 0 ? 1 : 0;
}
