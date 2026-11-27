#ifndef ASB_GAMEPAD_PROVISION_H
#define ASB_GAMEPAD_PROVISION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void asb_sha256_to_hex(const uint8_t digest[32], char hex_out[65]);
int  asb_gamepad_sync_needed(const char *recorded_hash, const char *current_hash);

#ifdef __cplusplus
}
#endif

#endif /* ASB_GAMEPAD_PROVISION_H */
