/*
 * win_provision.h -- platform-neutral generator for the Windows-guest provisioning files
 * (unattend.xml / setup.cmd / SetupComplete.cmd).
 *
 * SINGLE SOURCE OF TRUTH: compiled into BOTH the Windows backend (src/backend_win/disk_util.c
 * delegates here) and the macOS tools/iso-patch-mac target, so the staged scripts are byte-identical
 * regardless of host. There is no is-mac/is-pc fork. The AppSandboxSHM driver install is included
 * unconditionally; it is a harmless no-op on Windows-to-Windows (no ivshmem PCI device present).
 * See the [[windows-on-mac-provisioning-unification]] memory.
 */
#ifndef ASB_WIN_PROVISION_H
#define ASB_WIN_PROVISION_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Offline answer file for a from-scratch / VHDX-first install (no windowsPE pass).
 *   f          : open FILE* (binary; a UTF-8 BOM is written first, matching ccs=UTF-8)
 *   vm_name    : ComputerName (truncated to 15 chars)
 *   user, pass : local admin account (pass is plaintext; encoded internally)
 *   arch       : "arm64" or "amd64" (processorArchitecture)
 *   test_mode  : non-zero -> bcdedit /set testsigning on
 *   is_arm64   : non-zero -> add the HKLM\SYSTEM\Setup\LabConfig TPM/SecureBoot/... bypass keys
 *   lang       : UI/locale BCP-47 tag (e.g. "en-US")
 * Returns 0 on success. */
int asb_provision_unattend(FILE *f, const char *vm_name, const char *user, const char *pass,
                           const char *arch, int test_mode, int is_arm64, const char *lang,
                           const char *input_locale);

/* setup.cmd -- first-logon: agent already staged at C:\Windows\AppSandbox\; register the service. */
int asb_provision_setup_cmd(FILE *f);

/* SetupComplete.cmd -- runs as SYSTEM before first logon: trust the test cert, install AppSandboxSHM
 * (pnputil; no-op on Win-to-Win), install the VDD + VAD root devices (devcon), disable display sleep,
 * and -- if ssh_msi_name is non-NULL -- install OpenSSH Server (msiexec + sc + net start).
 * ssh_msi_name is the basename of the MSI staged into \Windows\AppSandbox\. */
int asb_provision_setupcomplete(FILE *f, const char *ssh_msi_name);

#ifdef __cplusplus
}
#endif

#endif /* ASB_WIN_PROVISION_H */
