/*
 * bcd_patch -- build a UEFI Windows-boot BCD store from the per-ISO BCD-Template.
 *
 * Reads \Windows\System32\config\BCD-Template (extracted fresh from the provided
 * ISO's install.wim), and produces a working \EFI\Microsoft\Boot\BCD by:
 *   - overwriting {bootmgr}.23000003 (displayorder placeholder) -> the OS loader,
 *   - CREATING the device/osdevice/default/path elements that the template lacks
 *     (carved into a freshly appended hbin), pointing at our GPT partitions,
 *   - fixing the base-block sequence numbers + XOR checksum.
 *
 * No external dependencies. See docs/appsandbox/BCD-BUILD-SPEC.md for the
 * regf + BCD-device-element layout this implements.
 */
#ifndef ASB_BCD_PATCH_H
#define ASB_BCD_PATCH_H

#include <stdint.h>
#include <stddef.h>

/* All 16-byte GUIDs are in GPT/registry mixed-endian on-disk form — i.e. exactly
 * the bytes our gpt.c writes into the partition-entry/header GUID fields. Copy
 * them straight from the GPT structures; do NOT re-encode. */
typedef struct {
    uint8_t disk_guid[16];      /* GPT header DiskGUID */
    uint8_t esp_part_guid[16];  /* ESP partition unique GUID  -> {bootmgr} device */
    uint8_t win_part_guid[16];  /* Windows partition unique GUID -> loader device/osdevice */
} bcd_guids_t;

/* Build the patched BCD. `tpl`/`tpl_len` = the raw BCD-Template hive bytes.
 * On success returns 0 and sets *out (malloc'd) / *out_len to the finished hive
 * (write it verbatim to the ESP). Returns non-zero on malformed input. */
int bcd_build(const uint8_t *tpl, size_t tpl_len, const bcd_guids_t *g,
              uint8_t **out, size_t *out_len);

int windows_boot_vioscsi(const uint8_t *system, size_t system_len,
                         const char *published_inf,
                         uint8_t **out, size_t *out_len);

#define ASB_VIOSCSI_DRIVER_PACKAGE "vioscsi.inf_arm64_69dbd3a18ad1eead"

#endif /* ASB_BCD_PATCH_H */
