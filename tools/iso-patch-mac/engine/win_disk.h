/*
 * win_disk.h -- callable entry point for the from-scratch Windows-disk builder.
 *
 * Builds a UEFI-bootable Windows 11 ARM64 raw disk from a generalized install.wim:
 *   GPT (ESP+MSR+NTFS) -> apply WIM image -> inject answer file -> stage the AppSandbox guest
 *   payload -> FAT32 ESP + boot files + patched BCD.
 *
 * Linked into the iso-patch-mac Xcode target (the standalone main is behind WIN_DISK_STANDALONE).
 */
#ifndef ASB_WIN_DISK_H
#define ASB_WIN_DISK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build the disk. Returns 0 on success.
 *   wim_path     : path to the install.wim (located on the mounted ISO at sources/install.wim)
 *   out_path     : output raw disk image path
 *   image_idx    : WIM image index (usually 1)
 *   disk_gib     : disk size in GiB
 *   manifest_path: a "<host-src>\t<guest-dest>" TSV (same layout as the Windows generate_vhdx_manifest:
 *                  unattend.xml, agent EXEs, drivers, setup.cmd, SetupComplete.cmd at their real guest
 *                  paths). Each dest's parents are created via mkdir -p over the apply-built dir map.
 *                  NULL -> inject only the built-in answer file (standalone smoke test).
 *   progress     : optional callback(pct 0-100, message) for STATUS/PROGRESS reporting, or NULL
 */
int win_disk_build(const char *wim_path,
                   const char *out_path,
                   uint32_t    image_idx,
                   uint64_t    disk_gib,
                   const char *manifest_path,
                   const char *vioscsi_dir,
                   void (*progress)(int pct, const char *msg));

#ifdef __cplusplus
}
#endif

#endif /* ASB_WIN_DISK_H */
