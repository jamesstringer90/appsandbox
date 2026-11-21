/*
 * gpt -- hand-written GUID Partition Table for the Windows guest disk.
 *
 * The Windows host path (iso-patch.c) delegates GPT creation to the OS via
 * IOCTL_DISK_SET_DRIVE_LAYOUT_EX. On macOS we author every byte ourselves
 * (no diskutil/gpt(8)), matching "all disk writing is ours". Layout matches
 * iso-patch.c do_to_vhdx exactly: 1 MiB alignment, ESP (FAT32) + MSR + Windows
 * (basic data), with the canonical Microsoft partition type GUIDs.
 */
#ifndef ASB_GPT_H
#define ASB_GPT_H

#include "blockio.h"
#include <stdint.h>

/* Partition LBA ranges the caller needs to then format. counts are in sectors. */
typedef struct {
    uint32_t sector_size;
    uint64_t esp_start_lba,  esp_count_lba;   /* EFI System Partition (FAT32) */
    uint64_t msr_start_lba,  msr_count_lba;   /* Microsoft Reserved (unformatted) */
    uint64_t win_start_lba,  win_count_lba;   /* Windows (NTFS) */
    /* GUIDs in on-disk mixed-endian form (ready to memcpy into a BCD device blob) */
    uint8_t  disk_guid[16];
    uint8_t  esp_part_guid[16];
    uint8_t  win_part_guid[16];
} gpt_layout_t;

/* Write a protective MBR + primary/backup GPT describing ESP/MSR/Windows onto
 * the image. esp_mb/msr_mb are the ESP and MSR sizes in MiB (e.g. 200, 128);
 * Windows fills the remainder. Fills *out with the resulting LBA ranges.
 * Returns 0 on success, -1 on error. */
int gpt_write_windows(blockio_t *io, uint32_t sector_size,
                      uint64_t esp_mb, uint64_t msr_mb, gpt_layout_t *out);
int gpt_grow_disk(blockio_t *io, uint64_t bytes, int mac_guest);

#endif /* ASB_GPT_H */
