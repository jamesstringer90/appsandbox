#ifndef DISK_UTIL_H
#define DISK_UTIL_H

#include <windows.h>

/* Compile-time target architecture. Build arch == host arch == guest arch
   (x64->x64->x64, ARM64->ARM->ARM; no cross-arch), so one compile-time flag
   drives every architecture decision. */
#if defined(_M_ARM64)
#  define ASB_IS_ARM64 1
#else
#  define ASB_IS_ARM64 0
#endif

/* Create a new dynamically-expanding VHDX file.
   size_gb: maximum virtual size in gigabytes. */
HRESULT vhdx_create(const wchar_t *path, ULONGLONG size_gb);

/* Create a differencing (child) VHDX that references parent_path.
   New writes go to child_path; parent_path remains unchanged. */
HRESULT vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path);

HRESULT vhdx_get_virtual_size(const wchar_t *path, ULONGLONG *size_bytes);

/* Shrinking is rejected. Pass the child path, never the shared template path. */
HRESULT vhdx_grow(const wchar_t *path, ULONGLONG size_gb);

/* Merge a differencing VHDX into its parent.
   After merge, child_path can be deleted. */
HRESULT vhdx_merge(const wchar_t *child_path);

void get_host_keyboard_settings(wchar_t *input_locale, size_t size,
                                DWORD *klid, WORD *langid);

/* Create a resources ISO containing autounattend.xml, agent, and helper
   executables for unattended Windows install with GPU-PV. GPU driver
   file copy is handled by the agent service's embedded 9P client (p9copy).
   res_dir: directory containing the agent and helper executables.
   The password is wiped from memory after use. */
HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode,
                              BOOL ssh_enabled,
                              const wchar_t *lang);

/* Create a resources ISO for an instance created from a template.
   Contains unattend.xml (not autounattend.xml) for post-sysprep mini-setup
   with the real user account, agent, and GPU copy setup. */
HRESULT iso_create_instance_resources(const wchar_t *iso_path,
                                       const wchar_t *vm_name,
                                       const wchar_t *admin_user,
                                       wchar_t *admin_pass,
                                       const wchar_t *res_dir,
                                       BOOL ssh_enabled,
                                       const wchar_t *lang);

/* ---- VHDX-First VM Creation (no resources ISO) ---- */

/* Forward declaration for GPU share list */
struct GpuDriverShareList;

/* Generate unattend.xml for VHDX-first boot (specialize + oobeSystem, no windowsPE).
   Returns TRUE on success. */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode,
                             const wchar_t *lang,
                             const wchar_t *input_locale);

/* Generate unattend.xml for VHDX-first *template* boot.
   Boots into audit mode, runs sysprep /generalize /oobe /shutdown /mode:vm.
   No user account or password needed. */
BOOL generate_unattend_vhdx_template(const wchar_t *output_path,
                                      const wchar_t *vm_name,
                                      BOOL test_mode);

/* Generate setup.cmd for VHDX-first boot (agent already on disk). */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path);

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk). */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path, BOOL ssh_enabled);

/* Generate staging manifest for iso-patch --stage.
   Writes tab-separated source\tdest lines for all files to pre-stage on the VHDX.
   gpu_shares may be NULL if no GPU.
   Returns number of entries written, or -1 on error. */
int generate_vhdx_manifest(const wchar_t *manifest_path,
                            const wchar_t *staging_dir,
                            const wchar_t *res_dir,
                            const void *gpu_shares,
                            BOOL ssh_enabled);

/* Ensure OpenSSH MSI is cached locally (downloads if needed).
   Returns TRUE if the MSI is available at the returned path. */
BOOL ensure_ssh_msi_cached(wchar_t *msi_path_out, int max_chars);

/* Stage the Linux agent ELFs + kernel modules + DKMS sources + systemd
   units + modprobe.d/modules-load.d + wsl-mesa + wsl-deps under
   <staging>/extras/. The direct-ISO->VHDX flow populates this dir (via
   the prefetch modules) and turns it into a manifest for iso-patch --stage. */
void stage_linux_agent_and_extras(const wchar_t *staging,
                                  const wchar_t *res_dir,
                                  BOOL ssh_enabled);

/* Hash a wide-char plaintext password into glibc $6$<salt>$<sha512> format
   for /etc/shadow. Generates a fresh random 16-char salt. UTF-8 conversion
   of the plaintext is wiped on return. Returns TRUE on success. */
BOOL unix_password_hash(const wchar_t *plain, char *hash_out, size_t hash_out_size);

/* Write language.json alongside VHDX */
void vm_save_language_json(const wchar_t *vhdx_path, const wchar_t *lang);

/* Read language.json — returns TRUE if found, fills lang_out */
BOOL vm_load_language_json(const wchar_t *vhdx_path, wchar_t *lang_out, int lang_out_max);

#endif /* DISK_UTIL_H */
