/* ubuntu_vhdx.c - direct Ubuntu ISO -> bootable VHDX.
 *
 * Pipeline:
 *   1. mount ISO via VirtDisk -> drive letter
 *   2. create + attach fixed VHDX -> physical disk handle
 *   3. write GPT: ESP (512 MiB) + Linux root (rest)
 *   4. open ext4 writer on root partition
 *   5. stream casper/minimal.squashfs -> rootfs (producer + N decompress
 *      workers + writer-consumer)
 *   6. extract grub-efi-<arch>-bin/-signed + shim-signed .debs from ISO
 *      pool/ -> stage into rootfs at /usr/lib/{grub,shim}/...
 *   7. write /etc/fstab, /boot/grub/grub.cfg (bootstrap), first-boot service
 *   8. close ext4 writer
 *   9. find + mount ESP, format FAT32, install signed shim+grub from
 *      .debs, write tiny redirect grub.cfg
 *   10. detach VHDX -> done
 *
 * Self-contained: only uses iso-patch's logging (log_msg/log_err/...) and
 * the vendored engine (ext4 / squashfs / xz). Volume + process helpers
 * are inline here (small, parallel to iso-patch's Windows-side ones).
 */

#include "ubuntu_vhdx.h"
#include "iso_patch_log.h"
#include "engine/ext4.h"
#include "engine/squashfs.h"
#include "engine/log.h"
#include "target_arch.h"

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "ole32.lib")

#define ESP_SIZE_BYTES   (512ULL * 1024ULL * 1024ULL)  /* 512 MiB */

/* VHDX vendor GUID (Microsoft). */
static const GUID VHDX_VENDOR_MS_LOCAL = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

/* GPT partition type GUIDs. */
static const GUID GUID_EFI_SYSTEM = {
    0xc12a7328, 0xf81f, 0x11d2,
    { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }
};
static const GUID GUID_LINUX_FS = {
    0x0fc63daf, 0x8483, 0x4772,
    { 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 }
};

/* ======================================================================
 *  Small helpers (analogues of iso-patch.c's static helpers — duplicated
 *  here to keep ubuntu_vhdx.c self-contained and not destabilise the
 *  Windows path).
 * ====================================================================== */

static DWORD u_get_disk_number_from_path(const wchar_t *phys_path)
{
    size_t len = wcslen(phys_path);
    while (len > 0 && phys_path[len - 1] >= L'0' && phys_path[len - 1] <= L'9')
        len--;
    return (DWORD)_wtoi(phys_path + len);
}

static DWORD u_snapshot_cdrom_drives(void)
{
    DWORD mask = 0;
    wchar_t root[4] = L"A:\\";
    for (int i = 0; i < 26; i++) {
        root[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root) == DRIVE_CDROM)
            mask |= (1u << i);
    }
    return mask;
}

static int u_run_cmd(const wchar_t *cmdline)
{
    wchar_t mut[2048];
    wcsncpy_s(mut, 2048, cmdline, _TRUNCATE);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        log_err(L"CreateProcessW failed: %lu (%s)", GetLastError(), cmdline);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

static BOOL u_create_temp_mount_dir(const wchar_t *prefix,
                                    wchar_t *path, size_t path_len)
{
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) return FALSE;
    swprintf_s(path, path_len, L"%s%s_%u\\",
               temp_dir, prefix, GetCurrentProcessId());
    if (!CreateDirectoryW(path, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return TRUE;
}

/* Find the volume corresponding to a given partition on a given disk.
 * Polls up to ~10 s waiting for the volume manager to publish it. */
static BOOL u_find_partition_volume(DWORD disk_number, uint64_t partition_offset,
                                    wchar_t *out_vol, size_t out_cap)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        wchar_t vol[64];
        HANDLE fh = FindFirstVolumeW(vol, ARRAYSIZE(vol));
        if (fh == INVALID_HANDLE_VALUE) return FALSE;
        do {
            size_t n = wcslen(vol);
            wchar_t open_path[64];
            wcsncpy_s(open_path, ARRAYSIZE(open_path), vol, _TRUNCATE);
            if (n > 0 && open_path[n - 1] == L'\\') open_path[n - 1] = 0;

            HANDLE vh = CreateFileW(open_path, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
            if (vh == INVALID_HANDLE_VALUE) continue;

            BYTE buf[512];
            DWORD br = 0;
            BOOL ok = DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                      NULL, 0, buf, sizeof(buf), &br, NULL);
            CloseHandle(vh);
            if (!ok) continue;

            VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)buf;
            for (DWORD i = 0; i < ext->NumberOfDiskExtents; i++) {
                if (ext->Extents[i].DiskNumber == disk_number &&
                    (uint64_t)ext->Extents[i].StartingOffset.QuadPart == partition_offset) {
                    FindVolumeClose(fh);
                    wcsncpy_s(out_vol, out_cap, vol, _TRUNCATE);
                    return TRUE;
                }
            }
        } while (FindNextVolumeW(fh, vol, ARRAYSIZE(vol)));
        FindVolumeClose(fh);
        Sleep(200);
    }
    return FALSE;
}

static BOOL u_mount_volume_to_dir(const wchar_t *vol, const wchar_t *dir)
{
    return SetVolumeMountPointW(dir, vol);
}

static BOOL u_unmount_volume_from_dir(const wchar_t *dir)
{
    return DeleteVolumeMountPointW(dir);
}

static BOOL u_format_fat32(const wchar_t *mount_dir, const wchar_t *label)
{
    wchar_t cmd[512], clean[MAX_PATH], sys_dir[MAX_PATH];
    GetSystemDirectoryW(sys_dir, MAX_PATH);
    wcscpy_s(clean, MAX_PATH, mount_dir);
    size_t cl = wcslen(clean);
    if (cl > 0 && clean[cl - 1] == L'\\') clean[cl - 1] = 0;
    swprintf_s(cmd, 512, L"%s\\format.com \"%s\" /FS:FAT32 /Q /Y /V:%s",
               sys_dir, clean, label);
    return (u_run_cmd(cmd) == 0);
}

/* ======================================================================
 *  Linux GPT layout writer.
 *  ESP (FAT32) at 1 MiB, default 512 MiB.
 *  Linux root (ext4) after ESP, to (disk_end - 1 MiB).
 * ====================================================================== */

typedef struct {
    uint64_t esp_offset;       /* bytes */
    uint64_t esp_length;       /* bytes */
    uint64_t root_offset;      /* bytes */
    uint64_t root_length;      /* bytes */
} linux_gpt_layout_t;

/* Write a protective-MBR + primary/backup GPT for the Gen2 VHDX.
 * Notable correctness points (each was a boot-breaker if wrong):
 *   - NPART=4 (multiple-of-4 required by Windows; last 2 zeroed)
 *   - RewritePartition=TRUE on each active entry
 *   - Gpt.MaxPartitionCount=128 on CREATE_DISK and layout
 *   - Gpt.StartingUsableOffset + UsableLength on layout
 *   - NO IOCTL_DISK_UPDATE_PROPERTIES after SET_DRIVE_LAYOUT_EX
 *     (caller does it post-ext4 writes; calling here triggers volume-mgr
 *      rescan that LOCKS the disk and silently breaks subsequent
 *      WriteFile to the physical disk handle — that was the root cause
 *      of the "non-bootable VHDX" we were debugging.) */
static BOOL apply_linux_gpt_layout(HANDLE disk_handle,
                                    uint64_t total_bytes,
                                    uint64_t esp_size_bytes,
                                    linux_gpt_layout_t *out_layout)
{
    DWORD bytes;
    const uint64_t SECTOR_BYTES = 512ULL;
    const uint64_t ONE_MIB      = 1024ULL * 1024ULL;
    const uint64_t ALIGN_LBA    = ONE_MIB / SECTOR_BYTES;   /* 2048 */
    const uint64_t GPT_RESERVED = 34;

    if (total_bytes < esp_size_bytes + (16ULL * ONE_MIB)) {
        log_err(L"disk too small: %llu < esp(%llu) + 16MiB",
                (unsigned long long)total_bytes,
                (unsigned long long)esp_size_bytes);
        return FALSE;
    }

    uint64_t total_sectors = total_bytes / SECTOR_BYTES;
    uint64_t esp_sectors   = esp_size_bytes / SECTOR_BYTES;
    uint64_t first_usable  = ALIGN_LBA;
    uint64_t last_usable   = total_sectors - GPT_RESERVED;

    uint64_t esp_start_lba  = first_usable;
    uint64_t esp_end_lba    = esp_start_lba + esp_sectors - 1;
    uint64_t root_start_lba = ((esp_end_lba + 1 + ALIGN_LBA - 1) / ALIGN_LBA) * ALIGN_LBA;
    uint64_t root_end_lba   = last_usable - 1;

    GUID disk_guid, esp_part_guid, root_part_guid;
    if (CoCreateGuid(&disk_guid)      != S_OK ||
        CoCreateGuid(&esp_part_guid)  != S_OK ||
        CoCreateGuid(&root_part_guid) != S_OK) {
        log_err(L"CoCreateGuid failed");
        return FALSE;
    }

    /* ---- Step 1: IOCTL_DISK_CREATE_DISK (initialise as GPT). ---- */
    {
        CREATE_DISK cd;
        ZeroMemory(&cd, sizeof(cd));
        cd.PartitionStyle = PARTITION_STYLE_GPT;
        cd.Gpt.DiskId = disk_guid;
        cd.Gpt.MaxPartitionCount = 128;
        if (!DeviceIoControl(disk_handle, IOCTL_DISK_CREATE_DISK,
                             &cd, sizeof(cd), NULL, 0, &bytes, NULL)) {
            log_err(L"IOCTL_DISK_CREATE_DISK failed: %lu", GetLastError());
            return FALSE;
        }
    }

    /* ---- Step 2: IOCTL_DISK_UPDATE_PROPERTIES (clear cached layout). ---- */
    DeviceIoControl(disk_handle, IOCTL_DISK_UPDATE_PROPERTIES,
                    NULL, 0, NULL, 0, &bytes, NULL);

    /* ---- Step 3: IOCTL_DISK_SET_DRIVE_LAYOUT_EX with 4 partition slots
       (2 real, 2 zeroed — Windows requires PartitionCount be a multiple
       of 4 and reads 4 entries minimum). ---- */
    const DWORD NPART = 4;
    size_t layout_size = sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                         (NPART - 1) * sizeof(PARTITION_INFORMATION_EX);
    DRIVE_LAYOUT_INFORMATION_EX *layout =
        (DRIVE_LAYOUT_INFORMATION_EX *)calloc(1, layout_size);
    if (!layout) return FALSE;

    layout->PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionCount = NPART;
    layout->Gpt.DiskId = disk_guid;
    layout->Gpt.StartingUsableOffset.QuadPart =
        (LONGLONG)(first_usable * SECTOR_BYTES);
    layout->Gpt.UsableLength.QuadPart =
        (LONGLONG)((last_usable - first_usable + 1) * SECTOR_BYTES);
    layout->Gpt.MaxPartitionCount = 128;

    PARTITION_INFORMATION_EX *p;

    /* Partition 0: ESP (FAT32). */
    p = &layout->PartitionEntry[0];
    p->PartitionStyle = PARTITION_STYLE_GPT;
    p->StartingOffset.QuadPart  = (LONGLONG)(esp_start_lba * SECTOR_BYTES);
    p->PartitionLength.QuadPart = (LONGLONG)((esp_end_lba - esp_start_lba + 1) * SECTOR_BYTES);
    p->PartitionNumber = 1;
    p->RewritePartition = TRUE;
    p->Gpt.PartitionType = GUID_EFI_SYSTEM;
    p->Gpt.PartitionId   = esp_part_guid;
    p->Gpt.Attributes    = 0;
    wcsncpy_s(p->Gpt.Name, ARRAYSIZE(p->Gpt.Name),
              L"EFI System Partition", _TRUNCATE);

    /* Partition 1: Linux root (ext4 placeholder). */
    p = &layout->PartitionEntry[1];
    p->PartitionStyle = PARTITION_STYLE_GPT;
    p->StartingOffset.QuadPart  = (LONGLONG)(root_start_lba * SECTOR_BYTES);
    p->PartitionLength.QuadPart = (LONGLONG)((root_end_lba - root_start_lba + 1) * SECTOR_BYTES);
    p->PartitionNumber = 2;
    p->RewritePartition = TRUE;
    p->Gpt.PartitionType = GUID_LINUX_FS;
    p->Gpt.PartitionId   = root_part_guid;
    p->Gpt.Attributes    = 0;
    wcsncpy_s(p->Gpt.Name, ARRAYSIZE(p->Gpt.Name),
              L"Linux root", _TRUNCATE);

    /* Partitions 2 and 3: zeroed (all-zero type GUID = unused entry). */
    layout->PartitionEntry[2].PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionEntry[3].PartitionStyle = PARTITION_STYLE_GPT;

    if (!DeviceIoControl(disk_handle, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                         layout, (DWORD)layout_size,
                         NULL, 0, &bytes, NULL)) {
        log_err(L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed: %lu", GetLastError());
        free(layout);
        return FALSE;
    }
    free(layout);

    /* DELIBERATELY no IOCTL_DISK_UPDATE_PROPERTIES here — deferred to the
       caller after ext4 writes complete. See header comment above. */

    out_layout->esp_offset  = esp_start_lba  * SECTOR_BYTES;
    out_layout->esp_length  = (esp_end_lba  - esp_start_lba  + 1) * SECTOR_BYTES;
    out_layout->root_offset = root_start_lba * SECTOR_BYTES;
    out_layout->root_length = (root_end_lba - root_start_lba + 1) * SECTOR_BYTES;
    return TRUE;
}

/* ======================================================================
 *  .deb extraction (uses Windows-builtin bsdtar at C:\Windows\System32\tar.exe
 *  which handles ar + zstd/xz/gzip via libarchive).
 * ====================================================================== */

static int u_find_first(const wchar_t *dir, const wchar_t *pattern,
                        wchar_t *out_full, size_t cap)
{
    wchar_t spec[MAX_PATH];
    swprintf(spec, MAX_PATH, L"%s\\%s", dir, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    swprintf(out_full, cap, L"%s\\%s", dir, fd.cFileName);
    FindClose(h);
    return 0;
}

/* Extract a single named member from a .deb into out_path. */
static int u_extract_from_deb(const wchar_t *deb_path,
                              const wchar_t *member_path,
                              const wchar_t *out_path)
{
    wchar_t tmp[MAX_PATH], cmd[2048];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return -1;
    /* No trailing backslash. */
    wcscat_s(tmp, MAX_PATH, L"isopatch-deb");
    swprintf(cmd, 2048, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
    u_run_cmd(cmd);
    CreateDirectoryW(tmp, NULL);

    /* ar-extract .deb. */
    swprintf(cmd, 2048, L"C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\"",
             deb_path, tmp);
    if (u_run_cmd(cmd) != 0) return -1;

    /* find data.tar.*, extract one member. */
    wchar_t data_tar[MAX_PATH];
    if (u_find_first(tmp, L"data.tar.*", data_tar, MAX_PATH) != 0) return -1;
    swprintf(cmd, 2048, L"C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\" \"%s\"",
             data_tar, tmp, member_path);
    if (u_run_cmd(cmd) != 0) return -1;

    /* copy extracted file. */
    wchar_t src[MAX_PATH];
    swprintf(src, MAX_PATH, L"%s\\%s", tmp, member_path);
    for (wchar_t *p = src; *p; p++) if (*p == L'/') *p = L'\\';

    /* mkdir -p parent of out_path. */
    wchar_t parent[MAX_PATH];
    wcsncpy_s(parent, MAX_PATH, out_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) {
        *slash = 0;
        wchar_t buf[MAX_PATH];
        wcsncpy_s(buf, MAX_PATH, parent, _TRUNCATE);
        wchar_t *p = buf;
        if (wcslen(buf) >= 3 && buf[1] == L':' && buf[2] == L'\\') p = buf + 3;
        for (; *p; p++) {
            if (*p == L'\\') {
                *p = 0;
                CreateDirectoryW(buf, NULL);
                *p = L'\\';
            }
        }
        CreateDirectoryW(buf, NULL);
    }

    if (!CopyFileW(src, out_path, FALSE)) {
        log_err(L"CopyFileW failed: %lu (%s -> %s)",
                GetLastError(), src, out_path);
        return -1;
    }
    return 0;
}

/* Extract a whole .deb's data.tar into a fresh temp dir; caller walks
 * the result and stages files into the ext4 writer. */
static int u_extract_deb_to_temp(const wchar_t *deb_path,
                                 wchar_t *out_dir, size_t cap)
{
    wchar_t base[MAX_PATH], cmd[2048];
    GetTempPathW(MAX_PATH, base);
    swprintf(out_dir, cap, L"%sisopatch-extract-%lu",
             base, GetTickCount());
    swprintf(cmd, 2048, L"cmd.exe /c rd /s /q \"%s\" 2>nul", out_dir);
    u_run_cmd(cmd);
    CreateDirectoryW(out_dir, NULL);

    swprintf(cmd, 2048, L"C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\"",
             deb_path, out_dir);
    if (u_run_cmd(cmd) != 0) return -1;

    wchar_t data_tar[MAX_PATH];
    if (u_find_first(out_dir, L"data.tar.*", data_tar, MAX_PATH) != 0) return -1;

    swprintf(cmd, 2048, L"C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\"",
             data_tar, out_dir);
    if (u_run_cmd(cmd) != 0) return -1;
    return 0;
}

static int u_find_deb(wchar_t iso_letter, const wchar_t *pool_dir,
                      const wchar_t *deb_prefix,
                      wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH], spec[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%c:\\pool\\main\\%s", iso_letter, pool_dir);
    swprintf(spec, MAX_PATH, L"%s*.deb", deb_prefix);
    return u_find_first(dir, spec, out, cap);
}

/* ======================================================================
 *  Signed grub + shim install onto an ESP mounted at a directory.
 * ====================================================================== */

static int install_signed_efi(wchar_t iso_letter, const wchar_t *esp_dir)
{
    /* esp_dir has trailing backslash. */
    wchar_t grub_dir[MAX_PATH], shim_dir[MAX_PATH];
    swprintf(grub_dir, MAX_PATH, L"%c:\\pool\\main\\g\\grub2-signed", iso_letter);
    swprintf(shim_dir, MAX_PATH, L"%c:\\pool\\main\\s\\shim-signed", iso_letter);

    wchar_t grub_deb[MAX_PATH], shim_deb[MAX_PATH];
    if (u_find_first(grub_dir, L"grub-efi-" IP_DEB_ARCH L"-signed_*.deb",
                     grub_deb, MAX_PATH) != 0) {
        log_err(L"grub-efi-" IP_DEB_ARCH L"-signed deb not found in %s", grub_dir);
        return -1;
    }
    if (u_find_first(shim_dir, L"shim-signed_*.deb",
                     shim_deb, MAX_PATH) != 0) {
        log_err(L"shim-signed deb not found in %s", shim_dir);
        return -1;
    }
    log_msg(L"signed EFI: grub=%s shim=%s", grub_deb, shim_deb);

    wchar_t dst_grub[MAX_PATH], dst_shim[MAX_PATH], dst_mm[MAX_PATH];
    wchar_t dst_boot[MAX_PATH], dst_grub_boot[MAX_PATH], dst_mm_boot[MAX_PATH];
    swprintf(dst_grub,      MAX_PATH, L"%sEFI\\ubuntu\\grub" IP_EFI_SFX L".efi", esp_dir);
    swprintf(dst_shim,      MAX_PATH, L"%sEFI\\ubuntu\\shim" IP_EFI_SFX L".efi", esp_dir);
    swprintf(dst_mm,        MAX_PATH, L"%sEFI\\ubuntu\\mm" IP_EFI_SFX L".efi",   esp_dir);
    swprintf(dst_boot,      MAX_PATH, L"%sEFI\\BOOT\\BOOT" IP_EFI_SFX_UP L".EFI", esp_dir);
    swprintf(dst_grub_boot, MAX_PATH, L"%sEFI\\BOOT\\grub" IP_EFI_SFX L".efi",   esp_dir);
    swprintf(dst_mm_boot,   MAX_PATH, L"%sEFI\\BOOT\\mm" IP_EFI_SFX L".efi",     esp_dir);

    if (u_extract_from_deb(grub_deb,
        L"usr/lib/grub/" IP_GRUB_PLATFORM L"-signed/grub" IP_EFI_SFX L".efi.signed", dst_grub) != 0) return -1;
    if (u_extract_from_deb(shim_deb,
        L"usr/lib/shim/shim" IP_EFI_SFX L".efi.signed.latest", dst_shim) != 0) return -1;
    if (u_extract_from_deb(shim_deb,
        L"usr/lib/shim/mm" IP_EFI_SFX L".efi", dst_mm) != 0) return -1;
    if (u_extract_from_deb(shim_deb,
        L"usr/lib/shim/shim" IP_EFI_SFX L".efi.signed.latest", dst_boot) != 0) return -1;
    if (u_extract_from_deb(grub_deb,
        L"usr/lib/grub/" IP_GRUB_PLATFORM L"-signed/grub" IP_EFI_SFX L".efi.signed", dst_grub_boot) != 0) return -1;
    if (u_extract_from_deb(shim_deb,
        L"usr/lib/shim/mm" IP_EFI_SFX L".efi", dst_mm_boot) != 0) return -1;

    return 0;
}

static int write_redirect_grub_cfg(const wchar_t *esp_dir, const char *root_uuid)
{
    char cfg[1024];
    snprintf(cfg, sizeof(cfg),
        "insmod part_gpt\n"
        "insmod ext2\n"
        "search.fs_uuid %s root\n"
        "set prefix=($root)/boot/grub\n"
        "configfile $prefix/grub.cfg\n",
        root_uuid);

    wchar_t paths[2][MAX_PATH];
    swprintf(paths[0], MAX_PATH, L"%sEFI\\ubuntu\\grub.cfg", esp_dir);
    swprintf(paths[1], MAX_PATH, L"%sEFI\\BOOT\\grub.cfg",   esp_dir);
    for (int i = 0; i < 2; i++) {
        HANDLE h = CreateFileW(paths[i], GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            log_err(L"CreateFileW grub.cfg failed: %lu (%s)",
                    GetLastError(), paths[i]);
            return -1;
        }
        DWORD wr = 0;
        WriteFile(h, cfg, (DWORD)strlen(cfg), &wr, NULL);
        CloseHandle(h);
    }
    return 0;
}

/* ======================================================================
 *  Squashfs -> ext4 ingest pipeline (producer/consumer).
 *
 *  - producer (walker thread, this thread): walks squashfs entries,
 *    dispatches files to N decompress workers, pushes non-files inline
 *  - decompress workers: pop work items, decompress XZ, push slots
 *  - consumer (writer thread, single): drains slots, calls ext4_writer_add_*
 *
 *  Slot queue is capped at 512 MiB outstanding so we don't OOM on large
 *  squashes.  Per-worker xz_dec is in TLS inside engine/squashfs.c.
 * ====================================================================== */

#define INGEST_QUEUE_CAP_BYTES   (512ULL * 1024 * 1024)
#define DECOMPRESS_WORKERS_MAX   32
#define WORK_QUEUE_CAP           64

typedef struct slot {
    char        *path;
    uint16_t     type;
    uint16_t     mode;
    uint32_t     uid, gid, mtime, rdev;
    uint64_t     size;
    void        *data;
    char        *symlink_target;
    uint32_t     symlink_target_size;
    uint64_t     mem_bytes;
    struct slot *next;
} slot_t;

typedef struct work_item {
    sqfs_entry_t      entry_copy;
    char             *path_dup;
    struct work_item *next;
} work_item_t;

typedef struct {
    work_item_t       *head, *tail;
    int                count, stop;
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv_worker, cv_walker;
} work_queue_t;

typedef struct {
    slot_t            *head, *tail;
    uint64_t           outstanding_bytes;
    int                stop;
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv_consumer, cv_producer;

    sqfs_ctx_t        *sq;
    ext4_writer_t     *ew;

    size_t             n_processed, n_files, n_dirs, n_syms, n_special, n_errors;
    uint64_t           bytes_total;

    /* Set by the orchestrator before workers start, used by the consumer
       to emit a real-fraction progress percentage. */
    uint32_t           total_entries;

    char               kernel_version[64];

    work_queue_t       wq;
    HANDLE             workers[DECOMPRESS_WORKERS_MAX];
    int                n_workers;
} pipeline_t;

static int decide_worker_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
    if (n < 2) n = 2;
    if (n > DECOMPRESS_WORKERS_MAX) n = DECOMPRESS_WORKERS_MAX;
    return n;
}

static uint16_t sqfs_type_to_ifmt(uint16_t t)
{
    switch (t) {
    case SQFS_REG_TYPE: case SQFS_EREG_TYPE: return 0x8000;
    case SQFS_DIR_TYPE: case SQFS_EDIR_TYPE: return 0x4000;
    case SQFS_SYM_TYPE: case SQFS_ESYM_TYPE: return 0xA000;
    case SQFS_BLK_TYPE: case SQFS_EBLK_TYPE: return 0x6000;
    case SQFS_CHR_TYPE: case SQFS_ECHR_TYPE: return 0x2000;
    case SQFS_FIFO_TYPE: case SQFS_EFIFO_TYPE: return 0x1000;
    case SQFS_SOCK_TYPE: case SQFS_ESOCK_TYPE: return 0xC000;
    default: return 0;
    }
}

static void pipeline_push(pipeline_t *p, slot_t *s)
{
    EnterCriticalSection(&p->cs);
    while (p->outstanding_bytes > 0 &&
           p->outstanding_bytes + s->mem_bytes > INGEST_QUEUE_CAP_BYTES &&
           !p->stop) {
        SleepConditionVariableCS(&p->cv_producer, &p->cs, INFINITE);
    }
    s->next = NULL;
    if (p->tail) p->tail->next = s; else p->head = s;
    p->tail = s;
    p->outstanding_bytes += s->mem_bytes;
    WakeConditionVariable(&p->cv_consumer);
    LeaveCriticalSection(&p->cs);
}

static slot_t *pipeline_pop(pipeline_t *p)
{
    EnterCriticalSection(&p->cs);
    while (!p->head && !p->stop)
        SleepConditionVariableCS(&p->cv_consumer, &p->cs, INFINITE);
    slot_t *s = p->head;
    if (s) {
        p->head = s->next;
        if (!p->head) p->tail = NULL;
        p->outstanding_bytes -= s->mem_bytes;
        WakeConditionVariable(&p->cv_producer);
    }
    LeaveCriticalSection(&p->cs);
    return s;
}

static void work_queue_push(work_queue_t *q, work_item_t *w)
{
    EnterCriticalSection(&q->cs);
    while (q->count >= WORK_QUEUE_CAP && !q->stop)
        SleepConditionVariableCS(&q->cv_walker, &q->cs, INFINITE);
    w->next = NULL;
    if (q->tail) q->tail->next = w; else q->head = w;
    q->tail = w;
    q->count++;
    WakeConditionVariable(&q->cv_worker);
    LeaveCriticalSection(&q->cs);
}

static work_item_t *work_queue_pop(work_queue_t *q)
{
    EnterCriticalSection(&q->cs);
    while (!q->head && !q->stop)
        SleepConditionVariableCS(&q->cv_worker, &q->cs, INFINITE);
    work_item_t *w = q->head;
    if (w) {
        q->head = w->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        WakeConditionVariable(&q->cv_walker);
    }
    LeaveCriticalSection(&q->cs);
    return w;
}

static DWORD WINAPI decompress_worker(LPVOID arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    for (;;) {
        work_item_t *w = work_queue_pop(&p->wq);
        if (!w) break;

        slot_t *s = (slot_t *)calloc(1, sizeof(*s));
        if (!s) {
            InterlockedIncrement((volatile LONG *)&p->n_errors);
            free(w->path_dup); free(w);
            continue;
        }
        s->path  = w->path_dup;
        s->type  = w->entry_copy.type;
        s->mode  = w->entry_copy.mode;
        s->uid   = w->entry_copy.uid;
        s->gid   = w->entry_copy.gid;
        s->mtime = w->entry_copy.mtime;
        s->rdev  = w->entry_copy.rdev;
        s->mem_bytes = 512;

        size_t sz = 0;
        sqfs_entry_t e = w->entry_copy;
        e.path = s->path;
        int rrc = sqfs_read_file(p->sq, &e, &s->data, &sz);
        if (rrc != 0) {
            log_err(L"sqfs_read_file failed");
            free(s->path); free(s);
            InterlockedIncrement((volatile LONG *)&p->n_errors);
            free(w);
            continue;
        }
        s->size = sz;
        s->mem_bytes += sz;
        pipeline_push(p, s);
        free(w);
    }
    return 0;
}

static int producer_cb(const sqfs_entry_t *e, void *user)
{
    pipeline_t *p = (pipeline_t *)user;

    if (e->path[0] == 0) return 0;

    if (p->kernel_version[0] == 0) {
        const char *m = strstr(e->path, "/boot/vmlinuz-");
        if (m) {
            const char *v = m + strlen("/boot/vmlinuz-");
            size_t n = 0;
            while (v[n] && v[n] != '/' && n < sizeof(p->kernel_version) - 1) n++;
            memcpy(p->kernel_version, v, n);
            p->kernel_version[n] = 0;
            log_msg(L"detected kernel from squashfs: %hs (path=%hs)",
                    p->kernel_version, e->path);
        }
    }

    if (e->type == SQFS_REG_TYPE || e->type == SQFS_EREG_TYPE) {
        work_item_t *w = (work_item_t *)calloc(1, sizeof(*w));
        if (!w) { p->n_errors++; return 0; }
        w->entry_copy = *e;
        w->path_dup   = _strdup(e->path);
        if (!w->path_dup) { free(w); p->n_errors++; return 0; }
        work_queue_push(&p->wq, w);
        return 0;
    }

    slot_t *s = (slot_t *)calloc(1, sizeof(*s));
    if (!s) { p->n_errors++; return 0; }
    s->path  = _strdup(e->path);
    s->type  = e->type;
    s->mode  = e->mode;
    s->uid   = e->uid;
    s->gid   = e->gid;
    s->mtime = e->mtime;
    s->rdev  = e->rdev;
    s->size  = e->size;
    s->mem_bytes = 512;

    if (e->type == SQFS_SYM_TYPE || e->type == SQFS_ESYM_TYPE) {
        s->symlink_target = (char *)malloc(e->symlink_target_size);
        if (!s->symlink_target) { free(s->path); free(s); p->n_errors++; return 0; }
        memcpy(s->symlink_target, e->symlink_target, e->symlink_target_size);
        s->symlink_target_size = e->symlink_target_size;
        s->mem_bytes += e->symlink_target_size;
    }

    pipeline_push(p, s);
    return 0;
}

static DWORD WINAPI consumer_thread(LPVOID arg)
{
    pipeline_t *p = (pipeline_t *)arg;
    DWORD last_progress = GetTickCount();
    for (;;) {
        slot_t *s = pipeline_pop(p);
        if (!s) break;

        int rc = 0;
        switch (s->type) {
        case SQFS_DIR_TYPE: case SQFS_EDIR_TYPE:
            rc = ext4_writer_add_dir(p->ew, s->path, s->mode, s->uid, s->gid, s->mtime);
            if (rc == 0) p->n_dirs++;
            break;
        case SQFS_REG_TYPE: case SQFS_EREG_TYPE:
            rc = ext4_writer_add_file(p->ew, s->path, s->mode, s->uid, s->gid, s->mtime,
                                      s->data, s->size);
            if (rc == 0) { p->n_files++; p->bytes_total += s->size; }
            /* Mirror unicode.pf2 into /boot/grub/fonts/ (loadfont's default
             * search path). */
            if (rc == 0 && strcmp(s->path, "/boot/grub/unicode.pf2") == 0) {
                ext4_writer_add_dir(p->ew, "/boot/grub/fonts", 0755, 0, 0, s->mtime);
                ext4_writer_add_file(p->ew, "/boot/grub/fonts/unicode.pf2",
                                     s->mode, s->uid, s->gid, s->mtime,
                                     s->data, s->size);
            }
            break;
        case SQFS_SYM_TYPE: case SQFS_ESYM_TYPE:
            rc = ext4_writer_add_symlink(p->ew, s->path,
                                         s->symlink_target, s->symlink_target_size,
                                         s->uid, s->gid, s->mtime);
            if (rc == 0) p->n_syms++;
            break;
        case SQFS_BLK_TYPE: case SQFS_EBLK_TYPE:
        case SQFS_CHR_TYPE: case SQFS_ECHR_TYPE:
        case SQFS_FIFO_TYPE: case SQFS_EFIFO_TYPE:
        case SQFS_SOCK_TYPE: case SQFS_ESOCK_TYPE: {
            uint16_t mode_with_type = sqfs_type_to_ifmt(s->type) | (s->mode & 0xfff);
            rc = ext4_writer_add_special(p->ew, s->path, mode_with_type,
                                         s->uid, s->gid, s->mtime, s->rdev);
            if (rc == 0) p->n_special++;
            break;
        }
        }
        if (rc != 0) p->n_errors++;
        p->n_processed++;

        DWORD now = GetTickCount();
        if (now - last_progress > 500) {
            /* Map ingest fraction (0..1 of total squashfs entries) into the
               10..75 progress range. The remaining 75..99 is reserved for
               the post-ingest phases (grub modules install, manifest
               staging, ESP setup) which together take ~10-15 s vs the
               ~2-minute ingest. */
            int pct = 10;
            if (p->total_entries > 0) {
                uint64_t frac = ((uint64_t)p->n_processed * 65ULL) / p->total_entries;
                pct = 10 + (int)frac;
                if (pct > 75) pct = 75;
            }
            log_progress(pct, L"Building rootfs");
            last_progress = now;
        }

        free(s->path);
        free(s->data);
        free(s->symlink_target);
        free(s);
    }
    return 0;
}

/* ======================================================================
 *  Stage grub modules + shim binaries from .debs into rootfs.
 *  Recreates the path layout that dpkg would install — so grub-install
 *  + update-grub run cleanly on first boot.
 * ====================================================================== */

struct stage_spec {
    const wchar_t *pool_dir;
    const wchar_t *deb_prefix;
    const wchar_t *src_subpath;
    const char    *dst_path;
};

static void ext4_mkdir_p(ext4_writer_t *ew, const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            ext4_writer_add_dir(ew, tmp, 0755, 0, 0, (uint32_t)time(NULL));
            *p = '/';
        }
    }
    ext4_writer_add_dir(ew, tmp, 0755, 0, 0, (uint32_t)time(NULL));
}

static int stage_grub_modules(wchar_t iso_letter, ext4_writer_t *ew)
{
    static const struct stage_spec specs[] = {
        { L"g\\grub2-unsigned", L"grub-efi-" IP_DEB_ARCH L"-bin_",
          L"usr\\lib\\grub\\" IP_GRUB_PLATFORM,
          "/usr/lib/grub/" IP_GRUB_PLATFORM_A },
        { L"g\\grub2-signed",   L"grub-efi-" IP_DEB_ARCH L"-signed_",
          L"usr\\lib\\grub\\" IP_GRUB_PLATFORM L"-signed",
          "/usr/lib/grub/" IP_GRUB_PLATFORM_A "-signed" },
        { L"s\\shim-signed",    L"shim-signed_",
          L"usr\\lib\\shim",
          "/usr/lib/shim" },
    };
    int total = 0;
    for (size_t si = 0; si < sizeof(specs) / sizeof(specs[0]); si++) {
        wchar_t deb[MAX_PATH], extract_dir[MAX_PATH], src_dir[MAX_PATH];
        if (u_find_deb(iso_letter, specs[si].pool_dir,
                       specs[si].deb_prefix, deb, MAX_PATH) != 0) {
            log_msg(L"warning: deb %s not found", specs[si].deb_prefix);
            continue;
        }
        if (u_extract_deb_to_temp(deb, extract_dir, MAX_PATH) != 0) {
            log_msg(L"warning: extract failed: %s", deb);
            continue;
        }
        swprintf(src_dir, MAX_PATH, L"%s\\%s",
                 extract_dir, specs[si].src_subpath);

        ext4_mkdir_p(ew, specs[si].dst_path);

        wchar_t glob[MAX_PATH];
        swprintf(glob, MAX_PATH, L"%s\\*", src_dir);
        WIN32_FIND_DATAW fd;
        HANDLE fh = FindFirstFileW(glob, &fd);
        if (fh == INVALID_HANDLE_VALUE) continue;
        int n = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t full[MAX_PATH];
            swprintf(full, MAX_PATH, L"%s\\%s", src_dir, fd.cFileName);
            HANDLE mh = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING, 0, NULL);
            if (mh == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz;
            GetFileSizeEx(mh, &sz);
            void *buf = malloc((size_t)sz.QuadPart);
            DWORD br = 0;
            if (buf && ReadFile(mh, buf, (DWORD)sz.QuadPart, &br, NULL) &&
                br == sz.QuadPart) {
                char dst[512], fn_a[260];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                    fn_a, sizeof(fn_a), NULL, NULL);
                snprintf(dst, sizeof(dst), "%s/%s",
                         specs[si].dst_path, fn_a);
                if (ext4_writer_add_file(ew, dst, 0644, 0, 0,
                        (uint32_t)time(NULL), buf,
                        (uint64_t)sz.QuadPart) == 0) n++;
            }
            free(buf);
            CloseHandle(mh);
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
        log_msg(L"staged %d files from %s", n, specs[si].deb_prefix);
        total += n;
    }
    return total;
}

/* ======================================================================
 *  Stage <iso>:\pool\main\ + <iso>:\dists\ -> rootfs at
 *  /opt/appsandbox/local-apt/. ~296 MiB and ~3000 files.
 *
 *  At first boot we write /etc/apt/sources.list.d/appsandbox-local.list
 *  pointing at file:/opt/appsandbox/local-apt resolute main, then run
 *  apt update + apt install — no network required for dkms +
 *  build-essential + linux-headers-$KVER. installed from the staged
 *  local apt mirror.
 * ====================================================================== */

/* Recursively copy a host directory tree into the ext4 writer under a
 * rootfs prefix. Used by stage_local_apt to dump pool/main + dists/.
 *
 *   host_dir:    Windows absolute path of the source dir (no trailing \)
 *   rootfs_pref: Linux absolute path prefix (no trailing /), e.g.
 *                "/opt/appsandbox/local-apt/pool"
 *
 * Creates intermediate dirs via ext4_writer_add_dir. Files staged with
 * mode 0644, uid/gid 0. Returns total file count, or -1 on a fatal
 * error opening the top-level dir. */
static int u_copy_tree_to_ext4(const wchar_t *host_dir,
                               const char    *rootfs_pref,
                               ext4_writer_t *ew,
                               int           *out_dir_count)
{
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%s\\*", host_dir);
    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW(pattern, &fd);
    if (fh == INVALID_HANDLE_VALUE) return -1;

    int file_count = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;

        wchar_t child_host[MAX_PATH];
        swprintf(child_host, MAX_PATH, L"%s\\%s", host_dir, fd.cFileName);

        char child_name_utf8[260];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            child_name_utf8, sizeof(child_name_utf8), NULL, NULL);
        char child_rootfs[1024];
        snprintf(child_rootfs, sizeof(child_rootfs), "%s/%s",
                 rootfs_pref, child_name_utf8);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ext4_writer_add_dir(ew, child_rootfs, 0755, 0, 0,
                                (uint32_t)time(NULL));
            if (out_dir_count) (*out_dir_count)++;
            int sub = u_copy_tree_to_ext4(child_host, child_rootfs, ew, out_dir_count);
            if (sub > 0) file_count += sub;
        } else {
            HANDLE mh = CreateFileW(child_host, GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (mh == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz;
            GetFileSizeEx(mh, &sz);
            void *buf = malloc((size_t)sz.QuadPart);
            DWORD br = 0;
            if (buf && ReadFile(mh, buf, (DWORD)sz.QuadPart, &br, NULL) &&
                br == sz.QuadPart) {
                if (ext4_writer_add_file(ew, child_rootfs, 0644, 0, 0,
                        (uint32_t)time(NULL), buf,
                        (uint64_t)sz.QuadPart) == 0) file_count++;
            }
            free(buf);
            CloseHandle(mh);
        }
    } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    return file_count;
}

static int stage_local_apt(wchar_t iso_letter, ext4_writer_t *ew)
{
    /* Create the local-apt root + the two top-level subdirs we copy. */
    ext4_mkdir_p(ew, "/opt/appsandbox/local-apt");
    ext4_mkdir_p(ew, "/opt/appsandbox/local-apt/pool");
    ext4_mkdir_p(ew, "/opt/appsandbox/local-apt/dists");

    int total_files = 0, total_dirs = 0;

    /* pool/main — the .deb blobs. ~3000 files, ~296 MiB on 26.04 daily. */
    {
        wchar_t src[MAX_PATH];
        swprintf(src, MAX_PATH, L"%c:\\pool\\main", iso_letter);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
            ext4_mkdir_p(ew, "/opt/appsandbox/local-apt/pool/main");
            int n = u_copy_tree_to_ext4(src, "/opt/appsandbox/local-apt/pool/main",
                                        ew, &total_dirs);
            if (n > 0) {
                log_msg(L"local-apt: staged %d .deb file(s) from pool/main", n);
                total_files += n;
            }
        } else {
            log_msg(L"WARN: %s not present on ISO; firstboot DKMS will need internet", src);
        }
    }

    /* dists/<release>/ — Release, Release.gpg, main/binary-<arch>/Packages{,.gz} */
    {
        wchar_t src[MAX_PATH];
        swprintf(src, MAX_PATH, L"%c:\\dists", iso_letter);
        if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
            int n = u_copy_tree_to_ext4(src, "/opt/appsandbox/local-apt/dists",
                                        ew, &total_dirs);
            if (n > 0) {
                log_msg(L"local-apt: staged %d dists/ metadata file(s)", n);
                total_files += n;
            }
        } else {
            log_msg(L"WARN: %s not present on ISO; apt update will fail", src);
        }
    }

    log_msg(L"local-apt: %d files / %d dirs total", total_files, total_dirs);
    return total_files;
}

/* ======================================================================
 *  First-boot service planting (runs grub-install + per-VM setup on
 *  first systemd boot, then disables itself).
 *
 *  We pre-enable via /etc/systemd/system/multi-user.target.wants/ symlink
 *  — same thing `systemctl enable` writes at runtime.
 * ====================================================================== */

static void plant_firstboot_service(ext4_writer_t *ew)
{
    /* First-boot provisioning script:
     *   - NO `set -e` — we want every step attempted even if earlier ones
     *     failed (e.g. grub-install missing from minimal.squashfs)
     *   - `set -x` so every command is echoed to stderr → systemd journal
     *     → host's com1 pipe (via StandardError=journal+console in the unit)
     *   - Explicit STEP markers between sections so we can see in the
     *     console exactly which steps ran and which failed
     *   - No global exec-to-log redirect: rely on systemd journal capture
     *     so output reaches both the journal AND the named-pipe console */
    const char *firstboot_sh =
        "#!/bin/bash\n"
        "# NOTE: deliberately no `set -e` — see plant_firstboot_service.\n"
        "# pipefail IS critical: without it, `cmd 2>&1 | tail -N` returns\n"
        "# tail's exit status (always 0), and we silently miss build/install\n"
        "# failures — that's how an earlier run reported OK while the agent\n"
        "# ELFs were never compiled.\n"
        "set -x\n"
        "set -o pipefail\n"
        "echo \"\"\n"
        "echo \"================================================================\"\n"
        "echo \"==== appsandbox-firstboot starting $(date -u 2>/dev/null || echo ?) ====\"\n"
        "echo \"================================================================\"\n"
        "\n"
        "# diagnostic: what's actually in the rootfs?\n"
        "echo \"---- rootfs sanity check ----\"\n"
        "ls -la /usr/sbin/grub-install /usr/sbin/update-grub /usr/bin/hostnamectl /usr/sbin/useradd 2>&1 || true\n"
        "ls /etc/xdg/autostart/gnome-initial-setup* 2>&1 || true\n"
        "ls /usr/lib/grub/" IP_GRUB_PLATFORM_A "/ 2>&1 | head -5 || true\n"
        "ls /usr/lib/shim/ 2>&1 || true\n"
        "mountpoint /boot/efi 2>&1 || true\n"
        "echo \"---- end sanity check ----\"\n"
        "echo\n"
        "\n"
        "# --- STEP 1: grub-install + update-grub ---\n"
        "echo \"==== STEP 1: grub-install + update-grub ====\"\n"
        "if command -v grub-install >/dev/null 2>&1; then\n"
        "    if mountpoint -q /boot/efi; then\n"
        "        grub-install --target=" IP_GRUB_PLATFORM_A " --efi-directory=/boot/efi \\\n"
        "                     --bootloader-id=ubuntu --recheck --no-nvram \\\n"
        "          && echo \"OK: grub-install\" || echo \"FAIL: grub-install rc=$?\"\n"
        "        update-grub && echo \"OK: update-grub\" || echo \"FAIL: update-grub rc=$?\"\n"
        "    else\n"
        "        echo \"SKIP STEP 1: /boot/efi not mounted (fstab issue?)\"\n"
        "    fi\n"
        "else\n"
        "    echo \"SKIP STEP 1: grub-install binary not found in rootfs\"\n"
        "fi\n"
        "\n"
        "# --- STEP 2: Hostname (from host marker = AppSandbox VM name) ---\n"
        "# Host staged /etc/appsandbox-hostname with the VM name (already\n"
        "# validated to a legal lowercase hostname by the create UI). Falls\n"
        "# back to 'ubuntu' if the marker is absent.\n"
        "echo \"==== STEP 2: hostname ====\"\n"
        "HN=ubuntu\n"
        "if [ -s /etc/appsandbox-hostname ]; then HN=$(tr -d '\\r\\n[:space:]' < /etc/appsandbox-hostname); fi\n"
        "[ -n \"$HN\" ] || HN=ubuntu\n"
        "echo \"$HN\" > /etc/hostname && echo \"OK: /etc/hostname=$HN\" || echo \"FAIL\"\n"
        "command -v hostnamectl >/dev/null 2>&1 && hostnamectl set-hostname \"$HN\" || echo \"SKIP hostnamectl\"\n"
        "if [ -f /etc/hosts ]; then\n"
        "    sed -i \"s/^127\\.0\\.1\\.1.*/127.0.1.1\\t$HN/\" /etc/hosts || true\n"
        "    grep -q '^127.0.1.1' /etc/hosts || printf '127.0.1.1\\t%s\\n' \"$HN\" >> /etc/hosts\n"
        "fi\n"
        "rm -f /etc/appsandbox-hostname 2>/dev/null || true\n"
        "\n"
        "# --- STEP 3: Timezone (from host marker, fallback Etc/UTC) ---\n"
        "# Host staged /etc/appsandbox-timezone with the IANA zone derived\n"
        "# from the current Windows user's timezone. All zones ship in\n"
        "# tzdata so any value resolves offline.\n"
        "echo \"==== STEP 3: timezone ====\"\n"
        "TZ=Etc/UTC\n"
        "if [ -s /etc/appsandbox-timezone ]; then TZ=$(tr -d '\\r\\n[:space:]' < /etc/appsandbox-timezone); fi\n"
        "if [ ! -e \"/usr/share/zoneinfo/$TZ\" ]; then echo \"WARN: zoneinfo $TZ missing, using Etc/UTC\"; TZ=Etc/UTC; fi\n"
        "ln -sf \"/usr/share/zoneinfo/$TZ\" /etc/localtime && echo \"OK: localtime=$TZ\" || echo \"FAIL: localtime\"\n"
        "echo \"$TZ\" > /etc/timezone || true\n"
        "rm -f /etc/appsandbox-timezone 2>/dev/null || true\n"
        "\n"
        "# --- STEP 4: Locale + keyboard (from host markers, gated fallbacks) ---\n"
        "# Host staged /etc/appsandbox-locale (already gated to the 8\n"
        "# ISO-preinstalled languages, else en_US.UTF-8) and\n"
        "# /etc/appsandbox-keyboard (XKB layout). If locale-gen rejects the\n"
        "# locale we fall back to en_US.UTF-8 so the desktop is never broken.\n"
        "echo \"==== STEP 4: locale + keyboard ====\"\n"
        "LOC=en_US.UTF-8\n"
        "if [ -s /etc/appsandbox-locale ]; then LOC=$(tr -d '\\r\\n[:space:]' < /etc/appsandbox-locale); fi\n"
        "echo \"LANG=$LOC\" > /etc/default/locale || true\n"
        "if command -v locale-gen >/dev/null 2>&1; then\n"
        "    if ! locale-gen \"$LOC\" >/dev/null 2>&1; then\n"
        "        echo \"WARN: locale-gen $LOC failed, falling back to en_US.UTF-8\"\n"
        "        LOC=en_US.UTF-8; echo \"LANG=$LOC\" > /etc/default/locale\n"
        "        locale-gen en_US.UTF-8 >/dev/null 2>&1 || true\n"
        "    fi\n"
        "    echo \"OK: locale=$LOC\"\n"
        "else\n"
        "    echo \"SKIP locale-gen\"\n"
        "fi\n"
        "KB=us\n"
        "if [ -s /etc/appsandbox-keyboard ]; then KB=$(tr -d '\\r\\n[:space:]' < /etc/appsandbox-keyboard); fi\n"
        "cat > /etc/default/keyboard <<EOF\n"
        "XKBMODEL=\"pc105\"\n"
        "XKBLAYOUT=\"$KB\"\n"
        "XKBVARIANT=\"\"\n"
        "XKBOPTIONS=\"\"\n"
        "BACKSPACE=\"guess\"\n"
        "EOF\n"
        "echo \"OK: keyboard=$KB\"\n"
        "rm -f /etc/appsandbox-locale /etc/appsandbox-keyboard 2>/dev/null || true\n"
        "\n"
        "# --- STEP 5: User account (from create-VM modal, falls back to test) ---\n"
        "# Host writes the chosen username + glibc $6$ hash into these\n"
        "# marker files during staging. If they're not present (e.g. the\n"
        "# modal had no password), we fall back to test/test123 so the VM\n"
        "# is still usable for debugging.\n"
        "echo \"==== STEP 5: user account ====\"\n"
        "ASB_USER=\"\"\n"
        "ASB_HASH=\"\"\n"
        "if [ -s /etc/appsandbox-admin-user ]; then\n"
        "    ASB_USER=$(tr -d '\\r\\n[:space:]' < /etc/appsandbox-admin-user)\n"
        "fi\n"
        "if [ -s /etc/appsandbox-admin-hash ]; then\n"
        "    ASB_HASH=$(tr -d '\\r\\n' < /etc/appsandbox-admin-hash)\n"
        "fi\n"
        "if [ -z \"$ASB_USER\" ]; then\n"
        "    ASB_USER=test\n"
        "    ASB_HASH=\"\"   # falls through to chpasswd path below\n"
        "    echo \"STEP 5: no admin marker — falling back to test/test123\"\n"
        "fi\n"
        "if ! id \"$ASB_USER\" >/dev/null 2>&1; then\n"
        "    if command -v useradd >/dev/null 2>&1; then\n"
        "        useradd -m -s /bin/bash -c 'AppSandbox User' -G sudo,plugdev,lpadmin \"$ASB_USER\" \\\n"
        "          && echo \"OK: useradd $ASB_USER\" || echo \"FAIL: useradd rc=$?\"\n"
        "        if [ -n \"$ASB_HASH\" ]; then\n"
        "            usermod -p \"$ASB_HASH\" \"$ASB_USER\" \\\n"
        "              && echo \"OK: set password hash for $ASB_USER\" \\\n"
        "              || echo \"FAIL: usermod -p rc=$?\"\n"
        "        else\n"
        "            echo \"$ASB_USER:test123\" | chpasswd \\\n"
        "              && echo \"OK: chpasswd $ASB_USER\" || echo \"FAIL: chpasswd rc=$?\"\n"
        "        fi\n"
        "    else\n"
        "        echo \"SKIP STEP 5: useradd not in rootfs\"\n"
        "    fi\n"
        "else\n"
        "    echo \"user '$ASB_USER' already exists\"\n"
        "fi\n"
        "# Wipe the hash file — it's been consumed by usermod.\n"
        "rm -f /etc/appsandbox-admin-hash 2>/dev/null || true\n"
        "\n"
        "# --- STEP 6: Skip GNOME welcome wizard ---\n"
        "echo \"==== STEP 6: skip GNOME OOBE ====\"\n"
        "if id \"$ASB_USER\" >/dev/null 2>&1; then\n"
        "    HOME_DIR=$(getent passwd \"$ASB_USER\" | cut -d: -f6)\n"
        "    install -d -o \"$ASB_USER\" -g \"$ASB_USER\" \"$HOME_DIR/.config\" 2>/dev/null || true\n"
        "    su \"$ASB_USER\" -c \"touch '$HOME_DIR/.config/gnome-initial-setup-done'\" 2>/dev/null || true\n"
        "fi\n"
        "rm -f /etc/xdg/autostart/gnome-initial-setup-first-login.desktop || true\n"
        "\n"
        "# --- STEP 7: Auto-login via gdm3 ---\n"
        "echo \"==== STEP 7: gdm autologin ====\"\n"
        "mkdir -p /etc/gdm3 || true\n"
        "cat > /etc/gdm3/custom.conf <<EOF\n"
        "[daemon]\n"
        "AutomaticLoginEnable=true\n"
        "AutomaticLogin=$ASB_USER\n"
        "WaylandEnable=true\n"
        "\n"
        "[security]\n"
        "\n"
        "[xdmcp]\n"
        "\n"
        "[chooser]\n"
        "\n"
        "[debug]\n"
        "EOF\n"
        "\n"
        "# ============================================================\n"
        "# AppSandbox guest extras install\n"
        "# ============================================================\n"
        "EXTRAS=/opt/appsandbox\n"
        "\n"
        "# --- STEP 7.4: isolated apt sources + offline update + install all build tools ---\n"
        "#\n"
        "# Use a DEDICATED sources-parts dir so apt-get update + install only\n"
        "# see our two local file:// sources. The system's ubuntu.sources is\n"
        "# left alone (subsequent post-firstboot apt commands work normally).\n"
        "#\n"
        "# Then install EVERY apt package we'll need across STEP 8 + STEP 12\n"
        "# in one shot — saves repeated apt overhead and avoids the\n"
        "# install-build-deps-after-trying-to-build ordering bug.\n"
        "echo \"==== STEP 7.4: apt sources + install all build tools ====\"\n"
        "APT_SOURCES_DIR=/etc/apt/appsandbox-sources.list.d\n"
        "install -d \"$APT_SOURCES_DIR\"\n"
        "if [ -d /opt/appsandbox/local-apt/dists ]; then\n"
        "    REL=$(ls /opt/appsandbox/local-apt/dists 2>/dev/null | head -1)\n"
        "    if [ -n \"$REL\" ]; then\n"
        "        echo \"deb [trusted=yes] file:/opt/appsandbox/local-apt $REL main\" \\\n"
        "            > \"$APT_SOURCES_DIR/appsandbox-local.list\" \\\n"
        "          && echo \"OK: appsandbox-local.list ($REL)\" || echo \"FAIL\"\n"
        "    else\n"
        "        echo \"WARN: /opt/appsandbox/local-apt/dists empty\"\n"
        "    fi\n"
        "else\n"
        "    echo \"WARN: no /opt/appsandbox/local-apt\"\n"
        "fi\n"
        "if [ -f /opt/appsandbox/local-apt-extras/Packages ]; then\n"
        "    echo \"deb [trusted=yes] file:/opt/appsandbox/local-apt-extras ./\" \\\n"
        "        > \"$APT_SOURCES_DIR/appsandbox-local-extras.list\" \\\n"
        "      && echo \"OK: appsandbox-local-extras.list\" || echo \"FAIL\"\n"
        "else\n"
        "    echo \"WARN: no local-apt-extras (host prefetch failed)\"\n"
        "fi\n"
        "\n"
        "# All apt operations in firstboot use this APT_OPTS to force\n"
        "# apt to read ONLY our local sources (no network).\n"
        "APT_OPTS=\"-o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts=$APT_SOURCES_DIR\"\n"
        "\n"
        "# Offline apt update.\n"
        "if apt-get update $APT_OPTS 2>&1 | tail -10; then\n"
        "    echo \"OK: apt update (local sources only)\"\n"
        "else\n"
        "    echo \"FAIL: apt update — agent + DKMS builds will fail\"\n"
        "fi\n"
        "\n"
        "TGT_KVER=$(uname -r)\n"
        "echo \"running kernel: $TGT_KVER\"\n"
        "\n"
        "# Install EVERY build dep in one shot, before any compile step:\n"
        "#   build-essential    -> gcc, g++, make, libc6-dev, dpkg-dev\n"
        "#   dkms               -> module build framework\n"
        "#   linux-headers-$KVER -> kernel module headers\n"
        "#   libasound2-dev     -> appsandbox-audio\n"
        "#   libxcb1-dev        -> appsandbox-clipboard\n"
        "#   libxcb-xfixes0-dev -> appsandbox-clipboard\n"
        "#   libdrm-dev         -> appsandbox-display\n"
        "#   pkg-config         -> Makefile's pkg-config invocation\n"
        "if DEBIAN_FRONTEND=noninteractive apt-get install -y $APT_OPTS \\\n"
        "       build-essential dkms \"linux-headers-$TGT_KVER\" \\\n"
        "       libasound2-dev libxcb1-dev libxcb-xfixes0-dev libdrm-dev \\\n"
        "       pkg-config 2>&1 | tail -20; then\n"
        "    echo \"OK: apt install all build tools\"\n"
        "else\n"
        "    rc=$?\n"
        "    echo \"FAIL: apt install all build tools (rc=$rc)\"\n"
        "    # Dump apt's view of available sources so we can diagnose offline.\n"
        "    apt-cache policy $APT_OPTS 2>&1 | head -30\n"
        "fi\n"
        "\n"
        "# --- STEP 7.5: optional openssh-server (gated on host marker) ---\n"
        "# Host drops /etc/appsandbox-ssh-enabled in the manifest when the\n"
        "# user requested SSH. openssh-server came in through prefetch-build-deps.\n"
        "if [ -f /etc/appsandbox-ssh-enabled ]; then\n"
        "    echo \"==== STEP 7.5: openssh-server install + enable ====\"\n"
        "    if DEBIAN_FRONTEND=noninteractive apt-get install -y $APT_OPTS openssh-server 2>&1 | tail -10; then\n"
        "        echo \"OK: openssh-server installed\"\n"
        "        for unit in ssh.service ssh.socket; do\n"
        "            systemctl unmask \"$unit\" 2>/dev/null || true\n"
        "            systemctl enable --now \"$unit\" 2>/dev/null && \\\n"
        "                echo \"OK: enabled $unit\" || echo \"WARN: enable $unit\"\n"
        "        done\n"
        "        if command -v ufw >/dev/null 2>&1; then\n"
        "            ufw allow OpenSSH 2>/dev/null || ufw allow 22/tcp 2>/dev/null || true\n"
        "        fi\n"
        "    else\n"
        "        echo \"FAIL: openssh-server install\"\n"
        "    fi\n"
        "fi\n"
        "\n"
        "# --- STEP 8: build agent binaries from source ---\n"
        "# All build tools were installed in STEP 7.4. This step just runs\n"
        "# make + make install against $EXTRAS/agent-src.\n"
        "echo \"==== STEP 8: build agent binaries from source ====\"\n"
        "BUILD_T0=$SECONDS\n"
        "AGENT_SRC=\"$EXTRAS/agent-src\"\n"
        "if [ ! -d \"$AGENT_SRC\" ]; then\n"
        "    echo \"FAIL STEP 8: $AGENT_SRC not present (host prefetch-repo failed?)\"\n"
        "elif ! command -v gcc >/dev/null 2>&1; then\n"
        "    echo \"FAIL STEP 8: gcc not installed (STEP 7.4 apt failed?)\"\n"
        "elif ! command -v make >/dev/null 2>&1; then\n"
        "    echo \"FAIL STEP 8: make not installed (STEP 7.4 apt failed?)\"\n"
        "else\n"
        "    cd \"$AGENT_SRC\"\n"
        "    # Build, capturing log; pipefail propagates make's rc.\n"
        "    if make -j$(nproc) 2>&1 | tail -20; then\n"
        "        echo \"OK: make\"\n"
        "        if make install PREFIX=/usr/local 2>&1 | tail -5; then\n"
        "            echo \"OK: make install\"\n"
        "        else\n"
        "            echo \"FAIL: make install (rc=$?)\"\n"
        "        fi\n"
        "    else\n"
        "        echo \"FAIL: make (rc=$?) — see lines above\"\n"
        "    fi\n"
        "    cd /\n"
        "fi\n"
        "echo \"==== STEP 8 finished in $((SECONDS - BUILD_T0)) s ====\"\n"
        "ls -la /usr/local/bin/appsandbox-* 2>&1 || true\n"
        "\n"
        "# --- STEP 9: install systemd unit files ---\n"
        "echo \"==== STEP 9: install systemd units ====\"\n"
        "for unit in appsandbox-agent.service appsandbox-audio.service \\\n"
        "            appsandbox-display.service appsandbox-input.service \\\n"
        "            asb-evict-simpledrm.service; do\n"
        "    if [ -f \"$EXTRAS/systemd/$unit\" ]; then\n"
        "        install -m 0644 \"$EXTRAS/systemd/$unit\" /etc/systemd/system/ \\\n"
        "          && echo \"OK: install $unit\" || echo \"FAIL: install $unit\"\n"
        "    fi\n"
        "done\n"
        "# Clipboard is a user-level unit (graphical-session.target).\n"
        "if [ -f \"$EXTRAS/systemd/appsandbox-clipboard.service\" ]; then\n"
        "    install -d /etc/systemd/user\n"
        "    install -m 0644 \"$EXTRAS/systemd/appsandbox-clipboard.service\" /etc/systemd/user/ \\\n"
        "      && echo \"OK: install user clipboard unit\" || echo \"FAIL\"\n"
        "fi\n"
        "\n"
        "# --- STEP 10: install modules-load.d + modprobe.d ---\n"
        "echo \"==== STEP 10: modules-load.d + modprobe.d ====\"\n"
        "for conf in \"$EXTRAS\"/systemd/modules-load.d-*.conf; do\n"
        "    [ -f \"$conf\" ] || continue\n"
        "    dest=/etc/modules-load.d/$(basename \"$conf\" | sed 's/^modules-load.d-//')\n"
        "    install -m 0644 \"$conf\" \"$dest\" \\\n"
        "      && echo \"OK: $dest\" || echo \"FAIL: $dest\"\n"
        "done\n"
        "if [ -f \"$EXTRAS/modprobe.d-asb_drm.conf\" ]; then\n"
        "    install -m 0644 \"$EXTRAS/modprobe.d-asb_drm.conf\" /etc/modprobe.d/asb_drm.conf \\\n"
        "      && echo \"OK: /etc/modprobe.d/asb_drm.conf\" || echo \"FAIL\"\n"
        "fi\n"
        "# dxgkrnl auto-load: the repo has no modules-load.d-dxgkrnl.conf,\n"
        "# so create one inline. Without\n"
        "# this, dxgkrnl loads once via STEP 16 modprobe but is gone after\n"
        "# the STEP 99 reboot, silently breaking /dev/dxg + GPU-PV.\n"
        "echo dxgkrnl > /etc/modules-load.d/dxgkrnl.conf \\\n"
        "  && echo \"OK: /etc/modules-load.d/dxgkrnl.conf\" || echo \"FAIL\"\n"
        "\n"
        "# --- STEP 12: DKMS build ---\n"
        "# build-essential + dkms + linux-headers-$TGT_KVER were all\n"
        "# installed in STEP 7.4. This step just does dkms add/build/install.\n"
        "echo \"==== STEP 12: DKMS build ====\"\n"
        "DKMS_T0=$SECONDS\n"
        "if ! command -v dkms >/dev/null 2>&1; then\n"
        "    echo \"FAIL STEP 12: dkms not installed (STEP 7.4 apt failed?)\"\n"
        "elif [ ! -d \"/lib/modules/$TGT_KVER/build\" ]; then\n"
        "    echo \"FAIL STEP 12: /lib/modules/$TGT_KVER/build missing — linux-headers-$TGT_KVER not installed\"\n"
        "else\n"
        "# DKMS source trees come from the host prefetch (--prefetch-repo\n"
        "# laid them down at /opt/appsandbox/{asb_drm,dxgkrnl}-src/).\n"
        "for mod in asb_drm dxgkrnl; do\n"
        "    SRC=\"$EXTRAS/$mod-src\"\n"
        "    [ -d \"$SRC\" ] || { echo \"SKIP $mod: no $SRC (host prefetch failed?)\"; continue; }\n"
        "    ver=$(awk -F= '/^PACKAGE_VERSION=/{gsub(/\"/,\"\",$2); print $2}' \"$SRC/dkms.conf\" 2>/dev/null)\n"
        "    [ -z \"$ver\" ] && ver=1.0.0\n"
        "    DEST=/usr/src/$mod-$ver\n"
        "    rm -rf \"$DEST\"\n"
        "    cp -r \"$SRC\" \"$DEST\"\n"
        "    dkms add -m $mod -v $ver 2>&1 | tail -2 || true\n"
        "    # pipefail is on, so the if/then sees dkms's exit, not tail's.\n"
        "    if dkms build -m $mod -v $ver -k \"$TGT_KVER\" 2>&1 | tail -5; then\n"
        "        echo \"OK: dkms build $mod\"\n"
        "    else\n"
        "        echo \"FAIL: dkms build $mod (rc=$?) — dumping make.log:\"\n"
        "        for log in $(find /var/lib/dkms -name make.log 2>/dev/null); do\n"
        "            echo \"---- $log (last 40 lines) ----\"\n"
        "            tail -40 \"$log\"\n"
        "        done\n"
        "    fi\n"
        "    if dkms install -m $mod -v $ver -k \"$TGT_KVER\" 2>&1 | tail -3; then\n"
        "        echo \"OK: dkms install $mod\"\n"
        "    else\n"
        "        echo \"FAIL: dkms install $mod (rc=$?)\"\n"
        "    fi\n"
        "done\n"
        "if depmod -a \"$TGT_KVER\"; then echo \"OK: depmod\"; else echo \"FAIL: depmod\"; fi\n"
        "fi  # end if dkms available + headers present\n"
        "echo \"==== STEP 12 finished in $((SECONDS - DKMS_T0)) s ====\"\n"
        "# Quick visibility: what .ko files actually exist for the running kernel?\n"
        "find /lib/modules/$TGT_KVER -name 'asb_drm.ko*' -o -name 'dxgkrnl.ko*' 2>/dev/null || true\n"
        "\n"
        "# --- STEP 13: wsl-mesa (optional GPU acceleration tarball) ---\n"
        "echo \"==== STEP 13: wsl-mesa ====\"\n"
        "if [ -f \"$EXTRAS/wsl-mesa.tar.zst\" ]; then\n"
        "    if ! command -v zstd >/dev/null 2>&1; then\n"
        "        DEBIAN_FRONTEND=noninteractive apt-get install -y zstd 2>&1 | tail -3\n"
        "    fi\n"
        "    if command -v zstd >/dev/null 2>&1; then\n"
        "        zstd -d \"$EXTRAS/wsl-mesa.tar.zst\" -c | tar -C / -x \\\n"
        "          && echo \"OK: wsl-mesa extracted to /opt/wsl-mesa\" \\\n"
        "          || echo \"FAIL: wsl-mesa extract\"\n"
        "        echo /opt/wsl-mesa/lib/" IP_MULTIARCH_A " > /etc/ld.so.conf.d/wsl-mesa.conf\n"
        "        install -d /etc/vulkan/icd.d\n"
        "        if [ -f /opt/wsl-mesa/share/vulkan/icd.d/" IP_DZN_ICD_A " ]; then\n"
        "            ln -sf /opt/wsl-mesa/share/vulkan/icd.d/" IP_DZN_ICD_A " \\\n"
        "                   /etc/vulkan/icd.d/" IP_DZN_ICD_A "\n"
        "        fi\n"
        "    fi\n"
        "else\n"
        "    echo \"no wsl-mesa.tar.zst (GPU acceleration tarball not staged)\"\n"
        "fi\n"
        "\n"
        "# --- STEP 14: wsl-deps on ldconfig path ---\n"
        "echo \"==== STEP 14: wsl-deps ====\"\n"
        "if [ -d \"$EXTRAS/wsl-deps\" ]; then\n"
        "    echo /opt/appsandbox/wsl-deps > /etc/ld.so.conf.d/appsandbox-wsl-deps.conf \\\n"
        "      && echo \"OK: ld.so.conf.d/appsandbox-wsl-deps.conf\" || echo \"FAIL\"\n"
        "else\n"
        "    echo \"no wsl-deps dir\"\n"
        "fi\n"
        "ldconfig && echo \"OK: ldconfig\" || echo \"FAIL: ldconfig\"\n"
        "\n"
        "# --- STEP 15: Mesa env files + nvidia-smi symlink + appsandbox-gpu helper ---\n"
        "echo \"==== STEP 15: Mesa env + nvidia-smi ====\"\n"
        "if [ -f \"$EXTRAS/50-appsandbox-gpu\" ]; then\n"
        "    install -d /etc/systemd/user-environment-generators\n"
        "    install -m 0755 \"$EXTRAS/50-appsandbox-gpu\" \\\n"
        "        /etc/systemd/user-environment-generators/ \\\n"
        "      && echo \"OK: user-environment-generator\" || echo \"FAIL\"\n"
        "fi\n"
        "if [ -f \"$EXTRAS/org.gnome.Shell-no-gpu.conf\" ]; then\n"
        "    install -d /etc/systemd/user/org.gnome.Shell@.service.d\n"
        "    install -m 0644 \"$EXTRAS/org.gnome.Shell-no-gpu.conf\" \\\n"
        "        /etc/systemd/user/org.gnome.Shell@.service.d/no-gpu.conf \\\n"
        "      && echo \"OK: Shell no-gpu drop-in\" || echo \"FAIL\"\n"
        "fi\n"
        "if [ -f \"$EXTRAS/appsandbox-gpu\" ]; then\n"
        "    install -m 0755 \"$EXTRAS/appsandbox-gpu\" /usr/local/bin/ \\\n"
        "      && echo \"OK: appsandbox-gpu helper\" || echo \"FAIL\"\n"
        "fi\n"
        "# nvidia-smi via the agent's 9P mount of WSL libs (symlink target\n"
        "# resolved at exec time; survives if the file is mounted later).\n"
        "ln -sf /usr/lib/wsl/lib/nvidia-smi /usr/local/bin/nvidia-smi || true\n"
        "\n"
        "# --- STEP 16: daemon-reload + enable units + mask sleep + dconf no-sleep ---\n"
        "echo \"==== STEP 16: enable services + disable sleep ====\"\n"
        "systemctl daemon-reload || true\n"
        "for u in appsandbox-agent.service appsandbox-audio.service \\\n"
        "         appsandbox-display.service appsandbox-input.service \\\n"
        "         asb-evict-simpledrm.service; do\n"
        "    if [ -f /etc/systemd/system/$u ]; then\n"
        "        systemctl enable $u 2>&1 | tail -1\n"
        "    fi\n"
        "done\n"
        "systemctl set-default graphical.target 2>&1 | tail -1 || true\n"
        "# Mask all sleep paths (VM has no human at a seat to wake it).\n"
        "systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target 2>&1 | tail -1 || true\n"
        "install -d /etc/systemd/logind.conf.d\n"
        "cat > /etc/systemd/logind.conf.d/10-appsandbox-nosleep.conf <<'EOF'\n"
        "[Login]\n"
        "HandleLidSwitch=ignore\n"
        "HandleLidSwitchDocked=ignore\n"
        "HandleLidSwitchExternalPower=ignore\n"
        "IdleAction=ignore\n"
        "EOF\n"
        "# dconf for no GNOME blank / idle / lock.\n"
        "install -d /etc/dconf/db/local.d /etc/dconf/profile\n"
        "cat > /etc/dconf/profile/user <<'EOF'\n"
        "user-db:user\n"
        "system-db:local\n"
        "EOF\n"
        "cat > /etc/dconf/db/local.d/00-appsandbox-nosleep <<'EOF'\n"
        "[org/gnome/desktop/session]\n"
        "idle-delay=uint32 0\n"
        "\n"
        "[org/gnome/desktop/screensaver]\n"
        "lock-enabled=false\n"
        "idle-activation-enabled=false\n"
        "\n"
        "[org/gnome/settings-daemon/plugins/power]\n"
        "sleep-inactive-ac-type='nothing'\n"
        "sleep-inactive-battery-type='nothing'\n"
        "idle-dim=false\n"
        "power-button-action='nothing'\n"
        "EOF\n"
        "command -v dconf >/dev/null 2>&1 && dconf update 2>&1 || echo \"SKIP dconf update\"\n"
        "# Kernel cmdline drop-in: suppress efifb/simplefb fbdev paths so\n"
        "# asb_drm is the only DRM device on subsequent boots. Matches the\n"
        "# a grub.d drop-in.\n"
        "# NOTE this doesn't kill simpledrm on Hyper-V Gen 2 by itself —\n"
        "# asb-evict-simpledrm.service handles that — but it stops legacy\n"
        "# fbdev paths from also binding.\n"
        "install -d /etc/default/grub.d\n"
        "printf 'GRUB_CMDLINE_LINUX_DEFAULT=\"$GRUB_CMDLINE_LINUX_DEFAULT video=efifb:off video=simplefb:off\"\\n' \\\n"
        "    > /etc/default/grub.d/99-appsandbox-no-efifb.cfg\n"
        "command -v update-grub >/dev/null 2>&1 && update-grub 2>&1 | tail -3 \\\n"
        "  && echo \"OK: update-grub with video=*:off cmdline\" || echo \"SKIP update-grub\"\n"
        "# Load asb_drm + dxgkrnl now so they're live on the next boot (they're\n"
        "# already in modules-load.d so subsequent boots auto-load).\n"
        "modprobe asb_drm 2>&1 || echo \"WARN: modprobe asb_drm\"\n"
        "modprobe dxgkrnl 2>&1 || echo \"WARN: modprobe dxgkrnl\"\n"
        "\n"
        "# --- STEP 17: end-of-firstboot verification summary ---\n"
        "# At-a-glance OK/MISSING for every critical artifact so the com1\n"
        "# capture tells you immediately whether the VM will work after the\n"
        "# STEP 99 reboot — no need to grep through scattered WARN lines.\n"
        "echo \"==== STEP 17: verification ====\"\n"
        "set +x\n"
        "_check_file() { if [ -e \"$1\" ]; then echo \"  [OK]      $1\"; else echo \"  [MISSING] $1\"; fi; }\n"
        "_check_glob() { if compgen -G \"$1\" >/dev/null 2>&1; then echo \"  [OK]      $1\"; else echo \"  [MISSING] $1\"; fi; }\n"
        "_check_unit_enabled() {\n"
        "    state=$(systemctl is-enabled \"$1\" 2>&1)\n"
        "    echo \"  [$state] $1\"\n"
        "}\n"
        "echo \"-- agent binaries (in /usr/local/bin/) --\"\n"
        "for b in appsandbox-agent appsandbox-audio appsandbox-clipboard \\\n"
        "         appsandbox-display appsandbox-input; do\n"
        "    _check_file /usr/local/bin/$b\n"
        "done\n"
        "echo \"-- system unit files --\"\n"
        "for u in appsandbox-agent.service appsandbox-audio.service \\\n"
        "         appsandbox-display.service appsandbox-input.service \\\n"
        "         asb-evict-simpledrm.service; do\n"
        "    _check_file /etc/systemd/system/$u\n"
        "done\n"
        "_check_file /etc/systemd/user/appsandbox-clipboard.service\n"
        "echo \"-- unit enablement --\"\n"
        "for u in appsandbox-agent.service appsandbox-audio.service \\\n"
        "         appsandbox-display.service appsandbox-input.service \\\n"
        "         asb-evict-simpledrm.service; do\n"
        "    _check_unit_enabled $u\n"
        "done\n"
        "echo \"-- modules-load.d + modprobe.d --\"\n"
        "_check_file /etc/modules-load.d/asb_drm.conf\n"
        "_check_file /etc/modules-load.d/dxgkrnl.conf\n"
        "_check_file /etc/modules-load.d/snd-aloop.conf\n"
        "_check_file /etc/modprobe.d/asb_drm.conf\n"
        "echo \"-- kernel modules for $TGT_KVER (DKMS-built) --\"\n"
        "_check_glob \"/lib/modules/$TGT_KVER/updates/dkms/asb_drm.ko*\"\n"
        "_check_glob \"/lib/modules/$TGT_KVER/updates/asb_drm.ko*\"\n"
        "_check_glob \"/lib/modules/$TGT_KVER/updates/dkms/dxgkrnl.ko*\"\n"
        "_check_glob \"/lib/modules/$TGT_KVER/updates/dxgkrnl/dxgkrnl.ko*\"\n"
        "echo \"-- local apt sources --\"\n"
        "_check_file /etc/apt/appsandbox-sources.list.d/appsandbox-local.list\n"
        "_check_file /etc/apt/appsandbox-sources.list.d/appsandbox-local-extras.list\n"
        "_check_glob '/opt/appsandbox/local-apt-extras/*.deb'\n"
        "echo \"-- staged source trees (from host prefetch) --\"\n"
        "_check_file /opt/appsandbox/agent-src/Makefile\n"
        "_check_file /opt/appsandbox/asb_drm-src/dkms.conf\n"
        "_check_file /opt/appsandbox/dxgkrnl-src/dkms.conf\n"
        "if [ -f /etc/appsandbox-ssh-enabled ]; then\n"
        "    echo \"-- ssh (host requested ssh_enabled) --\"\n"
        "    _check_file /usr/sbin/sshd\n"
        "    if systemctl is-active --quiet ssh.service 2>/dev/null \\\n"
        "       || systemctl is-active --quiet ssh.socket 2>/dev/null; then\n"
        "        echo \"  [OK]      ssh listening\"\n"
        "    else\n"
        "        echo \"  [MISSING] ssh.service/ssh.socket inactive\"\n"
        "    fi\n"
        "fi\n"
        "echo \"-- currently loaded modules --\"\n"
        "lsmod 2>/dev/null | grep -E '^(asb_drm|dxgkrnl|snd_aloop)' | awk '{print \"  [LOADED]  \"$1}' || true\n"
        "echo \"-- /dev/dri + /dev/dxg + /dev/uinput --\"\n"
        "# asb_drm registers as whatever card index DRM hands out (varies\n"
        "# based on what else bound — usually card1 once simpledrm is\n"
        "# evicted by asb-evict-simpledrm.service). Resolve by following\n"
        "# /sys/class/drm/card*/device/driver instead of hardcoding card0.\n"
        "asb_card=\"\"\n"
        "for c in /sys/class/drm/card*; do\n"
        "    [ -e \"$c/device/driver\" ] || continue\n"
        "    drv=$(basename \"$(readlink -f \"$c/device/driver\")\")\n"
        "    if [ \"$drv\" = asb_drm ]; then asb_card=$(basename \"$c\"); break; fi\n"
        "done\n"
        "if [ -n \"$asb_card\" ]; then\n"
        "    echo \"  [OK]      /dev/dri/$asb_card (asb_drm)\"\n"
        "else\n"
        "    echo \"  [MISSING] no /dev/dri/card* bound to asb_drm\"\n"
        "fi\n"
        "_check_file /dev/dxg\n"
        "_check_file /dev/uinput\n"
        "echo \"-- ldconfig + Vulkan ICD --\"\n"
        "_check_file /etc/ld.so.conf.d/appsandbox-wsl-deps.conf\n"
        "_check_file /etc/ld.so.conf.d/wsl-mesa.conf\n"
        "_check_file /etc/vulkan/icd.d/" IP_DZN_ICD_A "\n"
        "echo \"-- grub cmdline drop-in --\"\n"
        "_check_file /etc/default/grub.d/99-appsandbox-no-efifb.cfg\n"
        "_check_glob '/opt/appsandbox/local-apt/pool/main/*'\n"
        "echo \"-- regional settings (from host) --\"\n"
        "echo \"  hostname: $(cat /etc/hostname 2>/dev/null)\"\n"
        "echo \"  locale:   $(grep -h LANG= /etc/default/locale 2>/dev/null | head -1)\"\n"
        "echo \"  keyboard: $(grep -h XKBLAYOUT /etc/default/keyboard 2>/dev/null | head -1)\"\n"
        "echo \"  timezone: $(cat /etc/timezone 2>/dev/null)\"\n"
        "set -x\n"
        "\n"
        "# --- STEP 99: Mark done + reboot ---\n"
        "echo \"==== STEP 99: mark done + reboot ====\"\n"
        "mkdir -p /var/lib || true\n"
        "touch /var/lib/appsandbox-firstboot.done\n"
        "systemctl disable appsandbox-firstboot.service 2>&1 || true\n"
        "sync\n"
        "echo \"==== appsandbox-firstboot finished, rebooting ====\"\n"
        "systemctl reboot --force --force\n";

    ext4_mkdir_p(ew, "/usr/local/bin");
    ext4_writer_add_file(ew, "/usr/local/bin/appsandbox-firstboot.sh",
                         0755, 0, 0, (uint32_t)time(NULL),
                         firstboot_sh, strlen(firstboot_sh));

    /* Unit: oneshot, RemainAfterExit so systemd remembers it ran. Output
       goes to journal + console so the com1 named pipe captures every
       step boundary in real time. WantedBy=multi-user.target — pre-enabled
       via the symlink below. */
    const char *firstboot_unit =
        "[Unit]\n"
        "Description=AppSandbox first-boot setup\n"
        "After=local-fs.target\n"
        "ConditionPathExists=!/var/lib/appsandbox-firstboot.done\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        "ExecStart=/usr/local/bin/appsandbox-firstboot.sh\n"
        "StandardOutput=journal+console\n"
        "StandardError=journal+console\n"
        "TimeoutStartSec=300\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";
    ext4_mkdir_p(ew, "/etc/systemd/system");
    ext4_writer_add_file(ew, "/etc/systemd/system/appsandbox-firstboot.service",
                         0644, 0, 0, (uint32_t)time(NULL),
                         firstboot_unit, strlen(firstboot_unit));

    /* Pre-enable via the wants/ symlink — byte-equivalent to what
       `systemctl enable` writes at runtime. */
    ext4_mkdir_p(ew, "/etc/systemd/system/multi-user.target.wants");
    ext4_writer_add_symlink(ew,
        "/etc/systemd/system/multi-user.target.wants/appsandbox-firstboot.service",
        "/etc/systemd/system/appsandbox-firstboot.service",
        (uint32_t)strlen("/etc/systemd/system/appsandbox-firstboot.service"),
        0, 0, (uint32_t)time(NULL));
}

/* ======================================================================
 *  --stage MANIFEST file injection into rootfs.
 *  Same tab-separated format as the Windows path's stage_files() in
 *  iso-patch.c, but writes via ext4_writer_add_file instead of CopyFileW.
 *  Manifest lines: <src_windows_path>\t<dest_in_rootfs>
 * ====================================================================== */

static int stage_manifest_into_rootfs(const wchar_t *manifest_path,
                                      ext4_writer_t *ew)
{
    FILE *f = NULL;
    wchar_t line[2048];
    int count = 0;

    if (_wfopen_s(&f, manifest_path, L"r, ccs=UTF-8") != 0 || !f) {
        log_err(L"Cannot open staging manifest: %s", manifest_path);
        return -1;
    }
    while (fgetws(line, 2048, f)) {
        wchar_t *src, *rel_dest, *tab;
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
            line[--len] = 0;
        if (len == 0 || line[0] == L'#') continue;
        if (line[0] == L'\xFEFF') { memmove(line, line + 1, len * sizeof(wchar_t)); len--; if (!len) continue; }
        tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = 0;
        src = line;
        rel_dest = tab + 1;

        HANDLE h = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            log_err(L"manifest: missing source: %s", src);
            continue;
        }
        LARGE_INTEGER sz;
        GetFileSizeEx(h, &sz);
        void *buf = malloc((size_t)sz.QuadPart);
        DWORD br = 0;
        if (buf && ReadFile(h, buf, (DWORD)sz.QuadPart, &br, NULL) &&
            br == sz.QuadPart) {
            char dst[1024];
            WideCharToMultiByte(CP_UTF8, 0, rel_dest, -1, dst, sizeof(dst), NULL, NULL);
            /* Normalise: ensure leading '/' and back->fwd slashes. */
            char norm[1024];
            int ni = 0;
            if (dst[0] != '/') norm[ni++] = '/';
            for (int i = 0; dst[i] && ni < (int)sizeof(norm) - 1; i++) {
                norm[ni++] = (dst[i] == '\\') ? '/' : dst[i];
            }
            norm[ni] = 0;
            /* mkdir parents. */
            char parent[1024];
            strncpy(parent, norm, sizeof(parent));
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) {
                *slash = 0;
                ext4_mkdir_p(ew, parent);
            }
            if (ext4_writer_add_file(ew, norm, 0644, 0, 0,
                    (uint32_t)time(NULL), buf, (uint64_t)sz.QuadPart) == 0) {
                count++;
            }
        }
        free(buf);
        CloseHandle(h);
    }
    fclose(f);
    return count;
}

/* ======================================================================
 *  Top-level orchestrator.
 * ====================================================================== */

int do_ubuntu_to_vhdx(const wchar_t *iso_path_arg,
                      const wchar_t *vhdx_path_arg,
                      int size_gb,
                      const wchar_t *manifest_path)
{
    int exit_code = 1;
    DWORD result;
    wchar_t iso_path[MAX_PATH];
    wchar_t vhdx_path[MAX_PATH];

    HANDLE iso_handle = INVALID_HANDLE_VALUE;
    wchar_t iso_drive = 0;
    DWORD cdrom_before = 0;

    HANDLE vhdx_handle = INVALID_HANDLE_VALUE;
    HANDLE disk_handle = INVALID_HANDLE_VALUE;
    wchar_t phys_path[MAX_PATH];
    DWORD disk_number = 0;

    wchar_t esp_mount[MAX_PATH] = { 0 };
    BOOL esp_mounted = FALSE;

    if (size_gb < 16) size_gb = 16;
    uint64_t total_bytes = (uint64_t)size_gb * 1024ULL * 1024ULL * 1024ULL;

    if (!GetFullPathNameW(iso_path_arg, MAX_PATH, iso_path, NULL)) {
        log_err(L"Invalid ISO path: %s", iso_path_arg);
        return 1;
    }
    if (GetFileAttributesW(iso_path) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"ISO not found: %s", iso_path);
        return 1;
    }
    if (!GetFullPathNameW(vhdx_path_arg, MAX_PATH, vhdx_path, NULL)) {
        log_err(L"Invalid VHDX path: %s", vhdx_path_arg);
        return 1;
    }
    DeleteFileW(vhdx_path);

    /* ---- Step 1: Mount the ISO. ---- */
    log_msg(L"Mounting ISO...");
    log_progress(2, L"Mounting ISO");
    {
        VIRTUAL_STORAGE_TYPE st;
        OPEN_VIRTUAL_DISK_PARAMETERS op;
        ZeroMemory(&st, sizeof(st));
        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
        ZeroMemory(&op, sizeof(op));
        op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

        cdrom_before = u_snapshot_cdrom_drives();
        result = OpenVirtualDisk(&st, iso_path,
                                 VIRTUAL_DISK_ACCESS_READ,
                                 OPEN_VIRTUAL_DISK_FLAG_NONE,
                                 &op, &iso_handle);
        if (result != ERROR_SUCCESS) {
            log_err(L"OpenVirtualDisk(ISO) failed (%lu)", result);
            goto cleanup;
        }
        result = AttachVirtualDisk(iso_handle, NULL,
                                   ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                                   0, NULL, NULL);
        if (result != ERROR_SUCCESS) {
            log_err(L"AttachVirtualDisk(ISO) failed (%lu)", result);
            goto cleanup;
        }
        for (int i = 0; i < 20; i++) {
            DWORD now = u_snapshot_cdrom_drives();
            DWORD newly = now & ~cdrom_before;
            if (newly) {
                for (int b = 0; b < 26; b++) {
                    if (newly & (1u << b)) { iso_drive = (wchar_t)(L'A' + b); break; }
                }
                break;
            }
            Sleep(500);
        }
        if (!iso_drive) {
            log_err(L"ISO mounted but no drive letter appeared");
            goto cleanup;
        }
    }
    {
        wchar_t root[4]; swprintf(root, 4, L"%c:\\", iso_drive);
        wchar_t label[64];
        int ready = 0;
        for (int i = 0; i < 30; i++) {
            if (GetVolumeInformationW(root, label, 64, NULL, NULL, NULL, NULL, 0)) {
                ready = 1; break;
            }
            Sleep(500);
        }
        if (!ready) {
            log_err(L"ISO volume never became accessible");
            goto cleanup;
        }
    }

    /* ---- Step 2: Locate the rootfs squashfs.
     * Use minimal.squashfs (the full base, ~3.4 GiB on 26.04 desktop daily) —
     * it contains /boot/vmlinuz-X.Y.Z + /boot/initrd.img-X.Y.Z. The smaller
     * minimal.standard.squashfs is an overlay only (no /boot, no kernel)
     * and is not usable as a standalone rootfs source. */
    wchar_t sqfs_path[MAX_PATH];
    swprintf(sqfs_path, MAX_PATH, L"%c:\\casper\\minimal.squashfs", iso_drive);
    if (GetFileAttributesW(sqfs_path) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"casper/minimal.squashfs not found on ISO");
        goto cleanup;
    }
    log_msg(L"Source squashfs: %s", sqfs_path);

    /* ---- Step 3: Create + attach the VHDX (fixed allocation). ---- */
    log_msg(L"Creating %d GiB VHDX...", size_gb);
    log_progress(4, L"Creating VHDX");
    {
        VIRTUAL_STORAGE_TYPE st;
        CREATE_VIRTUAL_DISK_PARAMETERS params;
        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        st.VendorId = VHDX_VENDOR_MS_LOCAL;
        ZeroMemory(&params, sizeof(params));
        params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        params.Version2.MaximumSize = total_bytes;
        params.Version2.BlockSizeInBytes = 0;
        params.Version2.SectorSizeInBytes = 512;
        params.Version2.PhysicalSectorSizeInBytes = 4096;
        /* Dynamic VHDX: file grows on demand, matches the Windows
           --to-vhdx path. An earlier revision used fixed allocation but
           with the GPT/UPDATE_PROPERTIES bug fixed, dynamic works for
           Linux too and saves the upfront 40 GB zeroing. */
        result = CreateVirtualDisk(&st, vhdx_path,
                                   VIRTUAL_DISK_ACCESS_NONE,
                                   NULL,
                                   CREATE_VIRTUAL_DISK_FLAG_NONE,
                                   0, &params, NULL, &vhdx_handle);
        if (result != ERROR_SUCCESS) {
            log_err(L"CreateVirtualDisk failed (%lu)", result);
            goto cleanup;
        }
    }
    {
        ATTACH_VIRTUAL_DISK_PARAMETERS ap;
        ZeroMemory(&ap, sizeof(ap));
        ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
        /* BYPASS_DEFAULT_ENCRYPTION_POLICY: prevents the new VHDX from
           auto-inheriting BitLocker encryption on host volumes that have
           default-encrypt enabled, which would block UEFI from reading
           our boot sectors.  */
        result = AttachVirtualDisk(vhdx_handle, NULL,
                                   ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER |
                                   ATTACH_VIRTUAL_DISK_FLAG_BYPASS_DEFAULT_ENCRYPTION_POLICY,
                                   0, &ap, NULL);
        if (result != ERROR_SUCCESS) {
            log_err(L"AttachVirtualDisk(VHDX) failed (%lu)", result);
            goto cleanup;
        }
    }
    {
        DWORD plen = sizeof(phys_path);
        result = GetVirtualDiskPhysicalPath(vhdx_handle, &plen, phys_path);
        if (result != ERROR_SUCCESS) {
            log_err(L"GetVirtualDiskPhysicalPath failed (%lu)", result);
            goto cleanup;
        }
        disk_number = u_get_disk_number_from_path(phys_path);
    }
    /* FILE_FLAG_NO_BUFFERING + FILE_FLAG_WRITE_THROUGH: mandatory for fast
       writes through \\.\PhysicalDriveN. Without these,
       writes go through NTFS cache - 10-30 MB/s throughput AND ordering
       issues. NO_BUFFERING requires sector-aligned offsets/lengths/buffers,
       which the ext4 writer already guarantees. */
    disk_handle = CreateFileW(phys_path, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                              NULL);
    if (disk_handle == INVALID_HANDLE_VALUE) {
        log_err(L"Open physical disk failed: %lu", GetLastError());
        goto cleanup;
    }

    /* ---- Step 4: Partition (ESP + Linux root). ---- */
    log_msg(L"Partitioning (GPT: ESP %llu MiB + Linux root)...",
            (unsigned long long)(ESP_SIZE_BYTES / (1024 * 1024)));
    log_progress(6, L"Partitioning");
    linux_gpt_layout_t layout = { 0 };
    if (!apply_linux_gpt_layout(disk_handle, total_bytes, ESP_SIZE_BYTES, &layout)) {
        goto cleanup;
    }
    Sleep(250);  /* let Windows settle */

    /* ---- Step 5: Open ext4 writer, ingest squashfs into root. ---- */
    log_msg(L"Building ext4 + ingesting squashfs...");
    log_progress(10, L"Building ext4 root");
    uint64_t root_lba_start = layout.root_offset / 512ULL;
    uint64_t root_lba_count = layout.root_length / 512ULL;

    /* Peek squashfs for inode count to size the ext4 inode table. */
    uint32_t est_inodes = 256 * 1024;
    {
        sqfs_ctx_t *peek = sqfs_open(sqfs_path);
        if (peek) {
            est_inodes = sqfs_sb(peek)->inode_count + 4096;
            sqfs_close(peek);
        }
    }

    ext4_writer_t *ew = ext4_writer_open(disk_handle,
                                          root_lba_start, root_lba_count,
                                          est_inodes);
    if (!ew) {
        log_err(L"ext4_writer_open failed");
        goto cleanup;
    }
    ext4_writer_set_label(ew, "appsandbox-root");
    char uuid_text[40];
    ext4_writer_get_uuid_text(ew, uuid_text, sizeof(uuid_text));
    log_msg(L"root UUID: %hs", uuid_text);

    char kernel_ver[64] = { 0 };
    {
        sqfs_ctx_t *sq = sqfs_open(sqfs_path);
        if (!sq) {
            log_err(L"sqfs_open failed");
            ext4_writer_close(ew);
            goto cleanup;
        }
        pipeline_t pl = { 0 };
        pl.sq = sq;
        pl.ew = ew;
        pl.total_entries = sqfs_sb(sq)->inode_count;   /* drives progress % */
        InitializeCriticalSection(&pl.cs);
        InitializeConditionVariable(&pl.cv_consumer);
        InitializeConditionVariable(&pl.cv_producer);
        InitializeCriticalSection(&pl.wq.cs);
        InitializeConditionVariable(&pl.wq.cv_worker);
        InitializeConditionVariable(&pl.wq.cv_walker);
        pl.n_workers = decide_worker_count();
        log_msg(L"ingest: %d decompress workers", pl.n_workers);

        for (int wi = 0; wi < pl.n_workers; wi++) {
            DWORD tid;
            pl.workers[wi] = CreateThread(NULL, 0, decompress_worker, &pl, 0, &tid);
            if (!pl.workers[wi]) {
                log_err(L"CreateThread(worker) failed");
                /* Stop + join the workers already created so they don't
                   dereference the stack-local 'pl' after we return. */
                EnterCriticalSection(&pl.wq.cs);
                pl.wq.stop = 1;
                WakeAllConditionVariable(&pl.wq.cv_worker);
                LeaveCriticalSection(&pl.wq.cs);
                if (wi > 0) {
                    WaitForMultipleObjects(wi, pl.workers, TRUE, INFINITE);
                    for (int wj = 0; wj < wi; wj++) CloseHandle(pl.workers[wj]);
                }
                DeleteCriticalSection(&pl.cs);
                DeleteCriticalSection(&pl.wq.cs);
                sqfs_close(sq); ext4_writer_close(ew); goto cleanup;
            }
        }
        DWORD ctid;
        HANDLE consumer = CreateThread(NULL, 0, consumer_thread, &pl, 0, &ctid);
        if (!consumer) {
            log_err(L"CreateThread(consumer) failed");
            /* Stop + join all workers (no slots pushed yet; they are
               blocked in work_queue_pop) before the stack-local 'pl' dies. */
            EnterCriticalSection(&pl.wq.cs);
            pl.wq.stop = 1;
            WakeAllConditionVariable(&pl.wq.cv_worker);
            LeaveCriticalSection(&pl.wq.cs);
            WaitForMultipleObjects(pl.n_workers, pl.workers, TRUE, INFINITE);
            for (int wi = 0; wi < pl.n_workers; wi++) CloseHandle(pl.workers[wi]);
            DeleteCriticalSection(&pl.cs);
            DeleteCriticalSection(&pl.wq.cs);
            sqfs_close(sq); ext4_writer_close(ew); goto cleanup;
        }

        int walk_rc = sqfs_walk(sq, producer_cb, &pl);
        EnterCriticalSection(&pl.wq.cs);
        pl.wq.stop = 1;
        WakeAllConditionVariable(&pl.wq.cv_worker);
        LeaveCriticalSection(&pl.wq.cs);
        WaitForMultipleObjects(pl.n_workers, pl.workers, TRUE, INFINITE);
        for (int wi = 0; wi < pl.n_workers; wi++) CloseHandle(pl.workers[wi]);

        EnterCriticalSection(&pl.cs);
        pl.stop = 1;
        WakeConditionVariable(&pl.cv_consumer);
        LeaveCriticalSection(&pl.cs);
        WaitForSingleObject(consumer, INFINITE);
        CloseHandle(consumer);

        DeleteCriticalSection(&pl.cs);
        DeleteCriticalSection(&pl.wq.cs);
        log_msg(L"ingest: walk=%d files=%zu dirs=%zu syms=%zu special=%zu errors=%zu bytes=%.1f MiB",
                walk_rc, pl.n_files, pl.n_dirs, pl.n_syms, pl.n_special, pl.n_errors,
                (double)pl.bytes_total / (1024.0 * 1024.0));
        /* The squashfs reader is the integrity detector: a non-zero walk_rc
           means a structural traversal failure and n_errors counts per-file
           data/decompress failures. Either one means the rootfs is only
           partially ingested, so refuse to finalise it as success (leaving
           exit_code = 1 deletes the incomplete VHDX in cleanup). */
        if (walk_rc != 0 || pl.n_errors != 0) {
            log_err(L"squashfs ingest incomplete (walk=%d errors=%zu): "
                    L"aborting, source image may be truncated or corrupt",
                    walk_rc, pl.n_errors);
            sqfs_close(sq);
            ext4_writer_close(ew);
            goto cleanup;
        }
        strncpy(kernel_ver, pl.kernel_version, sizeof(kernel_ver) - 1);
        if (kernel_ver[0] == 0) {
            /* No /boot/vmlinuz-* was seen during the walk.  minimal.squashfs
               carries the kernel on the 26.04 desktop ISO, but on 24.04 it
               lives only in the minimal.standard.live overlay, so the rootfs
               built here has nothing to boot.  Writing the bootstrap
               grub.cfg with an empty version produces a disk that hangs at
               the GRUB prompt while the UI shows "Installing Linux" forever
               (jamesstringer90/appsandbox#66).  Fail loudly instead;
               exit_code stays 1 so cleanup deletes the VHDX. */
            log_err(L"no kernel (/boot/vmlinuz-*) found in %s - the disk would not boot. "
                    L"Only Ubuntu Desktop 26.04 LTS ISOs are supported.", sqfs_path);
            sqfs_close(sq);
            ext4_writer_close(ew);
            goto cleanup;
        }
        log_msg(L"kernel: %hs", kernel_ver);
        sqfs_close(sq);
    }

    /* ---- Step 6: Write /etc/fstab + mount points. ---- */
    {
        char fstab[512];
        snprintf(fstab, sizeof(fstab),
            "UUID=%s / ext4 rw,relatime,errors=remount-ro 0 1\n"
            "LABEL=ESP /boot/efi vfat umask=0077 0 1\n",
            uuid_text);
        ext4_writer_add_file(ew, "/etc/fstab", 0644, 0, 0,
                             (uint32_t)time(NULL), fstab, strlen(fstab));
        ext4_writer_add_dir(ew, "/boot",     0755, 0, 0, (uint32_t)time(NULL));
        ext4_writer_add_dir(ew, "/boot/efi", 0755, 0, 0, (uint32_t)time(NULL));
    }

    /* ---- Step 7: Stage grub modules + shim binaries from .debs. ---- */
    log_msg(L"Installing grub modules + shim from ISO .debs...");
    /* "Building" — not "Staging". This is part of the vanilla Ubuntu
       install; the AppSandbox-extras staging phase is later (Step 10). */
    log_progress(78, L"Installing grub modules");
    stage_grub_modules(iso_drive, ew);

    /* ---- Step 7b: Stage the ISO's pool/main + dists/ into rootfs so
       first-boot apt operations (dkms + build-essential +
       linux-headers-$(uname -r)) work offline without needing internet
       or a fresh apt update against archive.ubuntu.com. ~296 MiB. ---- */
    log_msg(L"Staging ISO pool/main + dists/ as a local apt source...");
    log_progress(80, L"Building local apt mirror");
    stage_local_apt(iso_drive, ew);

    /* ---- Step 8: Bootstrap /boot/grub/grub.cfg. ---- */
    {
        ext4_writer_add_dir(ew, "/boot/grub", 0755, 0, 0, (uint32_t)time(NULL));
        char boot_cfg[1024];
        snprintf(boot_cfg, sizeof(boot_cfg),
            "set timeout=0\n"
            "menuentry 'Ubuntu (bootstrap)' {\n"
            "    insmod gzio\n"
            "    insmod part_gpt\n"
            "    insmod ext2\n"
            "    set root='hd0,gpt2'\n"
            "    linux  /boot/vmlinuz-%s root=UUID=%s ro"
            " " IP_EARLYCON_A "console=tty0 console=" IP_SERIAL_A ",115200\n"
            "    initrd /boot/initrd.img-%s\n"
            "}\n",
            kernel_ver, uuid_text, kernel_ver);
        ext4_writer_add_file(ew, "/boot/grub/grub.cfg", 0644, 0, 0,
                             (uint32_t)time(NULL),
                             boot_cfg, strlen(boot_cfg));
    }

    /* ---- Step 9: First-boot service. ---- */
    plant_firstboot_service(ew);

    /* ---- Step 10: Honour --stage MANIFEST for caller-supplied files.
       This is the only true "Staging" phase — it copies AppSandbox's own
       resource files (agent ELFs, kernel modules, systemd units, etc.)
       into the rootfs. Everything before this point is the vanilla
       Ubuntu install (reports as "Building" / "Installing"). ---- */
    if (manifest_path) {
        log_msg(L"Staging AppSandbox extras from manifest...");
        log_progress(85, L"Staging AppSandbox extras");
        int n = stage_manifest_into_rootfs(manifest_path, ew);
        if (n >= 0) log_msg(L"Staging: %d file(s)", n);
    }

    /* ---- Close ext4 writer (flushes everything to disk). ---- */
    log_progress(88, L"Finalising ext4");
    if (ext4_writer_close(ew) != 0) {
        log_err(L"ext4_writer_close failed");
        goto cleanup;
    }

    /* ---- Step 11: Re-scan volumes so ESP appears. ---- */
    {
        DWORD bytes;
        DeviceIoControl(disk_handle, IOCTL_DISK_UPDATE_PROPERTIES,
                        NULL, 0, NULL, 0, &bytes, NULL);
    }
    CloseHandle(disk_handle);
    disk_handle = INVALID_HANDLE_VALUE;
    Sleep(1500);  /* let volume manager publish */

    /* ---- Step 12: Mount + format ESP, install signed shim/grub. ---- */
    log_msg(L"Mounting + formatting ESP...");
    log_progress(92, L"ESP setup");
    {
        wchar_t esp_vol[MAX_PATH];
        if (!u_find_partition_volume(disk_number, layout.esp_offset,
                                     esp_vol, MAX_PATH)) {
            log_err(L"Could not find ESP volume");
            goto cleanup;
        }
        if (!u_create_temp_mount_dir(L"asb_uvhdx_esp", esp_mount, MAX_PATH)) {
            log_err(L"create_temp_mount_dir failed");
            goto cleanup;
        }
        if (!u_mount_volume_to_dir(esp_vol, esp_mount)) {
            log_err(L"mount ESP failed: %lu", GetLastError());
            goto cleanup;
        }
        esp_mounted = TRUE;
    }
    if (!u_format_fat32(esp_mount, L"ESP")) {
        log_err(L"format ESP failed");
        goto cleanup;
    }

    log_msg(L"Installing signed shim + grub from ISO .debs...");
    log_progress(95, L"Installing boot loader");
    if (install_signed_efi(iso_drive, esp_mount) != 0) goto cleanup;
    if (write_redirect_grub_cfg(esp_mount, uuid_text) != 0) goto cleanup;

    log_progress(99, L"Done");
    log_done(vhdx_path);
    exit_code = 0;

cleanup:
    if (esp_mounted) {
        u_unmount_volume_from_dir(esp_mount);
        RemoveDirectoryW(esp_mount);
    }
    if (disk_handle != INVALID_HANDLE_VALUE) CloseHandle(disk_handle);
    if (vhdx_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(vhdx_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(vhdx_handle);
    }
    if (iso_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(iso_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(iso_handle);
    }
    if (exit_code != 0 && vhdx_path[0]) DeleteFileW(vhdx_path);
    return exit_code;
}
