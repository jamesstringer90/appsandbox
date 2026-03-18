#ifndef DISK_UTIL_H
#define DISK_UTIL_H

#include <windows.h>

/* Create a new dynamically-expanding VHDX file.
   size_gb: maximum virtual size in gigabytes. */
HRESULT vhdx_create(const wchar_t *path, ULONGLONG size_gb);

/* Create a differencing (child) VHDX that references parent_path.
   New writes go to child_path; parent_path remains unchanged. */
HRESULT vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path);

/* Merge a differencing VHDX into its parent.
   After merge, child_path can be deleted. */
HRESULT vhdx_merge(const wchar_t *child_path);

/* Create a resources ISO containing autounattend.xml and p9client.exe
   for unattended Windows install with GPU-PV. At each boot, p9client
   reads the manifest share and copies GPU driver files from Plan9 shares
   to the correct guest locations.
   res_dir: directory containing p9client.exe.
   The password is wiped from memory after use. */
HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode);

/* Create a resources ISO for an instance created from a template.
   Contains unattend.xml (not autounattend.xml) for post-sysprep mini-setup
   with the real user account, agent, and GPU copy setup. */
HRESULT iso_create_instance_resources(const wchar_t *iso_path,
                                       const wchar_t *vm_name,
                                       const wchar_t *admin_user,
                                       wchar_t *admin_pass,
                                       const wchar_t *res_dir);

/* ---- VHDX-First VM Creation (no resources ISO) ---- */

/* Forward declaration for GPU share list */
struct GpuDriverShareList;

/* Generate unattend.xml for VHDX-first boot (specialize + oobeSystem, no windowsPE).
   Returns TRUE on success. */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode);

/* Generate unattend.xml for VHDX-first *template* boot.
   Boots into audit mode, runs sysprep /generalize /oobe /shutdown /mode:vm.
   No user account or password needed. */
BOOL generate_unattend_vhdx_template(const wchar_t *output_path,
                                      const wchar_t *vm_name,
                                      BOOL test_mode);

/* Generate setup.cmd for VHDX-first boot (agent already on disk). */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path);

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk). */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path);

/* Generate staging manifest for iso-patch --stage.
   Writes tab-separated source\tdest lines for all files to pre-stage on the VHDX.
   gpu_shares may be NULL if no GPU.
   Returns number of entries written, or -1 on error. */
int generate_vhdx_manifest(const wchar_t *manifest_path,
                            const wchar_t *staging_dir,
                            const wchar_t *res_dir,
                            const void *gpu_shares);

#endif /* DISK_UTIL_H */
