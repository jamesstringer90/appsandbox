#ifndef ASB_MAC_INPUT_KEYS_H
#define ASB_MAC_INPUT_KEYS_H
#include <Carbon/Carbon.h>
#include <stdint.h>
#include "../core/protocol.h"

extern const uint16_t asb_mac_scan_codes[128];
int asb_mac_key_code(const InputPacket *packet);

uint32_t asb_mac_text_packets(uint32_t scalar,InputPacket packets[8],unsigned *count);

#endif
