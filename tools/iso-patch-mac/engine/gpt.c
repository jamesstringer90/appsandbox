/* gpt.c -- see gpt.h. */

#include "gpt.h"
#include <stdlib.h>
#include <string.h>

/* Canonical partition type GUIDs, in on-disk (mixed-endian) byte order. */
static const uint8_t TYPE_ESP[16] = {   /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11, 0xBA,0x4B, 0x00,0xA0,0xC9,0x3E,0xC9,0x3B };
static const uint8_t TYPE_MSR[16] = {   /* E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
    0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D, 0x81,0x7D, 0xF9,0x2D,0xF0,0x02,0x15,0xAE };
static const uint8_t TYPE_WIN[16] = {   /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
    0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44, 0x87,0xC0, 0x68,0xB6,0xB7,0x26,0x99,0xC7 };

static uint32_t crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static void rand_guid(uint8_t g[16]) {
    arc4random_buf(g, 16);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* version 4 */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* variant 1 */
}

static void put_le16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_le32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

static uint64_t get_le(const uint8_t *p, size_t len) {
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

/* Fill one 128-byte partition entry. name is ASCII -> UTF-16LE. */
static void fill_entry(uint8_t *e, const uint8_t type[16],
                       uint64_t first, uint64_t last, const char *name) {
    memcpy(e, type, 16);
    rand_guid(e + 16);
    put_le64(e + 32, first);
    put_le64(e + 40, last);
    put_le64(e + 48, 0);                       /* attributes */
    for (int i = 0; name && name[i] && i < 35; i++) put_le16(e + 56 + i*2, (uint16_t)name[i]);
}

int gpt_write_windows(blockio_t *io, uint32_t ss,
                      uint64_t esp_mb, uint64_t msr_mb, gpt_layout_t *out) {
    if (ss != 512) return -1;                   /* layout assumes 512-byte LBAs */
    uint64_t total = blockio_size(io) / ss;
    if (total < 2048 + 64) return -1;

    const uint32_t NENT = 128, ESZ = 128;
    uint32_t arr_sectors = (NENT * ESZ + ss - 1) / ss;   /* 32 */
    uint64_t align = (1024 * 1024) / ss;                 /* 2048 */

    uint64_t primary_hdr = 1;
    uint64_t primary_arr = 2;
    uint64_t backup_hdr  = total - 1;
    uint64_t backup_arr  = total - 1 - arr_sectors;
    uint64_t first_usable = primary_arr + arr_sectors;   /* 34 */
    uint64_t last_usable  = backup_arr - 1;

    /* Partition LBA ranges (1 MiB aligned). */
    uint64_t esp_start = (first_usable + align - 1) / align * align;
    uint64_t esp_count = esp_mb * 1024 * 1024 / ss;
    uint64_t msr_start = esp_start + esp_count;
    uint64_t msr_count = msr_mb * 1024 * 1024 / ss;
    uint64_t win_start = msr_start + msr_count;
    if (win_start > last_usable) return -1;
    uint64_t win_count = (last_usable + 1) - win_start;   /* fill remainder */

    /* Build the 128-entry array (only first 3 used). */
    uint8_t *arr = calloc(NENT, ESZ);
    if (!arr) return -1;
    fill_entry(arr + 0*ESZ, TYPE_ESP, esp_start, esp_start + esp_count - 1, "EFI system partition");
    fill_entry(arr + 1*ESZ, TYPE_MSR, msr_start, msr_start + msr_count - 1, "Microsoft reserved partition");
    fill_entry(arr + 2*ESZ, TYPE_WIN, win_start, win_start + win_count - 1, "Basic data partition");
    uint32_t arr_crc = crc32(arr, NENT * ESZ);

    uint8_t disk_guid[16]; rand_guid(disk_guid);

    /* Header builder (used for primary + backup; my_lba/alt_lba/arr_lba differ). */
    uint8_t hdr[512];
    int rc = -1;
    for (int pass = 0; pass < 2; pass++) {
        memset(hdr, 0, sizeof hdr);
        uint64_t my_lba  = pass == 0 ? primary_hdr : backup_hdr;
        uint64_t alt_lba = pass == 0 ? backup_hdr  : primary_hdr;
        uint64_t arr_lba = pass == 0 ? primary_arr : backup_arr;
        memcpy(hdr, "EFI PART", 8);
        put_le32(hdr + 8, 0x00010000);          /* revision 1.0 */
        put_le32(hdr + 12, 92);                 /* header size */
        /* hdr+16 header_crc = 0 for now */
        put_le64(hdr + 24, my_lba);
        put_le64(hdr + 32, alt_lba);
        put_le64(hdr + 40, first_usable);
        put_le64(hdr + 48, last_usable);
        memcpy(hdr + 56, disk_guid, 16);
        put_le64(hdr + 72, arr_lba);
        put_le32(hdr + 80, NENT);
        put_le32(hdr + 84, ESZ);
        put_le32(hdr + 88, arr_crc);
        put_le32(hdr + 16, crc32(hdr, 92));     /* header CRC over 92 bytes */

        if (blockio_write(io, my_lba * ss, hdr, 512) != 0) goto done;
        if (blockio_write(io, arr_lba * ss, arr, NENT * ESZ) != 0) goto done;
    }

    /* Protective MBR (LBA 0): one 0xEE partition covering the whole disk. */
    uint8_t mbr[512]; memset(mbr, 0, sizeof mbr);
    uint8_t *pe = mbr + 446;
    pe[0] = 0x00;                               /* not bootable */
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;   /* CHS first */
    pe[4] = 0xEE;                               /* type: GPT protective */
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;   /* CHS last */
    put_le32(pe + 8, 1);                        /* first LBA */
    uint64_t prot = total - 1;
    put_le32(pe + 12, prot > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)prot);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (blockio_write(io, 0, mbr, 512) != 0) goto done;

    out->sector_size = ss;
    out->esp_start_lba = esp_start; out->esp_count_lba = esp_count;
    out->msr_start_lba = msr_start; out->msr_count_lba = msr_count;
    out->win_start_lba = win_start; out->win_count_lba = win_count;
    memcpy(out->disk_guid,     disk_guid,        16);
    memcpy(out->esp_part_guid, arr + 0*ESZ + 16, 16);   /* ESP unique GUID */
    memcpy(out->win_part_guid, arr + 2*ESZ + 16, 16);   /* Windows unique GUID */
    rc = 0;
done:
    free(arr);
    return rc;
}

static const uint8_t empty_partition_type[16];

static int move_macos_recovery(blockio_t *io, uint8_t *entries, size_t table_bytes,
                               uint64_t ss, uint64_t shift) {
    static const uint8_t type_apfs[16] = {
        0xEF,0x57,0x34,0x7C,0x00,0x00,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC };
    static const uint8_t type_recovery[16] = {
        0x72,0x76,0x63,0x52,0x00,0x79,0xAA,0x11,0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC };
    uint8_t *apfs = NULL, *recovery = NULL;
    for (size_t i = 0; i < table_bytes; i += 128) {
        uint8_t *entry = entries + i;
        if (memcmp(entry, type_apfs, 16) == 0) {
            if (apfs) return -1;
            apfs = entry;
        } else if (memcmp(entry, type_recovery, 16) == 0) {
            if (recovery) return -1;
            recovery = entry;
        }
    }
    if (!apfs) return -1;

    uint64_t apfs_start = get_le(apfs + 32, 8), apfs_end = get_le(apfs + 40, 8);
    for (size_t i = 0; i < table_bytes; i += 128) {
        uint8_t *entry = entries + i;
        if (entry == apfs || entry == recovery || memcmp(entry, empty_partition_type, 16) == 0) continue;
        if (get_le(entry + 40, 8) >= apfs_start) return -1;
    }
    if (!recovery) return 0;

    uint64_t recovery_start = get_le(recovery + 32, 8), recovery_end = get_le(recovery + 40, 8);
    shift -= shift % (4096 / ss);
    if (recovery_start <= apfs_end || !shift) return -1;

    const size_t chunk_size = 4 * 1024 * 1024;
    uint8_t *chunk = malloc(chunk_size);
    if (!chunk) return -1;
    for (uint64_t left = (recovery_end - recovery_start + 1) * ss; left > 0;) {
        size_t n = left < chunk_size ? (size_t)left : chunk_size;
        left -= n;
        uint64_t offset = recovery_start * ss + left;
        if (blockio_read(io, offset, chunk, n) != 0 ||
            blockio_pwrite(io, offset + shift * ss, chunk, n) != 0) {
            free(chunk);
            return -1;
        }
    }
    free(chunk);
    put_le64(recovery + 32, recovery_start + shift);
    put_le64(recovery + 40, recovery_end + shift);
    return 0;
}

int gpt_grow_disk(blockio_t *io, uint64_t bytes, int mac_guest) {
    uint64_t old_bytes = blockio_size(io);
    if (bytes <= old_bytes || bytes % 4096 || old_bytes < 4 * 1024 * 1024) return -1;

    uint64_t ss = 512;
    uint8_t header[2][4096];
    if (blockio_read(io, ss, header[0], ss) != 0) return -1;
    if (memcmp(header[0], "EFI PART", 8) != 0) {
        ss = 4096;
        if (blockio_read(io, ss, header[0], ss) != 0) return -1;
    }
    if (old_bytes % ss) return -1;
    uint64_t old_last_lba = old_bytes / ss - 1, last_lba = bytes / ss - 1;
    uint64_t shift = last_lba - old_last_lba;
    if (blockio_read(io, old_last_lba * ss, header[1], ss) != 0) return -1;

    uint8_t mbr[512];
    if (blockio_read(io, 0, mbr, sizeof mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;
    uint8_t *protective = NULL;
    for (int i = 0; i < 4; ++i) {
        uint8_t *partition = mbr + 446 + i * 16;
        if (!partition[4]) continue;
        if (protective || partition[4] != 0xEE || get_le(partition + 8, 4) != 1) return -1;
        protective = partition;
    }
    if (!protective) return -1;

    uint64_t first = get_le(header[0] + 40, 8), last = get_le(header[0] + 48, 8);
    uint64_t tables[2];
    uint8_t entries[2][128 * 128];
    for (int i = 0; i < 2; ++i) {
        uint8_t *h = header[i];
        uint32_t expected = (uint32_t)get_le(h + 16, 4);
        put_le32(h + 16, 0);
        if (memcmp(h, "EFI PART", 8) != 0 || get_le(h + 8, 4) != 0x10000 ||
            get_le(h + 12, 4) != 92 || crc32(h, 92) != expected ||
            get_le(h + 24, 8) != (i ? old_last_lba : 1) ||
            get_le(h + 32, 8) != (i ? 1 : old_last_lba) ||
            get_le(h + 40, 8) != first || get_le(h + 48, 8) != last ||
            memcmp(h + 56, header[0] + 56, 16) != 0 ||
            get_le(h + 80, 4) != 128 || get_le(h + 84, 4) != 128) return -1;
        tables[i] = get_le(h + 72, 8);
        uint64_t sectors = sizeof entries[i] / ss;
        if (first >= last || last >= old_last_lba || tables[i] < 2 ||
            tables[i] > old_last_lba - sectors || (i ? tables[i] <= last : tables[i] + sectors > first) ||
            blockio_read(io, tables[i] * ss, entries[i], sizeof entries[i]) != 0 ||
            crc32(entries[i], sizeof entries[i]) != get_le(h + 88, 4)) return -1;
    }
    if (memcmp(entries[0], entries[1], sizeof entries[0]) != 0) return -1;

    for (size_t i = 0; i < sizeof entries[0]; i += 128) {
        uint8_t *entry = entries[0] + i;
        if (memcmp(entry, empty_partition_type, 16) == 0) continue;
        uint64_t begin = get_le(entry + 32, 8), end = get_le(entry + 40, 8);
        if (begin < first || end < begin || end > last) return -1;
    }

    if (blockio_zero(io, tables[1] * ss, sizeof entries[0]) != 0 ||
        blockio_zero(io, old_last_lba * ss, ss) != 0 ||
        (mac_guest && move_macos_recovery(io, entries[0], sizeof entries[0], ss, shift) != 0))
        return -1;

    tables[1] += shift;
    uint32_t table_crc = crc32(entries[0], sizeof entries[0]);
    for (int i = 1; i >= 0; --i) {
        uint64_t my_lba = i ? last_lba : 1;
        put_le64(header[i] + 24, my_lba);
        put_le64(header[i] + 32, i ? 1 : last_lba);
        put_le64(header[i] + 48, last + shift);
        put_le64(header[i] + 72, tables[i]);
        put_le32(header[i] + 88, table_crc);
        put_le32(header[i] + 16, crc32(header[i], 92));
        if (blockio_write(io, tables[i] * ss, entries[0], sizeof entries[0]) != 0 ||
            blockio_write(io, my_lba * ss, header[i], ss) != 0) return -1;
    }
    put_le32(protective + 12, last_lba > UINT32_MAX ? UINT32_MAX : (uint32_t)last_lba);
    return blockio_write(io, 0, mbr, sizeof mbr);
}
