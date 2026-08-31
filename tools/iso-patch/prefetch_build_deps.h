/* prefetch_build_deps.h - host-side prefetch of Ubuntu .debs needed
 * to build the AppSandbox agent ELFs from source on the guest.
 *
 * Runs as a new iso-patch.exe mode (--prefetch-build-deps) so we
 * don't depend on PowerShell being available on the host (corporate
 * AppLocker / ExecutionPolicy / etc. routinely block it). Pure
 * Win32 C: WinHTTP for downloads, vendored xz-embedded for index
 * decompression, BCrypt for SHA256.
 */
#ifndef PREFETCH_BUILD_DEPS_H
#define PREFETCH_BUILD_DEPS_H

#include <windows.h>

/* Prefetch the .deb dependency closure needed to build the agent
 * sources on the guest. Writes to <out_dir>:
 *   - Packages       (synthetic; only the closure stanzas, Filename:
 *                     rewritten to local basenames)
 *   - .closure.json  (manifest, for diagnostics)
 *   - <closure>.deb  (10-15 files, ~10 MiB)
 *
 * Idempotent / atomic: stages into <out_dir>.tmp then renames; never
 * leaves a partial cache behind on failure.
 *
 * Inputs:
 *   codename     - Ubuntu release codename (e.g. "resolute")
 *   kernel_ver   - running kernel version (e.g. "7.0.0-14-generic")
 *                  used to also fetch linux-headers-<ver> defensively
 *   out_dir      - cache destination
 *   mirror       - upstream mirror (default "http://archive.ubuntu.com/ubuntu"
 *                  if NULL)
 *
 * Returns 0 on success; non-zero on any failure. Logs to stdout
 * (caller is iso-patch wmain, prints to console). */
int do_prefetch_build_deps(const wchar_t *codename,
                           const wchar_t *kernel_ver,
                           const wchar_t *out_dir,
                           const wchar_t *mirror);

#endif /* PREFETCH_BUILD_DEPS_H */
