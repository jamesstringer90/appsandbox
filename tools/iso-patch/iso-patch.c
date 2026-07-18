/*
 * iso-patch.exe -- Windows ISO utilities:
 *
 *   iso-patch.exe <path-to-windows.iso>
 *       Rebuild ISO with noprompt UEFI boot files (no "Press Any Key").
 *       Output: <exe-dir>\<basename>_noprompt.iso
 *
 *   iso-patch.exe --to-vhdx <iso> [image-index] [size-gb] [--output path] [--stage manifest]
 *       Convert a Windows installer ISO into a bootable UEFI VHDX.
 *       Output defaults to <exe-dir>\<basename>.vhdx, or --output overrides.
 *       image-index defaults to 1, size-gb defaults to 64 (min 16).
 *       --stage: copy files listed in manifest onto the VHDX.
 *               Manifest: tab-separated lines of <source>\t<dest_relative_to_root>
 */

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <imapi2fs.h>
#include <shlwapi.h>
#include <stdio.h>

#include "ubuntu_vhdx.h"
#include "prefetch_build_deps.h"
#include "prefetch_wsl_deps.h"
#include "prefetch_repo.h"
#include "target_arch.h"

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

/* ---- GUIDs ---- */
/* Use custom names to avoid clashing with EXTERN_C declarations in imapi2fs.h */

static const GUID CLSID_FSImage =
    {0x2C941FC5, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID IID_IFSImage =
    {0x2C941FE1, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID CLSID_BootOpts =
    {0x2C941FCE, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID IID_IBootOpts =
    {0x2C941FD4, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID IID_IFsiDirItem =
    {0x2C941FDC, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};

static const GUID VHDX_VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

/* ---- Helpers ---- */

/* Output protocol (machine-parseable, one per line):
   STATUS:<message>     — step change (shown in UI log)
   PROGRESS:<pct>:<msg> — progress update (updates in place in UI)
   ERROR:<message>      — failure (shown in UI log as error)
   DONE:<path>          — success, path to output file
*/

void log_msg(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wprintf(L"STATUS:");
    vwprintf(fmt, ap);
    wprintf(L"\n");
    fflush(stdout);
    va_end(ap);
}

void log_progress(int pct, const wchar_t *step)
{
    wprintf(L"PROGRESS:%d:%s\n", pct, step);
    fflush(stdout);
}

void log_done(const wchar_t *path)
{
    wprintf(L"DONE:%s\n", path);
    fflush(stdout);
}

void log_err(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fwprintf(stderr, L"ERROR:");
    vfwprintf(stderr, fmt, ap);
    fwprintf(stderr, L"\n");
    fflush(stderr);
    va_end(ap);
}

/* Build output path: <exe_dir>\<basename><suffix> */
static void build_output_path(wchar_t *out, size_t out_len,
                               const wchar_t *input_path, const wchar_t *suffix)
{
    wchar_t exe_dir[MAX_PATH];
    const wchar_t *base_name;
    wchar_t stem[MAX_PATH];
    wchar_t *slash, *dot;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    base_name = wcsrchr(input_path, L'\\');
    base_name = base_name ? base_name + 1 : input_path;
    wcscpy_s(stem, MAX_PATH, base_name);
    dot = wcsrchr(stem, L'.');
    if (dot) *dot = L'\0';

    swprintf_s(out, out_len, L"%s\\%s%s", exe_dir, stem, suffix);
}

/* Run a command and wait for it to finish.
   If quiet=TRUE, child stdout/stderr are suppressed.
   Returns process exit code, or -1 on failure. */
static int run_command(const wchar_t *cmdline, BOOL quiet)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;
    wchar_t *cmd_buf;
    size_t len;
    HANDLE hNul = INVALID_HANDLE_VALUE;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (quiet) {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;
        hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                            &sa, OPEN_EXISTING, 0, NULL);
        if (hNul != INVALID_HANDLE_VALUE) {
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = hNul;
            si.hStdError = hNul;
        }
    }

    /* CreateProcessW needs a mutable command line */
    len = wcslen(cmdline) + 1;
    cmd_buf = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!cmd_buf) {
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    wcscpy_s(cmd_buf, len, cmdline);

    if (!CreateProcessW(NULL, cmd_buf, NULL, NULL, quiet ? TRUE : FALSE,
                         0, NULL, NULL, &si, &pi)) {
        log_err(L"Failed to run: %s (error %lu)", cmdline, GetLastError());
        free(cmd_buf);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return -1;
    }
    free(cmd_buf);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    return (int)exit_code;
}

/* Run a command, capture stdout, and report percentage progress.
   Scans output for patterns like "45.0%" and calls log_msg with updates.
   prefix: status message shown before the percentage (e.g. "Applying Windows image").
   Returns process exit code, or -1 on failure. */
static int run_command_with_progress(const wchar_t *cmdline, const wchar_t *prefix)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;
    wchar_t *cmd_buf;
    size_t len;
    int last_pct = -1;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;

    /* Ensure the read handle is not inherited */
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    ZeroMemory(&pi, sizeof(pi));

    len = wcslen(cmdline) + 1;
    cmd_buf = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!cmd_buf) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }
    wcscpy_s(cmd_buf, len, cmdline);

    if (!CreateProcessW(NULL, cmd_buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        log_err(L"Failed to run: %s (error %lu)", cmdline, GetLastError());
        free(cmd_buf);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }
    free(cmd_buf);

    /* Close write end — we only read */
    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    /* Read output and scan for percentage */
    {
        char buf[4096];
        DWORD bytes_read;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
            buf[bytes_read] = '\0';
            /* Scan for XX.X% pattern */
            for (DWORD i = 0; i < bytes_read; i++) {
                if (buf[i] == '%') {
                    /* Walk backwards to find the number */
                    int j = (int)i - 1;
                    while (j >= 0 && (buf[j] == '.' || (buf[j] >= '0' && buf[j] <= '9')))
                        j--;
                    j++;
                    if (j < (int)i) {
                        /* Parse the percentage as integer */
                        double pct_val = atof(buf + j);
                        int pct = (int)pct_val;
                        if (pct >= 0 && pct <= 100 && pct != last_pct) {
                            last_pct = pct;
                            log_progress(pct, prefix);
                        }
                    }
                }
            }
        }
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

/* Run a command and capture stdout into a caller-provided buffer.
   Returns process exit code, or -1 on failure. */
static int run_command_capture(const wchar_t *cmdline, char *out_buf, int out_buf_size)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code = (DWORD)-1;
    wchar_t *cmd_buf;
    size_t len;
    int total = 0;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    ZeroMemory(&pi, sizeof(pi));

    len = wcslen(cmdline) + 1;
    cmd_buf = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!cmd_buf) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }
    wcscpy_s(cmd_buf, len, cmdline);

    if (!CreateProcessW(NULL, cmd_buf, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        free(cmd_buf);
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }
    free(cmd_buf);

    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    /* Read all output into buffer */
    {
        DWORD bytes_read;
        while (total < out_buf_size - 1 &&
               ReadFile(hReadPipe, out_buf + total,
                        (DWORD)(out_buf_size - 1 - total), &bytes_read, NULL) &&
               bytes_read > 0) {
            total += (int)bytes_read;
        }
        out_buf[total] = '\0';
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

/* Snapshot of which drive letters are CDROM before mounting */
static DWORD snapshot_cdrom_drives(void)
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

/* Find a new CDROM drive letter that appeared after mounting */
static wchar_t find_new_cdrom(DWORD before_mask)
{
    wchar_t root[4] = L"A:\\";
    for (int i = 0; i < 26; i++) {
        root[0] = (wchar_t)(L'A' + i);
        if (!(before_mask & (1u << i)) && GetDriveTypeW(root) == DRIVE_CDROM)
            return (wchar_t)(L'A' + i);
    }
    return 0;
}

/* Write an IStream to a file */
static HRESULT write_stream_to_file(IStream *stream, const wchar_t *path)
{
    HANDLE hFile;
    BYTE buf[65536];
    ULONG bytes_read;
    DWORD bytes_written;
    HRESULT hr;
    LARGE_INTEGER zero;

    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    zero.QuadPart = 0;
    stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);

    for (;;) {
        hr = stream->lpVtbl->Read(stream, buf, sizeof(buf), &bytes_read);
        if (FAILED(hr)) {
            CloseHandle(hFile);
            return hr;
        }
        if (bytes_read == 0) break;
        if (!WriteFile(hFile, buf, bytes_read, &bytes_written, NULL)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(hFile);
            return hr;
        }
    }

    CloseHandle(hFile);
    return S_OK;
}

/* Navigate a directory tree: root -> child1 -> child2 -> ... -> NULL
   Returns the deepest IFsiDirectoryItem. Caller must Release(). */
static HRESULT navigate_fsi_path(IFsiDirectoryItem *root, IFsiDirectoryItem **out,
                                  const wchar_t *name1, ...)
{
    va_list ap;
    IFsiDirectoryItem *current = root;
    IFsiItem *item = NULL;
    const wchar_t *name = name1;
    HRESULT hr = S_OK;
    BSTR bstr;

    current->lpVtbl->AddRef(current);

    va_start(ap, name1);
    while (name) {
        bstr = SysAllocString(name);
        hr = current->lpVtbl->get_Item(current, bstr, &item);
        SysFreeString(bstr);
        if (FAILED(hr)) {
            current->lpVtbl->Release(current);
            va_end(ap);
            return hr;
        }

        IFsiDirectoryItem *next = NULL;
        hr = item->lpVtbl->QueryInterface(item, &IID_IFsiDirItem, (void **)&next);
        item->lpVtbl->Release(item);
        if (FAILED(hr)) {
            current->lpVtbl->Release(current);
            va_end(ap);
            return hr;
        }

        current->lpVtbl->Release(current);
        current = next;
        name = va_arg(ap, const wchar_t *);
    }
    va_end(ap);

    *out = current;
    return S_OK;
}

/* ======================================================================
 *  --to-vhdx: Convert Windows installer ISO to bootable UEFI VHDX
 * ====================================================================== */

/* Partition sizes */
#define EFI_SIZE_MB     200
#define MSR_SIZE_MB     128
#define DEFAULT_VHDX_SIZE_GB 64

/* Find the volume GUID path for a partition on a given disk.
   disk_number: the disk number from GetVirtualDiskPhysicalPath
   partition_number: 1-based partition index in the GPT layout.
                     Pass 0 to match any volume on the disk (e.g. ISO with no partitions).
   Returns TRUE and fills vol_path (e.g. \\?\Volume{guid}\) on success. */
static BOOL find_volume_for_partition(DWORD disk_number, DWORD partition_number,
                                       wchar_t *vol_path, size_t vol_path_len)
{
    HANDLE hFind;
    wchar_t vol_name[MAX_PATH];
    BOOL found = FALSE;

    hFind = FindFirstVolumeW(vol_name, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    do {
        /* Open the volume — strip trailing backslash for CreateFileW */
        wchar_t vol_dev[MAX_PATH];
        HANDLE hVol;
        wcscpy_s(vol_dev, MAX_PATH, vol_name);
        size_t vlen = wcslen(vol_dev);
        if (vlen > 0 && vol_dev[vlen - 1] == L'\\')
            vol_dev[vlen - 1] = L'\0';

        hVol = CreateFileW(vol_dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (hVol != INVALID_HANDLE_VALUE) {
            /* Get the disk extents for this volume */
            BYTE buf[256];
            DWORD bytes;
            if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                 NULL, 0, buf, sizeof(buf), &bytes, NULL)) {
                VOLUME_DISK_EXTENTS *ext = (VOLUME_DISK_EXTENTS *)buf;
                if (ext->NumberOfDiskExtents >= 1 &&
                    ext->Extents[0].DiskNumber == disk_number) {
                    if (partition_number == 0) {
                        /* Match any volume on this disk (e.g. ISO) */
                        wcscpy_s(vol_path, vol_path_len, vol_name);
                        found = TRUE;
                    } else {
                        /* Check partition number via IOCTL_STORAGE_GET_DEVICE_NUMBER */
                        STORAGE_DEVICE_NUMBER sdn;
                        if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                             NULL, 0, &sdn, sizeof(sdn), &bytes, NULL)) {
                            if (sdn.PartitionNumber == partition_number) {
                                wcscpy_s(vol_path, vol_path_len, vol_name);
                                found = TRUE;
                            }
                        }
                    }
                }
            }
            CloseHandle(hVol);
        }
        if (found) break;
    } while (FindNextVolumeW(hFind, vol_name, MAX_PATH));

    FindVolumeClose(hFind);
    return found;
}

/* Extract disk number from physical path like \\.\PhysicalDrive3 */
static DWORD get_disk_number_from_path(const wchar_t *phys_path)
{
    const wchar_t *p = wcsrchr(phys_path, L'e'); /* PhysicalDriv'e' */
    if (p && p[1] >= L'0' && p[1] <= L'9')
        return (DWORD)_wtoi(p + 1);
    /* Fallback: find last run of digits */
    size_t len = wcslen(phys_path);
    while (len > 0 && phys_path[len - 1] >= L'0' && phys_path[len - 1] <= L'9')
        len--;
    return (DWORD)_wtoi(phys_path + len);
}

/* Create a temporary directory under %TEMP% with given prefix.
   Returns TRUE and fills path on success. Path includes trailing backslash. */
static BOOL create_temp_mount_dir(const wchar_t *prefix, wchar_t *path, size_t path_len)
{
    wchar_t temp_dir[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, temp_dir);
    if (len == 0) return FALSE;

    swprintf_s(path, path_len, L"%s%s_%u\\", temp_dir, prefix, GetCurrentProcessId());

    if (!CreateDirectoryW(path, NULL)) {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }
    return TRUE;
}

/* Mount a volume to a directory (no drive letter). vol_path must have trailing backslash.
   mount_dir must have trailing backslash and must be an empty directory. */
static BOOL mount_volume_to_dir(const wchar_t *vol_path, const wchar_t *mount_dir)
{
    return SetVolumeMountPointW(mount_dir, vol_path);
}

/* Unmount a volume from a directory. */
static BOOL unmount_volume_from_dir(const wchar_t *mount_dir)
{
    return DeleteVolumeMountPointW(mount_dir);
}

/* Format a volume using format.exe. fs_type is "FAT32" or "NTFS".
   vol_path is e.g. \\?\Volume{guid}\ — but format.exe needs a drive letter
   or mount point. We pass the mount_dir which has a trailing backslash. */
static BOOL format_volume(const wchar_t *mount_dir, const wchar_t *fs_type, const wchar_t *label)
{
    wchar_t cmd[512];
    wchar_t sys_dir[MAX_PATH];
    int ret;

    GetSystemDirectoryW(sys_dir, MAX_PATH);

    /* Strip trailing backslash — format.com doesn't like "path\" */
    {
        wchar_t mount_clean[MAX_PATH];
        wcscpy_s(mount_clean, MAX_PATH, mount_dir);
        size_t mlen = wcslen(mount_clean);
        if (mlen > 0 && mount_clean[mlen - 1] == L'\\')
            mount_clean[mlen - 1] = L'\0';

        if (label && label[0]) {
            swprintf_s(cmd, 512, L"%s\\format.com \"%s\" /FS:%s /Q /Y /V:%s",
                        sys_dir, mount_clean, fs_type, label);
        } else {
            swprintf_s(cmd, 512, L"%s\\format.com \"%s\" /FS:%s /Q /Y",
                        sys_dir, mount_clean, fs_type);
        }
    }

    ret = run_command(cmd, TRUE);
    if (ret != 0) {
        log_err(L"Failed to format volume (exit code %d)", ret);
        return FALSE;
    }
    return TRUE;
}

/* Recursively create directories for a path.
   path should be the full file path — directories are created for all
   components except the last (the filename). */
static void ensure_parent_dirs(const wchar_t *file_path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *p;

    wcscpy_s(dir, MAX_PATH, file_path);

    /* Find the last backslash (parent directory) */
    p = wcsrchr(dir, L'\\');
    if (!p) return;
    *p = L'\0';

    /* If directory already exists, done */
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES)
        return;

    /* Recursively create parent */
    ensure_parent_dirs(dir);
    CreateDirectoryW(dir, NULL);
}

/* Process a staging manifest file. Each line is tab-separated:
   <source_path>\t<dest_path_relative_to_partition_root>
   Lines starting with # are comments. Empty lines are skipped.
   Returns the number of files staged, or -1 on error. */
static int stage_files(const wchar_t *manifest_path, const wchar_t *mount_root)
{
    FILE *f = NULL;
    wchar_t line[2048];
    int count = 0;
    int total = 0;
    int skipped = 0;

    if (_wfopen_s(&f, manifest_path, L"r, ccs=UTF-8") != 0 || !f) {
        log_err(L"Cannot open staging manifest: %s", manifest_path);
        return -1;
    }

    /* First pass: count lines for progress */
    while (fgetws(line, 2048, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
            line[--len] = L'\0';
        if (len == 0 || line[0] == L'#') continue;
        if (wcschr(line, L'\t')) total++;
    }
    fseek(f, 0, SEEK_SET);

    /* Second pass: copy files */
    while (fgetws(line, 2048, f)) {
        wchar_t *src, *rel_dest, *tab;
        wchar_t dest[MAX_PATH];
        const wchar_t *filename;
        size_t len = wcslen(line);

        /* Strip newline */
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r'))
            line[--len] = L'\0';
        if (len == 0 || line[0] == L'#') continue;

        /* Skip UTF-8 BOM (U+FEFF) that ccs=UTF-8 may produce on first line */
        if (line[0] == L'\xFEFF') {
            memmove(line, line + 1, len * sizeof(wchar_t));
            len--;
            if (len == 0) continue;
        }

        /* Split on tab */
        tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = L'\0';
        src = line;
        rel_dest = tab + 1;

        /* Skip leading backslash on relative dest */
        if (rel_dest[0] == L'\\') rel_dest++;

        /* Build full destination: mount_root + rel_dest */
        swprintf_s(dest, MAX_PATH, L"%s%s", mount_root, rel_dest);

        /* Extract just the filename for status */
        filename = wcsrchr(rel_dest, L'\\');
        filename = filename ? filename + 1 : rel_dest;

        count++;
        if (total > 0) {
            int pct = (count * 100) / total;
            log_progress(pct, L"Staging files...");
        }

        /* Create parent directories */
        ensure_parent_dirs(dest);

        log_msg(L"Copying: %s -> %s", src, dest);
        if (!CopyFileW(src, dest, FALSE)) {
            DWORD err = GetLastError();
            /* Only fatal for core files (unattend, setup scripts, agent).
               GPU/driver files are best-effort since Plan9 shares provide a fallback. */
            if (wcsstr(rel_dest, L"HostDriverStore") != NULL ||
                wcsstr(rel_dest, L"hostdriverstore") != NULL) {
                log_msg(L"Warning: skipped %s (error %lu)", filename, err);
                skipped++;
            } else {
                log_err(L"Failed to copy %s -> %s (error %lu)", src, dest, err);
                fclose(f);
                return -1;
            }
        }
    }

    fclose(f);
    if (skipped > 0)
        log_msg(L"Warning: %d GPU driver file(s) skipped (will be copied by agent at boot)", skipped);
    return count;
}

static int do_to_vhdx(const wchar_t *iso_path_arg, int image_index, int size_gb,
                       const wchar_t *manifest_path, const wchar_t *output_path_arg)
{
    int exit_code = 1;
    DWORD result;
    wchar_t iso_path[MAX_PATH];
    wchar_t vhdx_path[MAX_PATH];

    /* ISO mount state */
    HANDLE iso_handle = INVALID_HANDLE_VALUE;
    wchar_t iso_drive = 0;
    DWORD cdrom_before = 0;
    wchar_t iso_root[4];

    /* VHDX state */
    HANDLE vhdx_handle = INVALID_HANDLE_VALUE;
    HANDLE disk_handle = INVALID_HANDLE_VALUE;
    wchar_t phys_path[MAX_PATH];
    DWORD disk_number = 0;

    /* Mount points */
    wchar_t efi_mount[MAX_PATH] = {0};
    wchar_t win_mount[MAX_PATH] = {0};
    BOOL efi_mounted = FALSE;
    BOOL win_mounted = FALSE;

    /* WIM/ESD path found on ISO */
    wchar_t wim_path[MAX_PATH];

    /* Resolve ISO path */
    if (!GetFullPathNameW(iso_path_arg, MAX_PATH, iso_path, NULL)) {
        log_err(L"Invalid path: %s", iso_path_arg);
        return 1;
    }
    if (GetFileAttributesW(iso_path) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"File not found: %s", iso_path);
        return 1;
    }

    if (output_path_arg) {
        if (!GetFullPathNameW(output_path_arg, MAX_PATH, vhdx_path, NULL)) {
            log_err(L"Invalid output path: %s", output_path_arg);
            return 1;
        }
        ensure_parent_dirs(vhdx_path);
    } else {
        build_output_path(vhdx_path, MAX_PATH, iso_path, L".vhdx");
    }

    /* ---- Step 1: Mount the ISO ---- */
    log_msg(L"Mounting ISO...");
    {
        VIRTUAL_STORAGE_TYPE st;
        OPEN_VIRTUAL_DISK_PARAMETERS op;

        ZeroMemory(&st, sizeof(st));
        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;

        ZeroMemory(&op, sizeof(op));
        op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

        cdrom_before = snapshot_cdrom_drives();

        result = OpenVirtualDisk(&st, iso_path,
                                  VIRTUAL_DISK_ACCESS_READ,
                                  OPEN_VIRTUAL_DISK_FLAG_NONE,
                                  &op, &iso_handle);
        if (result != ERROR_SUCCESS) {
            log_err(L"OpenVirtualDisk(ISO) failed (error %lu)", result);
            goto cleanup;
        }

        result = AttachVirtualDisk(iso_handle, NULL,
                                    ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                                    0, NULL, NULL);
        if (result != ERROR_SUCCESS) {
            log_err(L"AttachVirtualDisk(ISO) failed (error %lu)", result);
            goto cleanup;
        }

        /* Wait for drive letter */
        for (int i = 0; i < 20; i++) {
            DWORD cdrom_now = snapshot_cdrom_drives();
            DWORD new_drives = cdrom_now & ~cdrom_before;
            if (new_drives) {
                for (int b = 0; b < 26; b++) {
                    if (new_drives & (1u << b)) {
                        iso_drive = (wchar_t)(L'A' + b);
                        break;
                    }
                }
                break;
            }
            Sleep(500);
        }
        if (!iso_drive) {
            log_err(L"Mounted ISO but no drive letter appeared");
            goto cleanup;
        }
    }
    swprintf_s(iso_root, 4, L"%c:\\", iso_drive);

    /* Wait for filesystem to be accessible */
    {
        wchar_t label[64];
        int ready = 0;
        for (int i = 0; i < 30; i++) {
            if (GetVolumeInformationW(iso_root, label, 64, NULL, NULL, NULL, NULL, 0)) {
                ready = 1;
                break;
            }
            Sleep(500);
        }
        if (!ready) {
            log_err(L"ISO volume never became accessible");
            goto cleanup;
        }
    }

    /* ---- Step 2: Find install.wim or install.esd ---- */
    swprintf_s(wim_path, MAX_PATH, L"%c:\\sources\\install.wim", iso_drive);
    if (GetFileAttributesW(wim_path) == INVALID_FILE_ATTRIBUTES) {
        swprintf_s(wim_path, MAX_PATH, L"%c:\\sources\\install.esd", iso_drive);
        if (GetFileAttributesW(wim_path) == INVALID_FILE_ATTRIBUTES) {
            log_err(L"Neither install.wim nor install.esd found on ISO");
            goto cleanup;
        }
    }
    {
        const wchar_t *wim_name = wcsrchr(wim_path, L'\\');
        wim_name = wim_name ? wim_name + 1 : wim_path;
        log_msg(L"Found %s", wim_name);
    }

    /* ---- Step 2.5: Detect image language ---- */
    {
        wchar_t dism_cmd[1024];
        char dism_output[8192];
        wchar_t sys_dir[MAX_PATH];
        const char *detected_lang = "en-US";  /* fallback */
        GetSystemDirectoryW(sys_dir, MAX_PATH);
        swprintf_s(dism_cmd, 1024,
            L"%s\\dism.exe /English /Get-WimInfo /WimFile:\"%s\" /Index:%d",
            sys_dir, wim_path, image_index);
        if (run_command_capture(dism_cmd, dism_output, sizeof(dism_output)) == 0) {
            /* Look for "xx-YY (Default)" pattern in DISM output */
            char *p = strstr(dism_output, "(Default)");
            if (p) {
                /* Walk backwards to find the language tag */
                char *end = p;
                while (end > dism_output && *(end - 1) == ' ') end--;
                if (end > dism_output) {
                    char *start = end;
                    while (start > dism_output && *(start - 1) != ' ' &&
                           *(start - 1) != '\n' && *(start - 1) != '\r' &&
                           *(start - 1) != '\t')
                        start--;
                    if (start < end) {
                        *end = '\0';
                        detected_lang = start;
                    }
                }
            }
        }
        printf("LANG:%s\n", detected_lang);
        fflush(stdout);
    }

    /* ---- Step 3: Create VHDX ---- */
    log_msg(L"Creating %d GB VHDX...", size_gb);
    {
        VIRTUAL_STORAGE_TYPE st;
        CREATE_VIRTUAL_DISK_PARAMETERS params;

        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
        st.VendorId = VHDX_VENDOR_MS;

        ZeroMemory(&params, sizeof(params));
        params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
        params.Version2.MaximumSize = (ULONGLONG)size_gb * 1024ULL * 1024ULL * 1024ULL;
        params.Version2.BlockSizeInBytes = 0;
        params.Version2.SectorSizeInBytes = 512;
        params.Version2.PhysicalSectorSizeInBytes = 4096;

        result = CreateVirtualDisk(&st, vhdx_path,
                                    VIRTUAL_DISK_ACCESS_NONE,
                                    NULL,
                                    CREATE_VIRTUAL_DISK_FLAG_NONE,
                                    0, &params, NULL, &vhdx_handle);
        if (result != ERROR_SUCCESS) {
            log_err(L"CreateVirtualDisk failed (error %lu)", result);
            goto cleanup;
        }
    }
    /* ---- Step 4: Attach VHDX ---- */
    {
        ATTACH_VIRTUAL_DISK_PARAMETERS ap;
        ZeroMemory(&ap, sizeof(ap));
        ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

        result = AttachVirtualDisk(vhdx_handle, NULL,
                                    ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
                                    0, &ap, NULL);
        if (result != ERROR_SUCCESS) {
            log_err(L"AttachVirtualDisk(VHDX) failed (error %lu)", result);
            goto cleanup;
        }
    }

    /* Get physical path */
    {
        DWORD phys_len = sizeof(phys_path);
        result = GetVirtualDiskPhysicalPath(vhdx_handle, &phys_len, phys_path);
        if (result != ERROR_SUCCESS) {
            log_err(L"GetVirtualDiskPhysicalPath failed (error %lu)", result);
            goto cleanup;
        }
        disk_number = get_disk_number_from_path(phys_path);
    }

    /* Open the physical disk */
    disk_handle = CreateFileW(phys_path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (disk_handle == INVALID_HANDLE_VALUE) {
        log_err(L"Failed to open disk %s (error %lu)", phys_path, GetLastError());
        goto cleanup;
    }

    /* ---- Step 5: Initialize GPT and create partitions ---- */
    log_msg(L"Partitioning disk...");
    {
        CREATE_DISK cd;
        DWORD bytes;

        ZeroMemory(&cd, sizeof(cd));
        cd.PartitionStyle = PARTITION_STYLE_GPT;
        /* Let Windows generate a disk GUID */
        CoCreateGuid(&cd.Gpt.DiskId);

        if (!DeviceIoControl(disk_handle, IOCTL_DISK_CREATE_DISK,
                              &cd, sizeof(cd), NULL, 0, &bytes, NULL)) {
            log_err(L"IOCTL_DISK_CREATE_DISK failed (error %lu)", GetLastError());
            goto cleanup;
        }
    }

    /* Refresh partition table */
    {
        DWORD bytes;
        DeviceIoControl(disk_handle, IOCTL_DISK_UPDATE_PROPERTIES,
                         NULL, 0, NULL, 0, &bytes, NULL);
    }

    /* Create EFI, MSR, and Windows partitions */
    {
        DWORD bytes;
        DWORD layout_size;
        DRIVE_LAYOUT_INFORMATION_EX *layout;
        PARTITION_INFORMATION_EX *p;
        LARGE_INTEGER efi_size, msr_size;
        LARGE_INTEGER efi_offset;
        GET_LENGTH_INFORMATION leninfo;

        /* Get total disk size */
        if (!DeviceIoControl(disk_handle, IOCTL_DISK_GET_LENGTH_INFO,
                              NULL, 0, &leninfo, sizeof(leninfo), &bytes, NULL)) {
            log_err(L"IOCTL_DISK_GET_LENGTH_INFO failed (error %lu)", GetLastError());
            goto cleanup;
        }
        efi_size.QuadPart = (LONGLONG)EFI_SIZE_MB * 1024LL * 1024LL;
        msr_size.QuadPart = (LONGLONG)MSR_SIZE_MB * 1024LL * 1024LL;
        efi_offset.QuadPart = 1024LL * 1024LL; /* 1MB offset (standard GPT alignment) */

        /* Allocate layout buffer for 3 partitions */
        layout_size = (DWORD)(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                       3 * sizeof(PARTITION_INFORMATION_EX));
        layout = (DRIVE_LAYOUT_INFORMATION_EX *)calloc(1, layout_size);
        if (!layout) {
            log_err(L"Out of memory");
            goto cleanup;
        }

        layout->PartitionStyle = PARTITION_STYLE_GPT;
        layout->PartitionCount = 3;
        CoCreateGuid(&layout->Gpt.DiskId);

        /* Partition 1: EFI System Partition */
        p = &layout->PartitionEntry[0];
        p->PartitionStyle = PARTITION_STYLE_GPT;
        p->StartingOffset.QuadPart = efi_offset.QuadPart;
        p->PartitionLength.QuadPart = efi_size.QuadPart;
        p->PartitionNumber = 1;
        /* PARTITION_SYSTEM_GUID = {c12a7328-f81f-11d2-ba4b-00a0c93ec93b} */
        p->Gpt.PartitionType.Data1 = 0xc12a7328;
        p->Gpt.PartitionType.Data2 = 0xf81f;
        p->Gpt.PartitionType.Data3 = 0x11d2;
        p->Gpt.PartitionType.Data4[0] = 0xba; p->Gpt.PartitionType.Data4[1] = 0x4b;
        p->Gpt.PartitionType.Data4[2] = 0x00; p->Gpt.PartitionType.Data4[3] = 0xa0;
        p->Gpt.PartitionType.Data4[4] = 0xc9; p->Gpt.PartitionType.Data4[5] = 0x3e;
        p->Gpt.PartitionType.Data4[6] = 0xc9; p->Gpt.PartitionType.Data4[7] = 0x3b;
        CoCreateGuid(&p->Gpt.PartitionId);
        wcscpy_s(p->Gpt.Name, 36, L"EFI System Partition");

        /* Partition 2: Microsoft Reserved */
        p = &layout->PartitionEntry[1];
        p->PartitionStyle = PARTITION_STYLE_GPT;
        p->StartingOffset.QuadPart = efi_offset.QuadPart + efi_size.QuadPart;
        p->PartitionLength.QuadPart = msr_size.QuadPart;
        p->PartitionNumber = 2;
        /* PARTITION_MSFT_RESERVED_GUID = {e3c9e316-0b5c-4db8-817d-f92df00215ae} */
        p->Gpt.PartitionType.Data1 = 0xe3c9e316;
        p->Gpt.PartitionType.Data2 = 0x0b5c;
        p->Gpt.PartitionType.Data3 = 0x4db8;
        p->Gpt.PartitionType.Data4[0] = 0x81; p->Gpt.PartitionType.Data4[1] = 0x7d;
        p->Gpt.PartitionType.Data4[2] = 0xf9; p->Gpt.PartitionType.Data4[3] = 0x2d;
        p->Gpt.PartitionType.Data4[4] = 0xf0; p->Gpt.PartitionType.Data4[5] = 0x02;
        p->Gpt.PartitionType.Data4[6] = 0x15; p->Gpt.PartitionType.Data4[7] = 0xae;
        CoCreateGuid(&p->Gpt.PartitionId);

        /* Partition 3: Windows (Basic Data) — uses remaining space minus 1MB at end */
        p = &layout->PartitionEntry[2];
        p->PartitionStyle = PARTITION_STYLE_GPT;
        p->StartingOffset.QuadPart = efi_offset.QuadPart + efi_size.QuadPart + msr_size.QuadPart;
        p->PartitionLength.QuadPart = leninfo.Length.QuadPart - p->StartingOffset.QuadPart - 1024LL * 1024LL;
        p->PartitionNumber = 3;
        /* PARTITION_BASIC_DATA_GUID = {ebd0a0a2-b9e5-4433-87c0-68b6b72699c7} */
        p->Gpt.PartitionType.Data1 = 0xebd0a0a2;
        p->Gpt.PartitionType.Data2 = 0xb9e5;
        p->Gpt.PartitionType.Data3 = 0x4433;
        p->Gpt.PartitionType.Data4[0] = 0x87; p->Gpt.PartitionType.Data4[1] = 0xc0;
        p->Gpt.PartitionType.Data4[2] = 0x68; p->Gpt.PartitionType.Data4[3] = 0xb6;
        p->Gpt.PartitionType.Data4[4] = 0xb7; p->Gpt.PartitionType.Data4[5] = 0x26;
        p->Gpt.PartitionType.Data4[6] = 0x99; p->Gpt.PartitionType.Data4[7] = 0xc7;
        CoCreateGuid(&p->Gpt.PartitionId);

        if (!DeviceIoControl(disk_handle, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                              layout, layout_size, NULL, 0, &bytes, NULL)) {
            log_err(L"IOCTL_DISK_SET_DRIVE_LAYOUT_EX failed (error %lu)", GetLastError());
            free(layout);
            goto cleanup;
        }
        free(layout);
    }

    /* Refresh again after partitioning */
    {
        DWORD bytes;
        DeviceIoControl(disk_handle, IOCTL_DISK_UPDATE_PROPERTIES,
                         NULL, 0, NULL, 0, &bytes, NULL);
    }

    /* Give the OS time to enumerate the new volumes */
    Sleep(2000);

    /* ---- Step 6: Find and mount the EFI and Windows volumes ---- */
    log_msg(L"Formatting partitions...");
    {
        wchar_t efi_vol[MAX_PATH];
        if (!find_volume_for_partition(disk_number, 1, efi_vol, MAX_PATH)) {
            log_err(L"Could not find EFI partition volume");
            goto cleanup;
        }
        if (!create_temp_mount_dir(L"asb_efi", efi_mount, MAX_PATH)) {
            log_err(L"Failed to create temp mount directory");
            goto cleanup;
        }
        if (!mount_volume_to_dir(efi_vol, efi_mount)) {
            log_err(L"Failed to mount EFI partition (error %lu)", GetLastError());
            goto cleanup;
        }
        efi_mounted = TRUE;
    }

    {
        wchar_t win_vol[MAX_PATH];
        if (!find_volume_for_partition(disk_number, 3, win_vol, MAX_PATH)) {
            log_err(L"Could not find Windows partition volume");
            goto cleanup;
        }
        if (!create_temp_mount_dir(L"asb_win", win_mount, MAX_PATH)) {
            log_err(L"Failed to create temp mount directory");
            goto cleanup;
        }
        if (!mount_volume_to_dir(win_vol, win_mount)) {
            log_err(L"Failed to mount Windows partition (error %lu)", GetLastError());
            goto cleanup;
        }
        win_mounted = TRUE;
    }

    /* ---- Step 7: Format partitions ---- */
    if (!format_volume(efi_mount, L"FAT32", L"SYSTEM")) {
        log_err(L"Failed to format EFI partition");
        goto cleanup;
    }
    if (!format_volume(win_mount, L"NTFS", L"Windows")) {
        log_err(L"Failed to format Windows partition");
        goto cleanup;
    }

    /* ---- Step 8: Apply WIM/ESD image using dism.exe ---- */
    {
        wchar_t cmd[1024];
        wchar_t sys_dir[MAX_PATH];
        wchar_t win_clean[MAX_PATH];
        int ret;

        GetSystemDirectoryW(sys_dir, MAX_PATH);

        /* Strip trailing backslash from win_mount for dism */
        wcscpy_s(win_clean, MAX_PATH, win_mount);
        {
            size_t wl = wcslen(win_clean);
            if (wl > 0 && win_clean[wl - 1] == L'\\')
                win_clean[wl - 1] = L'\0';
        }

        swprintf_s(cmd, 1024,
                    L"%s\\dism.exe /Apply-Image /ImageFile:\"%s\" /Index:%d /ApplyDir:\"%s\"",
                    sys_dir, wim_path, image_index, win_clean);

        ret = run_command_with_progress(cmd, L"Applying Windows image...");
        if (ret != 0) {
            log_err(L"Failed to apply Windows image (dism exit code %d)", ret);
            goto cleanup;
        }
    }

    /* ---- Step 9: Stage additional files from manifest ---- */
    if (manifest_path) {
        log_msg(L"Staging files...");
        {
            int staged = stage_files(manifest_path, win_mount);
            if (staged < 0) {
                log_err(L"File staging failed");
                goto cleanup;
            }
            if (staged > 0)
                log_msg(L"Staged %d file(s)", staged);
        }
    }

    /* ---- Step 10: Set up UEFI boot files with bcdboot ---- */
    log_msg(L"Installing boot files...");
    {
        wchar_t windows_dir[MAX_PATH];
        wchar_t efi_clean[MAX_PATH];
        wchar_t cmd[1024];
        wchar_t sys_dir[MAX_PATH];
        int ret;

        GetSystemDirectoryW(sys_dir, MAX_PATH);
        swprintf_s(windows_dir, MAX_PATH, L"%sWindows", win_mount);

        /* Strip trailing backslash from efi_mount for bcdboot */
        wcscpy_s(efi_clean, MAX_PATH, efi_mount);
        {
            size_t el = wcslen(efi_clean);
            if (el > 0 && efi_clean[el - 1] == L'\\')
                efi_clean[el - 1] = L'\0';
        }

        swprintf_s(cmd, 1024,
                    L"%s\\bcdboot.exe \"%s\" /s \"%s\" /f UEFI",
                    sys_dir, windows_dir, efi_clean);

        ret = run_command(cmd, TRUE);
        if (ret != 0) {
            log_err(L"Failed to install boot files (bcdboot exit code %d)", ret);
            goto cleanup;
        }
    }

    log_done(vhdx_path);
    exit_code = 0;

cleanup:
    /* Unmount volumes */
    if (win_mounted) {
        unmount_volume_from_dir(win_mount);
        RemoveDirectoryW(win_mount);
    }
    if (efi_mounted) {
        unmount_volume_from_dir(efi_mount);
        RemoveDirectoryW(efi_mount);
    }
    /* Close disk handle before detaching */
    if (disk_handle != INVALID_HANDLE_VALUE)
        CloseHandle(disk_handle);

    /* Detach and close VHDX */
    if (vhdx_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(vhdx_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(vhdx_handle);
    }

    /* Detach and close ISO */
    if (iso_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(iso_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(iso_handle);
    }

    /* If we failed, delete the partial VHDX */
    if (exit_code != 0 && vhdx_path[0]) {
        DeleteFileW(vhdx_path);
    }

    return exit_code;
}

/* ======================================================================
 *  Noprompt ISO rebuild (original functionality)
 * ====================================================================== */

static int do_noprompt(const wchar_t *iso_path_arg)
{
    HRESULT hr;
    DWORD result;
    int exit_code = 1;

    /* State that needs cleanup */
    HANDLE iso_handle = INVALID_HANDLE_VALUE;
    wchar_t iso_drive = 0;
    DWORD cdrom_before = 0;
    IFileSystemImage *pImage = NULL;
    IFsiDirectoryItem *pRoot = NULL;
    IFsiDirectoryItem *pBootDir = NULL;
    IBootOptions *pBootOpts = NULL;
    IStream *pBootStream = NULL;
    IStream *pCdbootStream = NULL;
    IFileSystemImageResult *pResult = NULL;
    IStream *pOutStream = NULL;

    /* Paths */
    wchar_t iso_path[MAX_PATH];
    wchar_t output_path[MAX_PATH];
    wchar_t drive_root[4];
    wchar_t noprompt_efisys[MAX_PATH];
    wchar_t noprompt_cdboot[MAX_PATH];
    wchar_t vol_label[MAX_PATH];

    /* Resolve full path */
    if (!GetFullPathNameW(iso_path_arg, MAX_PATH, iso_path, NULL)) {
        log_err(L"Invalid path: %s", iso_path_arg);
        return 1;
    }
    if (GetFileAttributesW(iso_path) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"File not found: %s", iso_path);
        return 1;
    }

    build_output_path(output_path, MAX_PATH, iso_path, L"_noprompt.iso");

    /* ---- Step 1: Initialize COM ---- */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        log_err(L"CoInitializeEx failed (0x%08X)", hr);
        return 1;
    }

    /* ---- Step 2: Mount the source ISO ---- */
    log_msg(L"Mounting %s...", iso_path);
    {
        VIRTUAL_STORAGE_TYPE st;
        OPEN_VIRTUAL_DISK_PARAMETERS op;

        ZeroMemory(&st, sizeof(st));
        st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;

        ZeroMemory(&op, sizeof(op));
        op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

        cdrom_before = snapshot_cdrom_drives();
        log_msg(L"CDROM drives before mount: 0x%08X", cdrom_before);

        result = OpenVirtualDisk(&st, iso_path,
                                  VIRTUAL_DISK_ACCESS_READ,
                                  OPEN_VIRTUAL_DISK_FLAG_NONE,
                                  &op, &iso_handle);
        if (result != ERROR_SUCCESS) {
            log_err(L"OpenVirtualDisk failed (error %lu)", result);
            goto cleanup;
        }
        log_msg(L"OpenVirtualDisk succeeded");

        result = AttachVirtualDisk(iso_handle, NULL,
                                    ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                                    0, NULL, NULL);
        if (result != ERROR_SUCCESS) {
            log_err(L"AttachVirtualDisk failed (error %lu)", result);
            goto cleanup;
        }
        log_msg(L"AttachVirtualDisk succeeded");

        /* Get the physical path assigned by the system */
        {
            wchar_t phys_path[MAX_PATH];
            DWORD phys_len = sizeof(phys_path);
            result = GetVirtualDiskPhysicalPath(iso_handle, &phys_len, phys_path);
            if (result == ERROR_SUCCESS)
                log_msg(L"Physical path: %s", phys_path);
            else
                log_msg(L"GetVirtualDiskPhysicalPath failed (error %lu)", result);
        }

        /* Wait for the drive letter to appear */
        for (int i = 0; i < 20; i++) {
            DWORD cdrom_now = snapshot_cdrom_drives();
            DWORD new_drives = cdrom_now & ~cdrom_before;
            if (i == 0 || (i % 4 == 0))
                log_msg(L"Poll %d: CDROM drives now: 0x%08X, new: 0x%08X", i, cdrom_now, new_drives);
            if (new_drives) {
                for (int b = 0; b < 26; b++) {
                    if (new_drives & (1u << b)) {
                        iso_drive = (wchar_t)(L'A' + b);
                        break;
                    }
                }
                break;
            }
            Sleep(500);
        }
        if (!iso_drive) {
            wchar_t root[4] = L"A:\\";
            for (int i = 0; i < 26; i++) {
                root[0] = (wchar_t)(L'A' + i);
                UINT dt = GetDriveTypeW(root);
                if (dt != DRIVE_NO_ROOT_DIR)
                    log_msg(L"  %c: type=%u", (wchar_t)(L'A' + i), dt);
            }
            log_err(L"Mounted ISO but no new drive letter appeared");
            goto cleanup;
        }
    }

    swprintf_s(drive_root, 4, L"%c:\\", iso_drive);
    log_msg(L"Mounted at %c:\\", iso_drive);

    /* Wait for the filesystem to become accessible */
    {
        int ready = 0;
        wchar_t test_label[64];
        for (int i = 0; i < 30; i++) {
            if (GetVolumeInformationW(drive_root, test_label, 64, NULL, NULL, NULL, NULL, 0)) {
                ready = 1;
                break;
            }
            log_msg(L"Waiting for volume to be ready... (attempt %d, error %lu)", i + 1, GetLastError());
            Sleep(500);
        }
        if (!ready) {
            log_err(L"Volume %c:\\ never became accessible", iso_drive);
            goto cleanup;
        }
    }

    /* ---- Step 3: Verify noprompt files ---- */
    swprintf_s(noprompt_efisys, MAX_PATH, L"%c:\\efi\\microsoft\\boot\\efisys_noprompt.bin", iso_drive);
    swprintf_s(noprompt_cdboot, MAX_PATH, L"%c:\\efi\\microsoft\\boot\\cdboot_noprompt.efi", iso_drive);

    log_msg(L"Checking: %s", noprompt_efisys);
    if (GetFileAttributesW(noprompt_efisys) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"efisys_noprompt.bin not found on ISO (error %lu)", GetLastError());
        goto cleanup;
    }
    log_msg(L"Checking: %s", noprompt_cdboot);
    if (GetFileAttributesW(noprompt_cdboot) == INVALID_FILE_ATTRIBUTES) {
        log_err(L"cdboot_noprompt.efi not found on ISO (error %lu)", GetLastError());
        goto cleanup;
    }
    log_msg(L"Found efisys_noprompt.bin and cdboot_noprompt.efi");

    /* ---- Step 4: Read volume label ---- */
    vol_label[0] = L'\0';
    GetVolumeInformationW(drive_root, vol_label, MAX_PATH, NULL, NULL, NULL, NULL, 0);
    if (vol_label[0])
        log_msg(L"Volume label: %s", vol_label);

    /* ---- Step 5: Create IMAPI2 IFileSystemImage ---- */
    hr = CoCreateInstance(&CLSID_FSImage, NULL, CLSCTX_ALL,
                          &IID_IFSImage, (void **)&pImage);
    if (FAILED(hr)) {
        log_err(L"CoCreateInstance(MsftFileSystemImage) failed (0x%08X)", hr);
        goto cleanup;
    }

    pImage->lpVtbl->put_FileSystemsToCreate(pImage, FsiFileSystemUDF);
    pImage->lpVtbl->put_UDFRevision(pImage, 0x0150);
    pImage->lpVtbl->put_FreeMediaBlocks(pImage, 4194304);

    if (vol_label[0]) {
        BSTR bstrLabel = SysAllocString(vol_label);
        pImage->lpVtbl->put_VolumeName(pImage, bstrLabel);
        SysFreeString(bstrLabel);
    }

    /* ---- Step 6: Set UEFI boot image ---- */
    hr = SHCreateStreamOnFileW(noprompt_efisys, STGM_READ | STGM_SHARE_DENY_WRITE,
                                &pBootStream);
    if (FAILED(hr)) {
        log_err(L"SHCreateStreamOnFileW(efisys_noprompt.bin) failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = CoCreateInstance(&CLSID_BootOpts, NULL, CLSCTX_ALL,
                          &IID_IBootOpts, (void **)&pBootOpts);
    if (FAILED(hr)) {
        log_err(L"CoCreateInstance(BootOptions) failed (0x%08X)", hr);
        goto cleanup;
    }

    pBootOpts->lpVtbl->AssignBootImage(pBootOpts, pBootStream);
    pBootOpts->lpVtbl->put_PlatformId(pBootOpts, 0xEF);
    pBootOpts->lpVtbl->put_Emulation(pBootOpts, 0);

    hr = pImage->lpVtbl->put_BootImageOptions(pImage, pBootOpts);
    if (FAILED(hr)) {
        log_err(L"put_BootImageOptions failed (0x%08X)", hr);
        goto cleanup;
    }

    /* ---- Step 7: Import source ISO file tree ---- */
    log_msg(L"Importing files from source ISO...");
    hr = pImage->lpVtbl->get_Root(pImage, &pRoot);
    if (FAILED(hr)) {
        log_err(L"get_Root failed (0x%08X)", hr);
        goto cleanup;
    }

    {
        BSTR bstrDrive = SysAllocString(drive_root);
        hr = pRoot->lpVtbl->AddTree(pRoot, bstrDrive, VARIANT_FALSE);
        SysFreeString(bstrDrive);
    }
    if (FAILED(hr)) {
        log_err(L"AddTree failed (0x%08X)", hr);
        goto cleanup;
    }

    /* ---- Step 8: Swap boot files ---- */
    log_msg(L"Swapping boot files with noprompt variants...");
    hr = navigate_fsi_path(pRoot, &pBootDir,
                           L"efi", L"microsoft", L"boot", NULL);
    if (FAILED(hr)) {
        log_err(L"Failed to navigate to \\efi\\microsoft\\boot\\ (0x%08X)", hr);
        goto cleanup;
    }

    {
        BSTR name;
        name = SysAllocString(L"efisys.bin");
        hr = pBootDir->lpVtbl->Remove(pBootDir, name);
        SysFreeString(name);
        if (FAILED(hr)) {
            log_err(L"Failed to remove efisys.bin from tree (0x%08X)", hr);
            goto cleanup;
        }

        name = SysAllocString(L"cdboot.efi");
        hr = pBootDir->lpVtbl->Remove(pBootDir, name);
        SysFreeString(name);
        if (FAILED(hr)) {
            log_err(L"Failed to remove cdboot.efi from tree (0x%08X)", hr);
            goto cleanup;
        }
    }

    {
        BSTR name;
        IStream *stream = NULL;

        hr = SHCreateStreamOnFileW(noprompt_efisys, STGM_READ | STGM_SHARE_DENY_WRITE, &stream);
        if (FAILED(hr)) {
            log_err(L"SHCreateStreamOnFileW(efisys_noprompt) failed (0x%08X)", hr);
            goto cleanup;
        }
        name = SysAllocString(L"efisys.bin");
        hr = pBootDir->lpVtbl->AddFile(pBootDir, name, stream);
        SysFreeString(name);
        stream->lpVtbl->Release(stream);
        stream = NULL;
        if (FAILED(hr)) {
            log_err(L"AddFile(efisys.bin) failed (0x%08X)", hr);
            goto cleanup;
        }

        hr = SHCreateStreamOnFileW(noprompt_cdboot, STGM_READ | STGM_SHARE_DENY_WRITE, &stream);
        if (FAILED(hr)) {
            log_err(L"SHCreateStreamOnFileW(cdboot_noprompt) failed (0x%08X)", hr);
            goto cleanup;
        }
        name = SysAllocString(L"cdboot.efi");
        hr = pBootDir->lpVtbl->AddFile(pBootDir, name, stream);
        SysFreeString(name);
        stream->lpVtbl->Release(stream);
        stream = NULL;
        if (FAILED(hr)) {
            log_err(L"AddFile(cdboot.efi) failed (0x%08X)", hr);
            goto cleanup;
        }
    }

    /* ---- Step 9: Build and write the patched ISO ---- */
    log_msg(L"Building patched ISO (this may take several minutes)...");
    hr = pImage->lpVtbl->CreateResultImage(pImage, &pResult);
    if (FAILED(hr)) {
        log_err(L"CreateResultImage failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = pResult->lpVtbl->get_ImageStream(pResult, &pOutStream);
    if (FAILED(hr)) {
        log_err(L"get_ImageStream failed (0x%08X)", hr);
        goto cleanup;
    }

    log_msg(L"Writing %s...", output_path);
    hr = write_stream_to_file(pOutStream, output_path);
    if (FAILED(hr)) {
        log_err(L"write_stream_to_file failed (0x%08X)", hr);
        goto cleanup;
    }

    log_msg(L"Done.");
    exit_code = 0;

cleanup:
    if (pOutStream)    pOutStream->lpVtbl->Release(pOutStream);
    if (pResult)       pResult->lpVtbl->Release(pResult);
    if (pCdbootStream) pCdbootStream->lpVtbl->Release(pCdbootStream);
    if (pBootStream)   pBootStream->lpVtbl->Release(pBootStream);
    if (pBootOpts)     pBootOpts->lpVtbl->Release(pBootOpts);
    if (pBootDir)      pBootDir->lpVtbl->Release(pBootDir);
    if (pRoot)         pRoot->lpVtbl->Release(pRoot);
    if (pImage)        pImage->lpVtbl->Release(pImage);

    if (iso_handle != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(iso_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(iso_handle);
    }

    CoUninitialize();
    return exit_code;
}

/* ======================================================================
 *  --qcow2-to-vhdx: convert an Ubuntu qcow2 cloud image to a bootable VHDX
 * ======================================================================
 *
 *  Pipeline: qcow2 -> raw disk image -> append VHD-fixed footer -> use
 *  Microsoft's VirtDisk API (CreateVirtualDisk SourcePath) to copy the
 *  VHD into a fresh dynamic VHDX. Output is a Hyper-V Gen 2 boot-capable
 *  image suitable for differencing children.
 *
 *  Lives in iso-patch.exe (not the host DLL) so the format-conversion
 *  work runs out-of-process: process isolation, separate progress pipe,
 *  cleaner debugging. Run by `disk_util.c::ensure_ubuntu_cloud_image_cached`
 *  after it downloads the qcow2.
 * ====================================================================== */

/* Emit a DEBUG line on stdout — recognized by the disk_util.c parser
   alongside STATUS/PROGRESS/ERROR/DONE. Use for fine-grained diagnostics
   (inflate state, qcow2 table walks, etc.). */
static void log_debug(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wprintf(L"DEBUG:");
    vwprintf(fmt, ap);
    wprintf(L"\n");
    fflush(stdout);
    va_end(ap);
}

/* Enable a Windows token privilege by name (e.g. SE_MANAGE_VOLUME_NAME).
   Admin tokens have these privileges present-but-disabled by default;
   AttachVirtualDisk on a freshly-created VHDX requires SE_MANAGE_VOLUME
   to actually be ENABLED. AppSandbox.exe gets it implicitly via Hyper-V
   Administrators membership, but iso-patch.exe spawned from a vanilla
   elevated cmd doesn't. Call this at startup; it's a no-op if the
   privilege isn't in the token at all. */
static BOOL enable_privilege(LPCWSTR name)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    BOOL ok = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
        /* AdjustTokenPrivileges returns TRUE even when the privilege
           isn't in the token; check GetLastError() for the real story. */
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(token);
    return ok;
}

/* ---- Big-endian readers/writers (qcow2 + VHD footer are both BE) ---- */
static DWORD     read_be32(const BYTE *p) {
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) |
           ((DWORD)p[2] <<  8) |  (DWORD)p[3];
}
static ULONGLONG read_be64(const BYTE *p) {
    return ((ULONGLONG)read_be32(p) << 32) | (ULONGLONG)read_be32(p + 4);
}
static void put_be32(BYTE *p, DWORD v) {
    p[0] = (BYTE)(v >> 24); p[1] = (BYTE)(v >> 16);
    p[2] = (BYTE)(v >>  8); p[3] = (BYTE)v;
}
static void put_be64(BYTE *p, ULONGLONG v) {
    p[0] = (BYTE)(v >> 56); p[1] = (BYTE)(v >> 48);
    p[2] = (BYTE)(v >> 40); p[3] = (BYTE)(v >> 32);
    p[4] = (BYTE)(v >> 24); p[5] = (BYTE)(v >> 16);
    p[6] = (BYTE)(v >>  8); p[7] = (BYTE)v;
}

/* Read exactly `len` bytes at `offset` from `h`. */
static BOOL pread_full(HANDLE h, ULONGLONG offset, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got;
    BYTE *p = (BYTE *)buf;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    while (len > 0) {
        if (!ReadFile(h, p, len, &got, NULL) || got == 0) return FALSE;
        p += got; len -= got;
    }
    return TRUE;
}

static BOOL pwrite_full(HANDLE h, ULONGLONG offset, const void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD wrote;
    const BYTE *p = (const BYTE *)buf;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    while (len > 0) {
        if (!WriteFile(h, p, len, &wrote, NULL) || wrote == 0) return FALSE;
        p += wrote; len -= wrote;
    }
    return TRUE;
}

/* ===================== DEFLATE inflater (RFC 1951) =====================
 *
 * Standalone: caller supplies a finite output buffer (one qcow2 cluster
 * = 64 KB for our use), we decompress until either a BFINAL block ends
 * or the output is full. Strict-no-deps: no zlib, no contrib code.
 *
 * Debug logging is structured per call: when an error fires, we emit a
 * DEBUG: line tagged with the failing stage + state (block index, src
 * position, dst position, last symbol attempted, etc.) so the host-side
 * log shows where to look. Healthy paths emit one DEBUG: per block at
 * minimum to confirm progress.
 */

static const WORD kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const BYTE kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const WORD kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const BYTE kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const BYTE kCLOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

typedef struct {
    SHORT count[16];
    SHORT symbol[288];
} HuffTable;

typedef struct {
    const BYTE *src; size_t src_len; size_t src_pos;
    BYTE       *dst; size_t dst_cap; size_t dst_pos;
    DWORD       bit_buf;
    int         bit_count;
    int         error;
    int         block_idx;     /* debug: which block within this stream */
    const char *fail_stage;    /* debug: what step failed */
} InflateState;

static int infl_bits(InflateState *s, int n)
{
    int v;
    while (s->bit_count < n) {
        if (s->src_pos >= s->src_len) {
            s->error = 1;
            if (!s->fail_stage) s->fail_stage = "bits-eof";
            return -1;
        }
        s->bit_buf |= ((DWORD)s->src[s->src_pos++]) << s->bit_count;
        s->bit_count += 8;
    }
    v = (int)(s->bit_buf & (((DWORD)1 << n) - 1));
    s->bit_buf >>= n;
    s->bit_count -= n;
    return v;
}

static int infl_build(HuffTable *t, const BYTE *lens, int n)
{
    int left = 1;
    int i;
    SHORT offs[16];

    for (i = 0; i < 16; i++) t->count[i] = 0;
    for (i = 0; i < n; i++) {
        if (lens[i] >= 16) return -1;
        t->count[lens[i]]++;
    }
    if (t->count[0] == n) return 0;
    for (i = 1; i <= 15; i++) {
        left <<= 1;
        left -= t->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i+1] = offs[i] + t->count[i];
    for (i = 0; i < n; i++) {
        if (lens[i] != 0) {
            t->symbol[offs[lens[i]]++] = (SHORT)i;
        }
    }
    return 0;
}

static int infl_decode(InflateState *s, const HuffTable *t)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len <= 15; len++) {
        int bit = infl_bits(s, 1);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        if (code - t->count[len] < first) {
            return t->symbol[index + (code - first)];
        }
        index += t->count[len];
        first = (first + t->count[len]) << 1;
    }
    s->error = 1;
    if (!s->fail_stage) s->fail_stage = "decode-no-match-15bits";
    return -1;
}

static int infl_codes(InflateState *s, const HuffTable *litlen, const HuffTable *dist)
{
    int sym;
    while ((sym = infl_decode(s, litlen)) >= 0) {
        if (sym < 256) {
            if (s->dst_pos >= s->dst_cap) {
                s->error = 1; s->fail_stage = "literal-overflow";
                return -1;
            }
            s->dst[s->dst_pos++] = (BYTE)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int idx = sym - 257;
            int extra, length, dist_sym, dist_extra, distance;
            if (idx < 0 || idx >= 29) {
                s->error = 1; s->fail_stage = "bad-length-sym"; return -1;
            }
            extra = infl_bits(s, kLenExtra[idx]); if (extra < 0) return -1;
            length = kLenBase[idx] + extra;
            dist_sym = infl_decode(s, dist);
            if (dist_sym < 0 || dist_sym >= 30) {
                s->error = 1; s->fail_stage = "bad-dist-sym"; return -1;
            }
            dist_extra = infl_bits(s, kDistExtra[dist_sym]); if (dist_extra < 0) return -1;
            distance = kDistBase[dist_sym] + dist_extra;
            if ((size_t)distance > s->dst_pos) {
                s->error = 1; s->fail_stage = "dist-past-start"; return -1;
            }
            {
                size_t base = s->dst_pos - distance;
                int i;
                if (s->dst_pos + length > s->dst_cap) {
                    s->error = 1; s->fail_stage = "copy-overflow"; return -1;
                }
                for (i = 0; i < length; i++) {
                    s->dst[s->dst_pos] = s->dst[base + i];
                    s->dst_pos++;
                }
            }
        }
    }
    if (!s->fail_stage) s->fail_stage = "decode-litlen";
    return -1;
}

static int infl_stored(InflateState *s)
{
    int len, nlen;
    s->bit_buf = 0;
    s->bit_count = 0;
    if (s->src_pos + 4 > s->src_len) {
        s->error = 1; s->fail_stage = "stored-header-eof"; return -1;
    }
    len  =  s->src[s->src_pos++];
    len |= ((int)s->src[s->src_pos++]) << 8;
    nlen =  s->src[s->src_pos++];
    nlen|= ((int)s->src[s->src_pos++]) << 8;
    if ((len ^ nlen) != 0xFFFF) {
        s->error = 1; s->fail_stage = "stored-len-nlen-mismatch"; return -1;
    }
    if (s->src_pos + len > s->src_len) {
        s->error = 1; s->fail_stage = "stored-body-eof"; return -1;
    }
    if (s->dst_pos + (size_t)len > s->dst_cap) {
        s->error = 1; s->fail_stage = "stored-output-overflow"; return -1;
    }
    if (len > 0) {
        memcpy(s->dst + s->dst_pos, s->src + s->src_pos, len);
        s->src_pos += len;
        s->dst_pos += len;
    }
    return 0;
}

static int infl_fixed(InflateState *s)
{
    static HuffTable litlen, dist;
    static int built = 0;
    if (!built) {
        BYTE lens[288];
        int i;
        for (i =   0; i < 144; i++) lens[i] = 8;
        for (i = 144; i < 256; i++) lens[i] = 9;
        for (i = 256; i < 280; i++) lens[i] = 7;
        for (i = 280; i < 288; i++) lens[i] = 8;
        if (infl_build(&litlen, lens, 288) != 0) {
            s->error = 1; s->fail_stage = "fixed-litlen-build"; return -1;
        }
        for (i =   0; i <  30; i++) lens[i] = 5;
        if (infl_build(&dist,   lens,  30) != 0) {
            s->error = 1; s->fail_stage = "fixed-dist-build"; return -1;
        }
        built = 1;
    }
    return infl_codes(s, &litlen, &dist);
}

static int infl_dynamic(InflateState *s)
{
    int hlit, hdist, hclen;
    BYTE cl_lens[19];
    BYTE all_lens[288 + 30];
    HuffTable cl_table, litlen, dist;
    int i, total;

    hlit  = infl_bits(s, 5); if (hlit  < 0) return -1; hlit  += 257;
    hdist = infl_bits(s, 5); if (hdist < 0) return -1; hdist += 1;
    hclen = infl_bits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30 || hclen > 19) {
        s->error = 1; s->fail_stage = "dyn-header-out-of-range"; return -1;
    }

    for (i = 0; i < 19; i++) cl_lens[i] = 0;
    for (i = 0; i < hclen; i++) {
        int v = infl_bits(s, 3); if (v < 0) return -1;
        cl_lens[kCLOrder[i]] = (BYTE)v;
    }
    if (infl_build(&cl_table, cl_lens, 19) != 0) {
        s->error = 1; s->fail_stage = "cl-table-build"; return -1;
    }

    total = hlit + hdist;
    i = 0;
    while (i < total) {
        int sym = infl_decode(s, &cl_table);
        if (sym < 0) return -1;
        if (sym < 16) {
            all_lens[i++] = (BYTE)sym;
        } else if (sym == 16) {
            int rep = infl_bits(s, 2); if (rep < 0) return -1;
            rep += 3;
            if (i == 0) { s->error = 1; s->fail_stage = "cl-rep16-at-start"; return -1; }
            while (rep-- && i < total) { all_lens[i] = all_lens[i-1]; i++; }
        } else if (sym == 17) {
            int rep = infl_bits(s, 3); if (rep < 0) return -1;
            rep += 3;
            while (rep-- && i < total) all_lens[i++] = 0;
        } else if (sym == 18) {
            int rep = infl_bits(s, 7); if (rep < 0) return -1;
            rep += 11;
            while (rep-- && i < total) all_lens[i++] = 0;
        } else { s->error = 1; s->fail_stage = "cl-bad-symbol"; return -1; }
    }

    if (infl_build(&litlen, all_lens, hlit) != 0) {
        s->error = 1; s->fail_stage = "dyn-litlen-build"; return -1;
    }
    if (infl_build(&dist, all_lens + hlit, hdist) != 0) {
        s->error = 1; s->fail_stage = "dyn-dist-build"; return -1;
    }
    return infl_codes(s, &litlen, &dist);
}

static int inflate_deflate(BYTE *out, size_t out_max,
                            const BYTE *in, size_t in_len,
                            size_t *out_actual, int verbose)
{
    InflateState s;
    int bfinal, btype;
    s.src = in; s.src_len = in_len; s.src_pos = 0;
    s.dst = out; s.dst_cap = out_max; s.dst_pos = 0;
    s.bit_buf = 0; s.bit_count = 0; s.error = 0;
    s.block_idx = 0; s.fail_stage = NULL;

    do {
        size_t pos_before = s.src_pos;
        bfinal = infl_bits(&s, 1); if (bfinal < 0) goto fail;
        btype  = infl_bits(&s, 2); if (btype  < 0) goto fail;
        if (verbose) {
            log_debug(L"deflate block %d: bfinal=%d btype=%d src_pos=%zu dst_pos=%zu",
                      s.block_idx, bfinal, btype, pos_before, s.dst_pos);
        }
        if (btype == 0) {
            if (infl_stored(&s) != 0) goto fail;
        } else if (btype == 1) {
            if (infl_fixed(&s) != 0) goto fail;
        } else if (btype == 2) {
            if (infl_dynamic(&s) != 0) goto fail;
        } else {
            s.error = 1; s.fail_stage = "btype-3-reserved";
            goto fail;
        }
        if (s.error) goto fail;
        s.block_idx++;
    } while (!bfinal);

    if (out_actual) *out_actual = s.dst_pos;
    return 0;

fail:
    log_debug(L"deflate failed at block %d, stage=%S, src_pos=%zu/%zu, dst_pos=%zu/%zu, bits_buffered=%d",
              s.block_idx,
              s.fail_stage ? s.fail_stage : "unknown",
              s.src_pos, s.src_len, s.dst_pos, s.dst_cap, s.bit_count);
    /* Hex dump the source bytes near the failure (16 bytes before + 16 after). */
    {
        size_t start = (s.src_pos > 16) ? (s.src_pos - 16) : 0;
        size_t end   = (s.src_pos + 16 < s.src_len) ? (s.src_pos + 16) : s.src_len;
        wchar_t hex[256] = L""; size_t i; wchar_t pair[8];
        for (i = start; i < end && wcslen(hex) < 240; i++) {
            swprintf_s(pair, 8, L"%02x ", s.src[i]);
            wcscat_s(hex, 256, pair);
        }
        log_debug(L"  bytes [%zu..%zu]: %s", start, end, hex);
    }
    return -1;
}

static int inflate_zlib(BYTE *out, size_t out_max,
                         const BYTE *in, size_t in_len,
                         size_t *out_actual, int verbose)
{
    if (in_len < 2) {
        log_debug(L"zlib: stream too short (%zu bytes)", in_len);
        return -1;
    }
    if (verbose) {
        log_debug(L"zlib header: CMF=0x%02x FLG=0x%02x", in[0], in[1]);
    }
    if ((in[0] & 0x0F) != 8) {
        log_debug(L"zlib: bad CMF method (low nibble != 8)");
        return -1;
    }
    if ((in[1] & 0x20) != 0) {
        log_debug(L"zlib: FDICT set (preset dictionary not supported)");
        return -1;
    }
    if (((((int)in[0]) << 8) | in[1]) % 31 != 0) {
        log_debug(L"zlib: CMF*256+FLG not multiple of 31 (%d)",
                  (((int)in[0]) << 8) | in[1]);
        return -1;
    }
    return inflate_deflate(out, out_max, in + 2, in_len - 2, out_actual, verbose);
}

/* ===================== qcow2 -> raw =====================
 *
 * Walks L1 + L2 cluster tables, copies uncompressed clusters verbatim,
 * decompresses compressed clusters via inflate_zlib, leaves unallocated
 * clusters as sparse-file holes (NTFS).
 */

#define QCOW2_MAGIC              0x514649FBu
#define QCOW2_L1E_OFFSET_MASK    0x00FFFFFFFFFFFE00ULL
#define QCOW2_L2E_COMPRESSED_BIT 0x4000000000000000ULL
#define QCOW2_L2E_OFFSET_MASK    0x00FFFFFFFFFFFE00ULL

static HRESULT qcow2_to_raw(const wchar_t *src_path, const wchar_t *dst_path)
{
    HANDLE  hSrc = INVALID_HANDLE_VALUE;
    HANDLE  hDst = INVALID_HANDLE_VALUE;
    BYTE    header[112];
    BYTE   *l1_table = NULL;
    BYTE   *l2_table = NULL;
    BYTE   *cluster_buf = NULL;
    HRESULT hr = E_FAIL;
    DWORD   version, cluster_bits, l1_size, crypt_method;
    ULONGLONG virtual_size, l1_table_offset, incompat_features;
    DWORD   cluster_size, l2_entries;
    ULONGLONG total_clusters;
    DWORD   l2_table_bytes;
    ULONGLONG cluster_idx;
    int     compressed_seen = 0;  /* trace: only verbose-log the first few */

    hSrc = CreateFileW(src_path, GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSrc == INVALID_HANDLE_VALUE) {
        log_err(L"qcow2_to_raw: open src failed (%lu)", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!pread_full(hSrc, 0, header, sizeof(header))) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
    }
    if (read_be32(header + 0) != QCOW2_MAGIC) {
        log_err(L"qcow2_to_raw: bad magic (not a qcow2 file)"); goto cleanup;
    }
    version           = read_be32(header + 4);
    cluster_bits      = read_be32(header + 20);
    virtual_size      = read_be64(header + 24);
    crypt_method      = read_be32(header + 32);
    l1_size           = read_be32(header + 36);
    l1_table_offset   = read_be64(header + 40);
    incompat_features = (version >= 3) ? read_be64(header + 72) : 0;

    log_msg(L"qcow2 v%lu, cluster=%lu B, virtual=%llu B",
            version, 1ul << cluster_bits, virtual_size);
    log_debug(L"qcow2 header: crypt=%lu l1_size=%lu l1_off=0x%llx incompat=0x%llx",
              crypt_method, l1_size, l1_table_offset, incompat_features);
    if (version >= 3) {
        log_debug(L"qcow2 v3: header_length=%lu compression_type=%d",
                  read_be32(header + 100), header[104]);
    }

    if (version != 2 && version != 3) {
        log_err(L"qcow2_to_raw: unsupported version %lu", version); goto cleanup;
    }
    if (crypt_method != 0) {
        log_err(L"qcow2_to_raw: encrypted qcow2 not supported"); goto cleanup;
    }
    if (incompat_features != 0) {
        log_err(L"qcow2_to_raw: incompat_features=0x%llx not supported", incompat_features);
        goto cleanup;
    }
    if (cluster_bits < 9 || cluster_bits > 21) {
        log_err(L"qcow2_to_raw: cluster_bits %lu out of range", cluster_bits); goto cleanup;
    }
    cluster_size = 1u << cluster_bits;
    l2_entries   = cluster_size / 8;
    total_clusters = (virtual_size + cluster_size - 1) / cluster_size;

    {
        ULONGLONG need_l1 = (total_clusters + l2_entries - 1) / l2_entries;
        DWORD l1_bytes;
        /* l1_size is attacker-controlled (header). Reject an undersized table
           (would under-read against the cluster loop) and an oversized one
           (l1_size * 8 must not overflow the 32-bit allocation/read length;
           0x1FFFFFFF keeps l1_size*8 <= 0xFFFFFFF8). */
        if (l1_size < need_l1 || l1_size > 0x1FFFFFFFu) {
            log_err(L"qcow2_to_raw: invalid l1_size %lu (need %llu)", l1_size, need_l1);
            goto cleanup;
        }
        l1_bytes = (DWORD)((SIZE_T)l1_size * 8);
        l1_table = (BYTE *)HeapAlloc(GetProcessHeap(), 0, l1_bytes);
        if (!l1_table) { hr = E_OUTOFMEMORY; goto cleanup; }
        if (!pread_full(hSrc, l1_table_offset, l1_table, l1_bytes)) {
            log_err(L"qcow2_to_raw: read L1 table failed (%lu)", GetLastError());
            hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
        }
    }

    hDst = CreateFileW(dst_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDst == INVALID_HANDLE_VALUE) {
        log_err(L"qcow2_to_raw: create dst failed (%lu)", GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
    }
    { DWORD bytes; DeviceIoControl(hDst, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &bytes, NULL); }
    { LARGE_INTEGER li; li.QuadPart = (LONGLONG)virtual_size;
      if (!SetFilePointerEx(hDst, li, NULL, FILE_BEGIN) || !SetEndOfFile(hDst)) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
    } }

    l2_table_bytes = l2_entries * 8;
    l2_table    = (BYTE *)HeapAlloc(GetProcessHeap(), 0, l2_table_bytes);
    cluster_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cluster_size);
    if (!l2_table || !cluster_buf) { hr = E_OUTOFMEMORY; goto cleanup; }

    {
        ULONGLONG cached_l2_index = (ULONGLONG)-1;
        ULONG     last_pct = 0;

        for (cluster_idx = 0; cluster_idx < total_clusters; cluster_idx++) {
            ULONGLONG l1_idx = cluster_idx / l2_entries;
            ULONGLONG l2_idx = cluster_idx % l2_entries;
            ULONGLONG l1_entry, l2_table_off, l2_entry, cluster_off;

            if (l1_idx >= l1_size) break;

            l1_entry = read_be64(l1_table + l1_idx * 8);
            l2_table_off = l1_entry & QCOW2_L1E_OFFSET_MASK;

            if (l2_table_off == 0) {
                cluster_idx = (l1_idx + 1) * l2_entries - 1;
                continue;
            }
            if (l1_idx != cached_l2_index) {
                if (!pread_full(hSrc, l2_table_off, l2_table, l2_table_bytes)) {
                    log_err(L"qcow2_to_raw: read L2 table failed (%lu)", GetLastError());
                    hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
                }
                cached_l2_index = l1_idx;
            }

            l2_entry = read_be64(l2_table + l2_idx * 8);
            if (l2_entry == 0) continue;

            if (l2_entry & QCOW2_L2E_COMPRESSED_BIT) {
                DWORD x = 62 - (cluster_bits - 8);
                ULONGLONG comp_off    = l2_entry & (((ULONGLONG)1 << x) - 1);
                DWORD     comp_secs_m1= (DWORD)((l2_entry >> x) & (((ULONGLONG)1 << (62 - x)) - 1));
                DWORD     comp_bytes  = (comp_secs_m1 + 1) * 512u - (DWORD)(comp_off & 511);
                BYTE     *comp_buf;
                size_t    out_actual = 0;
                int       verbose = (compressed_seen < 3);  /* log first few in detail */

                if (verbose) {
                    log_debug(L"compressed cluster %llu: L2=0x%llx comp_off=0x%llx secs_m1=%lu bytes=%lu",
                              cluster_idx, l2_entry, comp_off, comp_secs_m1, comp_bytes);
                }
                compressed_seen++;

                comp_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, comp_bytes);
                if (!comp_buf) { hr = E_OUTOFMEMORY; goto cleanup; }
                if (!pread_full(hSrc, comp_off, comp_buf, comp_bytes)) {
                    DWORD err = GetLastError();
                    HeapFree(GetProcessHeap(), 0, comp_buf);
                    log_err(L"qcow2_to_raw: read compressed cluster %llu failed (%lu)", cluster_idx, err);
                    hr = HRESULT_FROM_WIN32(err); goto cleanup;
                }
                if (verbose) {
                    log_debug(L"compressed cluster %llu first 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                              cluster_idx,
                              comp_buf[0], comp_buf[1], comp_buf[2], comp_buf[3],
                              comp_buf[4], comp_buf[5], comp_buf[6], comp_buf[7],
                              comp_buf[8], comp_buf[9], comp_buf[10], comp_buf[11],
                              comp_buf[12], comp_buf[13], comp_buf[14], comp_buf[15]);
                }
                /* qcow2 calls its compression type "zlib" but QEMU actually
                   writes RAW DEFLATE streams (deflateInit2 with -MAX_WBITS,
                   no 2-byte zlib header, no Adler-32). So feed comp_buf
                   directly to inflate_deflate, NOT inflate_zlib. */
                if (inflate_deflate(cluster_buf, cluster_size, comp_buf, comp_bytes,
                                     &out_actual, verbose) != 0) {
                    HeapFree(GetProcessHeap(), 0, comp_buf);
                    log_err(L"qcow2_to_raw: inflate failed on cluster %llu", cluster_idx);
                    goto cleanup;
                }
                HeapFree(GetProcessHeap(), 0, comp_buf);
                if (verbose) {
                    log_debug(L"compressed cluster %llu inflated to %zu bytes (target %lu)",
                              cluster_idx, out_actual, cluster_size);
                }
                if (out_actual < cluster_size)
                    memset(cluster_buf + out_actual, 0, cluster_size - out_actual);
                if (!pwrite_full(hDst, cluster_idx * cluster_size, cluster_buf, cluster_size)) {
                    hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
                }
                continue;
            }

            cluster_off = l2_entry & QCOW2_L2E_OFFSET_MASK;
            if (cluster_off == 0) continue;
            if (!pread_full(hSrc, cluster_off, cluster_buf, cluster_size)) {
                log_err(L"qcow2_to_raw: read cluster %llu failed (%lu)", cluster_idx, GetLastError());
                hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
            }
            if (!pwrite_full(hDst, cluster_idx * cluster_size, cluster_buf, cluster_size)) {
                hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
            }

            {
                ULONG pct = (ULONG)((cluster_idx + 1) * 100 / total_clusters);
                if (pct >= last_pct + 5) {
                    log_progress(pct, L"Converting qcow2 to raw...");
                    last_pct = pct;
                }
            }
        }
    }

    hr = S_OK;
    log_msg(L"qcow2_to_raw: done");

cleanup:
    if (l1_table)    HeapFree(GetProcessHeap(), 0, l1_table);
    if (l2_table)    HeapFree(GetProcessHeap(), 0, l2_table);
    if (cluster_buf) HeapFree(GetProcessHeap(), 0, cluster_buf);
    if (hSrc != INVALID_HANDLE_VALUE) CloseHandle(hSrc);
    if (hDst != INVALID_HANDLE_VALUE) CloseHandle(hDst);
    return hr;
}

/* ---- VHD fixed footer + VHD -> VHDX conversion ---- */

#define VHDX_VENDOR_MS_LOCAL { 0xec984aec, 0xa0f9, 0x47e9, \
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b } }
static const GUID VHDX_VENDOR_MS_GUID = VHDX_VENDOR_MS_LOCAL;

static void vhd_compute_chs(ULONGLONG total_sectors,
                             WORD *out_cyls, BYTE *out_heads, BYTE *out_spt)
{
    BYTE heads, spt; DWORD cth; WORD cyls;
    if (total_sectors > 65535ULL * 16 * 255) total_sectors = 65535ULL * 16 * 255;
    if (total_sectors >= 65535ULL * 16 * 63) {
        spt = 255; heads = 16; cth = (DWORD)(total_sectors / spt);
    } else {
        spt = 17; cth = (DWORD)(total_sectors / spt);
        heads = (BYTE)((cth + 1023) / 1024); if (heads < 4) heads = 4;
        if (cth >= ((DWORD)heads * 1024) || heads > 16) {
            spt = 31; heads = 16; cth = (DWORD)(total_sectors / spt);
        }
        if (cth >= ((DWORD)heads * 1024)) {
            spt = 63; heads = 16; cth = (DWORD)(total_sectors / spt);
        }
    }
    cyls = (WORD)(cth / heads);
    *out_cyls = cyls; *out_heads = heads; *out_spt = spt;
}

static HRESULT vhd_append_fixed_footer(const wchar_t *path, ULONGLONG size_bytes)
{
    BYTE footer[512]; HANDLE h; DWORD bytes_written;
    SYSTEMTIME st; FILETIME ft;
    ULARGE_INTEGER vhd_epoch_ft;
    DWORD timestamp; ULONGLONG total_sectors;
    WORD cyls; BYTE heads, spt; GUID uid; DWORD sum; int i;

    ZeroMemory(footer, sizeof(footer));
    memcpy(footer + 0, "conectix", 8);
    put_be32(footer + 8, 0x00000002u);
    put_be32(footer + 12, 0x00010000u);
    put_be64(footer + 16, 0xFFFFFFFFFFFFFFFFull);
    GetSystemTime(&st); SystemTimeToFileTime(&st, &ft);
    {
        ULARGE_INTEGER now_ft;
        now_ft.LowPart = ft.dwLowDateTime; now_ft.HighPart = ft.dwHighDateTime;
        vhd_epoch_ft.QuadPart = 125911584000000000ULL;
        timestamp = (now_ft.QuadPart < vhd_epoch_ft.QuadPart) ? 0
                  : (DWORD)((now_ft.QuadPart - vhd_epoch_ft.QuadPart) / 10000000ULL);
    }
    put_be32(footer + 24, timestamp);
    memcpy(footer + 28, "asb!", 4);
    put_be32(footer + 32, 0x00010000u);
    put_be32(footer + 36, 0x5769326Bu);  /* "Wi2k" */
    put_be64(footer + 40, size_bytes);
    put_be64(footer + 48, size_bytes);
    total_sectors = size_bytes / 512;
    vhd_compute_chs(total_sectors, &cyls, &heads, &spt);
    footer[56] = (BYTE)(cyls >> 8); footer[57] = (BYTE)cyls;
    footer[58] = heads; footer[59] = spt;
    put_be32(footer + 60, 2u);  /* fixed */
    if (CoCreateGuid(&uid) != S_OK) ZeroMemory(&uid, sizeof(uid));
    memcpy(footer + 68, &uid, 16);
    sum = 0; for (i = 0; i < 512; i++) sum += footer[i]; sum = ~sum;
    put_be32(footer + 64, sum);

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    { LARGE_INTEGER z; z.QuadPart = 0; SetFilePointerEx(h, z, NULL, FILE_END); }
    if (!WriteFile(h, footer, sizeof(footer), &bytes_written, NULL) ||
        bytes_written != sizeof(footer)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        return HRESULT_FROM_WIN32(err);
    }
    CloseHandle(h);
    return S_OK;
}

/* Create an empty dynamic VHDX of `size_bytes`, attach it, then byte-copy
   the contents of `raw_src` onto the attached physical drive. Bypasses the
   VHD intermediate entirely — CreateVirtualDisk's SourcePath doesn't do
   cross-format VHD->VHDX conversion (returns ERROR_VHD_INVALID_TYPE).

   This is the same pattern do_to_vhdx() uses for Windows ISO installs:
   create + attach + treat-as-block-device. Skips sparse regions in the
   source (FSCTL_QUERY_ALLOCATED_RANGES) so we don't write 3+ GB of zeros
   through the VHDX layer for a mostly-empty cloud image. */
static HRESULT raw_to_vhdx(const wchar_t *raw_src, const wchar_t *vhdx_dst,
                            ULONGLONG size_bytes)
{
    VIRTUAL_STORAGE_TYPE vt;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    ATTACH_VIRTUAL_DISK_PARAMETERS aparams;
    HANDLE hVhdx = INVALID_HANDLE_VALUE;
    HANDLE hRaw  = INVALID_HANDLE_VALUE;
    HANDLE hDrive = INVALID_HANDLE_VALUE;
    HRESULT hr = E_FAIL;
    DWORD result;
    wchar_t phys_path[MAX_PATH];
    DWORD phys_len = sizeof(phys_path);

    vt.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    vt.VendorId = VHDX_VENDOR_MS_GUID;

    /* --- 1. Create empty dynamic VHDX at the raw image's natural size.
       Per-VM disk sizing is handled separately by the differencing
       child created in disk_util.c. */
    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.MaximumSize = size_bytes;
    params.Version2.BlockSizeInBytes = 0;     /* default 32 MiB */
    params.Version2.SectorSizeInBytes = 512;
    params.Version2.PhysicalSectorSizeInBytes = 4096;

    log_debug(L"raw_to_vhdx: creating VHDX size=%llu at %s", size_bytes, vhdx_dst);
    result = CreateVirtualDisk(&vt, vhdx_dst, VIRTUAL_DISK_ACCESS_NONE, NULL,
                                CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &params, NULL, &hVhdx);
    if (result != ERROR_SUCCESS) {
        log_err(L"raw_to_vhdx: CreateVirtualDisk failed (0x%08X)", result);
        return HRESULT_FROM_WIN32(result);
    }

    /* --- 2. Attach so the host kernel surfaces it as a physical disk --- */
    ZeroMemory(&aparams, sizeof(aparams));
    aparams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    result = AttachVirtualDisk(hVhdx, NULL,
                                ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
                                0, &aparams, NULL);
    if (result != ERROR_SUCCESS) {
        log_err(L"raw_to_vhdx: AttachVirtualDisk failed (0x%08X)", result);
        hr = HRESULT_FROM_WIN32(result);
        goto cleanup;
    }

    /* --- 3. Resolve to \\.\PhysicalDriveN --- */
    result = GetVirtualDiskPhysicalPath(hVhdx, &phys_len, phys_path);
    if (result != ERROR_SUCCESS) {
        log_err(L"raw_to_vhdx: GetVirtualDiskPhysicalPath failed (0x%08X)", result);
        hr = HRESULT_FROM_WIN32(result);
        goto cleanup;
    }
    log_debug(L"raw_to_vhdx: attached at %s", phys_path);

    /* --- 4. Open source raw + target physical drive --- */
    hRaw = CreateFileW(raw_src, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hRaw == INVALID_HANDLE_VALUE) {
        log_err(L"raw_to_vhdx: open raw source failed (%lu)", GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    hDrive = CreateFileW(phys_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                          NULL);
    if (hDrive == INVALID_HANDLE_VALUE) {
        log_err(L"raw_to_vhdx: open physical drive failed (%lu)", GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }

    /* --- 5. Query allocated ranges in the source raw image (sparse-aware
       copy: skip ranges that are pure holes in NTFS). Falls back to a
       full linear copy if the query fails. --- */
    log_msg(L"Copying raw image to VHDX (sparse-aware)...");
    {
        FILE_ALLOCATED_RANGE_BUFFER query_range;
        FILE_ALLOCATED_RANGE_BUFFER ranges[256];
        DWORD bytes_returned;
        BOOL more;
        DWORD copy_buf_size = 1024 * 1024;  /* 1 MiB */
        BYTE *copy_buf;
        ULONGLONG total_copied = 0;
        ULONG last_pct = 0;

        copy_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, copy_buf_size);
        if (!copy_buf) { hr = E_OUTOFMEMORY; goto cleanup; }

        query_range.FileOffset.QuadPart = 0;
        query_range.Length.QuadPart = (LONGLONG)size_bytes;

        do {
            more = !DeviceIoControl(hRaw, FSCTL_QUERY_ALLOCATED_RANGES,
                                     &query_range, sizeof(query_range),
                                     ranges, sizeof(ranges),
                                     &bytes_returned, NULL);
            DWORD nr = bytes_returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
            DWORD i;
            if (more && GetLastError() != ERROR_MORE_DATA) {
                log_debug(L"raw_to_vhdx: FSCTL_QUERY_ALLOCATED_RANGES failed (%lu) — falling back to linear copy",
                          GetLastError());
                HeapFree(GetProcessHeap(), 0, copy_buf);
                copy_buf = NULL;
                break;
            }

            for (i = 0; i < nr; i++) {
                ULONGLONG off = (ULONGLONG)ranges[i].FileOffset.QuadPart;
                ULONGLONG len = (ULONGLONG)ranges[i].Length.QuadPart;
                LARGE_INTEGER li;
                /* Position both source and target at the same offset. */
                li.QuadPart = (LONGLONG)off;
                if (!SetFilePointerEx(hRaw, li, NULL, FILE_BEGIN) ||
                    !SetFilePointerEx(hDrive, li, NULL, FILE_BEGIN)) {
                    hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
                }
                while (len > 0) {
                    DWORD chunk = (DWORD)(len > copy_buf_size ? copy_buf_size : len);
                    DWORD got;
                    if (!ReadFile(hRaw, copy_buf, chunk, &got, NULL) || got != chunk) {
                        log_err(L"raw_to_vhdx: read at %llu len %lu failed (%lu)",
                                off + (ranges[i].Length.QuadPart - len), chunk, GetLastError());
                        hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
                    }
                    /* FILE_FLAG_NO_BUFFERING demands sector-aligned writes;
                       1 MiB chunks are sector-aligned, the last short chunk
                       might not be — pad to 512 in that case. */
                    if (chunk & 511) {
                        DWORD padded = (chunk + 511) & ~511u;
                        memset(copy_buf + chunk, 0, padded - chunk);
                        chunk = padded;
                    }
                    {
                        DWORD wrote;
                        if (!WriteFile(hDrive, copy_buf, chunk, &wrote, NULL) || wrote != chunk) {
                            log_err(L"raw_to_vhdx: write at %llu len %lu failed (%lu)",
                                    off + (ranges[i].Length.QuadPart - len), chunk, GetLastError());
                            hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup;
                        }
                    }
                    len -= (chunk > len) ? len : chunk;
                    total_copied += chunk;

                    {
                        ULONG pct = (ULONG)(total_copied * 100 / size_bytes);
                        if (pct >= last_pct + 5) {
                            log_progress(pct, L"Copying to VHDX...");
                            last_pct = pct;
                        }
                    }
                }
            }

            /* Advance query window past the last reported range so the
               next call gets the next batch. */
            if (more && nr > 0) {
                ULONGLONG last_end = (ULONGLONG)ranges[nr - 1].FileOffset.QuadPart
                                    + (ULONGLONG)ranges[nr - 1].Length.QuadPart;
                if (last_end >= size_bytes) break;
                query_range.FileOffset.QuadPart = (LONGLONG)last_end;
                query_range.Length.QuadPart = (LONGLONG)(size_bytes - last_end);
            } else if (!more) {
                /* Final batch (DeviceIoControl returned TRUE — no more data). */
                break;
            } else if (nr == 0) {
                /* ERROR_MORE_DATA but zero ranges — shouldn't happen, bail. */
                break;
            }
        } while (1);

        if (copy_buf) HeapFree(GetProcessHeap(), 0, copy_buf);
        log_debug(L"raw_to_vhdx: copied %llu bytes (sparse holes left as zero in VHDX)",
                  total_copied);
    }

    hr = S_OK;

cleanup:
    if (hDrive != INVALID_HANDLE_VALUE) CloseHandle(hDrive);
    if (hRaw   != INVALID_HANDLE_VALUE) CloseHandle(hRaw);
    if (hVhdx  != INVALID_HANDLE_VALUE) {
        DetachVirtualDisk(hVhdx, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(hVhdx);
    }
    if (FAILED(hr)) DeleteFileW(vhdx_dst);
    return hr;
}

/* Walk every volume on `disk_no` and return the first one whose mounted
   filesystem reports as FAT32. That's how we locate the Ubuntu cloud-image
   ESP regardless of which partition slot it lives in (Canonical uses 15,
   but the layout has shifted across releases). Also logs what we see, so a
   failure tells us exactly which volumes Windows did detect. */
static BOOL find_fat32_volume_on_disk(DWORD disk_no,
                                       wchar_t *vol_path, size_t vol_path_len)
{
    HANDLE hFind;
    wchar_t vol_name[MAX_PATH];
    BOOL found = FALSE;
    int seen = 0;

    hFind = FindFirstVolumeW(vol_name, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    do {
        wchar_t vol_dev[MAX_PATH];
        HANDLE hVol;
        BYTE buf[256];
        DWORD bytes;
        VOLUME_DISK_EXTENTS *ext;
        wchar_t fs_name[32] = {0};
        wchar_t label[64] = {0};
        STORAGE_DEVICE_NUMBER sdn = {0};

        wcscpy_s(vol_dev, MAX_PATH, vol_name);
        {
            size_t vlen = wcslen(vol_dev);
            if (vlen > 0 && vol_dev[vlen - 1] == L'\\')
                vol_dev[vlen - 1] = L'\0';
        }

        hVol = CreateFileW(vol_dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE) continue;

        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                             NULL, 0, buf, sizeof(buf), &bytes, NULL)) {
            ext = (VOLUME_DISK_EXTENTS *)buf;
            if (ext->NumberOfDiskExtents >= 1 &&
                ext->Extents[0].DiskNumber == disk_no) {
                DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                 NULL, 0, &sdn, sizeof(sdn), &bytes, NULL);
                /* GetVolumeInformationW wants the trailing-slash form. */
                if (GetVolumeInformationW(vol_name, label,
                                           (DWORD)(sizeof(label)/sizeof(label[0])),
                                           NULL, NULL, NULL,
                                           fs_name,
                                           (DWORD)(sizeof(fs_name)/sizeof(fs_name[0])))) {
                    log_debug(L"patch_esp: vol part=%u fs=%s label=%s (%s)",
                              sdn.PartitionNumber, fs_name, label, vol_name);
                    seen++;
                    if (!found && _wcsicmp(fs_name, L"FAT32") == 0) {
                        wcscpy_s(vol_path, vol_path_len, vol_name);
                        found = TRUE;
                    }
                } else {
                    log_debug(L"patch_esp: vol part=%u (no fs info, err %lu) %s",
                              sdn.PartitionNumber, GetLastError(), vol_name);
                    seen++;
                }
            }
        }
        CloseHandle(hVol);
    } while (FindNextVolumeW(hFind, vol_name, MAX_PATH));

    FindVolumeClose(hFind);
    if (!found)
        log_debug(L"patch_esp: enumerated %d volume(s) on disk %u, none were FAT32",
                  seen, disk_no);
    return found;
}

/* Re-attach the just-built Ubuntu cloud-image VHDX, mount its FAT32 ESP,
   and replace /EFI/ubuntu/grub.cfg with a stanza that forwards the kernel
   ring buffer and systemd journal to the serial console. Without this the only kernel
   messages on Hyper-V COM1 are the pre-pivot ones; once dracut hands off
   to systemd, printk goes to /dev/kmsg only and we go blind on serial.

   We boot the same kernel/initrd via the symlinks /boot/vmlinuz and
   /boot/initrd.img — grub on ext4 follows symlinks, so this stays valid
   across kernel-package upgrades. */
static HRESULT patch_ubuntu_esp_grub_for_debug(const wchar_t *vhdx_path)
{
    VIRTUAL_STORAGE_TYPE vt;
    OPEN_VIRTUAL_DISK_PARAMETERS op;
    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    HANDLE hVhdx = INVALID_HANDLE_VALUE;
    DWORD result;
    HRESULT hr = E_FAIL;
    wchar_t phys_path[MAX_PATH];
    DWORD phys_len = sizeof(phys_path);
    DWORD disk_no = 0;
    wchar_t esp_vol[MAX_PATH] = {0};
    wchar_t esp_mount[MAX_PATH] = {0};
    wchar_t grub_cfg_path[MAX_PATH];
    BOOL attached = FALSE;
    BOOL mounted = FALSE;
    HANDLE hCfg = INVALID_HANDLE_VALUE;
    DWORD written = 0;

    /* GRUB script. Notes (verified in grub-2.12 source, not assumed):
       - search.fs_label LABEL VAR sets VAR from the partition with label
         LABEL (search_label.c).
       - Ubuntu cloud images put the kernel/initrd at the *root* of a
         separate FS labeled "BOOT" (per prior serial-log evidence
         "Mounting boot.mount" + "BOOT_IMAGE=/vmlinuz-..."). So we search
         by label rather than guess partition numbers.
       - GRUB's wildcard translator (wildcard.c) glob-expands every script
         argument: '*' → regex '.*', anchored '^...$'. We use that to find
         the kernel/initrd files without baking in a version, which would
         break on kernel updates. The `for` loop just picks whichever
         filename the glob resolves to (in practice exactly one). */
    static const char cfg[] =
        "# Generated by iso-patch.exe (debug-serial)\n"
        "set timeout=0\n"
        "search.fs_label BOOT root\n"
        "for f in ($root)/vmlinuz-*-generic; do set vmlinuz=$f; done\n"
        "for f in ($root)/initrd.img-*-generic; do set initrdimg=$f; done\n"
        "linux $vmlinuz root=LABEL=cloudimg-rootfs ro "
            IP_EARLYCON_A "console=tty1 console=" IP_SERIAL_A " "
            "systemd.journald.forward_to_console=1 "
            "systemd.log_target=console "
            "systemd.log_level=info "
            "loglevel=7 "
            IP_EARLYPRINTK_A "\n"
        "initrd $initrdimg\n"
        "boot\n";

    log_msg(L"Patching ESP /EFI/ubuntu/grub.cfg for serial debug");

    vt.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    vt.VendorId = VHDX_VENDOR_MS_GUID;

    ZeroMemory(&op, sizeof(op));
    op.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    op.Version2.GetInfoOnly = FALSE;

    result = OpenVirtualDisk(&vt, vhdx_path, VIRTUAL_DISK_ACCESS_NONE,
                              OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &hVhdx);
    if (result != ERROR_SUCCESS) {
        log_err(L"patch_esp: OpenVirtualDisk failed (%lu)", result);
        return HRESULT_FROM_WIN32(result);
    }

    ZeroMemory(&ap, sizeof(ap));
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    result = AttachVirtualDisk(hVhdx, NULL,
                                ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
                                0, &ap, NULL);
    if (result != ERROR_SUCCESS) {
        log_err(L"patch_esp: AttachVirtualDisk failed (%lu)", result);
        hr = HRESULT_FROM_WIN32(result);
        goto cleanup;
    }
    attached = TRUE;

    result = GetVirtualDiskPhysicalPath(hVhdx, &phys_len, phys_path);
    if (result != ERROR_SUCCESS) {
        log_err(L"patch_esp: GetVirtualDiskPhysicalPath failed (%lu)", result);
        hr = HRESULT_FROM_WIN32(result);
        goto cleanup;
    }
    disk_no = get_disk_number_from_path(phys_path);
    log_debug(L"patch_esp: disk_no=%u (%s)", disk_no, phys_path);

    /* The raw_to_vhdx pass wrote a fresh GPT into PhysicalDriveN, but
       partmgr cached the (empty) layout from when we first attached. Force
       a partition-table re-read so Windows surfaces the new partitions as
       volumes. Without this, FindFirstVolume never sees the ESP. */
    {
        HANDLE hDisk = CreateFileW(phys_path,
                                     GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, NULL);
        if (hDisk == INVALID_HANDLE_VALUE) {
            log_err(L"patch_esp: open %s for rescan failed (%lu)",
                    phys_path, GetLastError());
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto cleanup;
        } else {
            DWORD ret_bytes = 0;
            if (!DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                                  NULL, 0, NULL, 0, &ret_bytes, NULL)) {
                log_debug(L"patch_esp: IOCTL_DISK_UPDATE_PROPERTIES failed (%lu) "
                          L"— proceeding anyway", GetLastError());
            } else {
                log_debug(L"patch_esp: partition-table rescan ok");
            }
            CloseHandle(hDisk);
        }
    }

    /* Let the volume manager finish enumerating partitions after the
       rescan. Auto-mount of FAT32 partitions happens asynchronously. */
    Sleep(3000);

    if (!find_fat32_volume_on_disk(disk_no, esp_vol, MAX_PATH)) {
        log_err(L"patch_esp: no FAT32 volume found on disk %u", disk_no);
        hr = E_FAIL;
        goto cleanup;
    }
    log_debug(L"patch_esp: ESP volume %s", esp_vol);

    if (!create_temp_mount_dir(L"asb_esp", esp_mount, MAX_PATH)) {
        log_err(L"patch_esp: create_temp_mount_dir failed (%lu)", GetLastError());
        hr = E_FAIL;
        goto cleanup;
    }
    if (!mount_volume_to_dir(esp_vol, esp_mount)) {
        log_err(L"patch_esp: SetVolumeMountPointW(%s -> %s) failed (%lu)",
                esp_mount, esp_vol, GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    mounted = TRUE;
    log_debug(L"patch_esp: mounted at %s", esp_mount);

    swprintf_s(grub_cfg_path, MAX_PATH, L"%sEFI\\ubuntu\\grub.cfg", esp_mount);
    hCfg = CreateFileW(grub_cfg_path, GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hCfg == INVALID_HANDLE_VALUE) {
        log_err(L"patch_esp: open %s for write failed (%lu)",
                grub_cfg_path, GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    if (!WriteFile(hCfg, cfg, (DWORD)(sizeof(cfg) - 1), &written, NULL) ||
        written != sizeof(cfg) - 1) {
        log_err(L"patch_esp: write grub.cfg failed (%lu)", GetLastError());
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto cleanup;
    }
    log_msg(L"patch_esp: wrote %lu bytes to %s", written, grub_cfg_path);
    hr = S_OK;

cleanup:
    if (hCfg != INVALID_HANDLE_VALUE) CloseHandle(hCfg);
    if (mounted) unmount_volume_from_dir(esp_mount);
    if (esp_mount[0]) RemoveDirectoryW(esp_mount);
    if (attached) DetachVirtualDisk(hVhdx, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    if (hVhdx != INVALID_HANDLE_VALUE) CloseHandle(hVhdx);
    return hr;
}

/* ---- Top-level: do_qcow2_to_vhdx ---- */

static int do_qcow2_to_vhdx(const wchar_t *qcow2_in, const wchar_t *vhdx_out)
{
    wchar_t raw_path[MAX_PATH];
    wchar_t cache_dir[MAX_PATH];
    HRESULT hr;
    HANDLE h; LARGE_INTEGER sz;

    /* AttachVirtualDisk on a freshly-created VHDX requires SE_MANAGE_VOLUME.
       Admin tokens have it present-but-disabled; enable it now. */
    if (!enable_privilege(SE_MANAGE_VOLUME_NAME)) {
        log_debug(L"SE_MANAGE_VOLUME_NAME not enabled (token lacks it, or AdjustTokenPrivileges failed). "
                  L"AttachVirtualDisk will probably fail with 0x522 — run as Administrator.");
    } else {
        log_debug(L"SE_MANAGE_VOLUME_NAME enabled.");
    }

    /* Derive intermediate path in the same directory as the output. */
    wcscpy_s(cache_dir, MAX_PATH, vhdx_out);
    { wchar_t *s = wcsrchr(cache_dir, L'\\'); if (s) *s = L'\0'; }
    swprintf_s(raw_path, MAX_PATH, L"%s\\.iso-patch-raw.img", cache_dir);

    /* Delete any stale VHDX so CreateVirtualDisk doesn't trip on
       FILE_EXISTS from a previous failed run. */
    DeleteFileW(vhdx_out);

    log_msg(L"qcow2 -> raw...");
    hr = qcow2_to_raw(qcow2_in, raw_path);
    if (FAILED(hr)) { log_err(L"qcow2_to_raw failed (0x%08X)", hr); goto fail; }

    h = CreateFileW(raw_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz)) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        log_err(L"can't stat raw image"); goto fail;
    }
    CloseHandle(h);
    if ((sz.QuadPart & 511) != 0) { log_err(L"raw size not 512-aligned"); goto fail; }

    log_msg(L"Creating VHDX and copying raw image (%lld bytes)...", sz.QuadPart);
    hr = raw_to_vhdx(raw_path, vhdx_out, (ULONGLONG)sz.QuadPart);
    if (FAILED(hr)) { log_err(L"raw_to_vhdx failed (0x%08X)", hr); goto fail; }

    /* Inject debug-serial kernel cmdline so the COM1 pipe shows kernel +
       systemd messages past dracut pivot. Non-fatal if it fails — VM will
       still boot, just without the visibility. */
    {
        HRESULT pr = patch_ubuntu_esp_grub_for_debug(vhdx_out);
        if (FAILED(pr))
            log_err(L"patch_ubuntu_esp_grub_for_debug failed (0x%08X) — "
                    L"VM will boot without serial debug visibility", pr);
    }

    DeleteFileW(raw_path);
    log_done(vhdx_out);
    return 0;

fail:
    DeleteFileW(raw_path);
    return 1;
}

/* ---- Main ---- */

int wmain(int argc, wchar_t *argv[])
{
    if (argc < 2) {
        wprintf(L"Usage:\n");
        wprintf(L"  iso-patch.exe <windows.iso>\n");
        wprintf(L"      Rebuild ISO with noprompt UEFI boot (no \"Press Any Key\").\n\n");
        wprintf(L"  iso-patch.exe --to-vhdx <windows.iso> [image-index] [size-gb] [options]\n");
        wprintf(L"      Convert Windows installer ISO to a bootable UEFI VHDX.\n");
        wprintf(L"      image-index defaults to 1 (first edition in install.wim/esd).\n");
        wprintf(L"      size-gb defaults to %d.\n", DEFAULT_VHDX_SIZE_GB);
        wprintf(L"      --output <path>  Output VHDX path (default: next to iso-patch.exe).\n");
        wprintf(L"      --stage <file>   Tab-separated manifest of files to copy onto the VHDX.\n");
        wprintf(L"                       Each line: <source_path>\\t<dest_relative_to_root>\n\n");
        wprintf(L"  iso-patch.exe --qcow2-to-vhdx <qcow2-in> --output <vhdx-out>\n");
        wprintf(L"      Convert an Ubuntu cloud-image qcow2 to a Hyper-V Gen2 VHDX.\n");
        wprintf(L"      Builds a sparse raw image, wraps with VHD-fixed footer, then\n");
        wprintf(L"      CreateVirtualDisk converts to VHDX. Emits STATUS / PROGRESS /\n");
        wprintf(L"      DEBUG / ERROR / DONE lines like the other modes.\n\n");
        wprintf(L"  iso-patch.exe --ubuntu-to-vhdx <ubuntu.iso> --output <vhdx> [options]\n");
        wprintf(L"      Convert an Ubuntu desktop ISO directly to a fully-installed\n");
        wprintf(L"      bootable VHDX (no subiquity, no user interaction). Streams\n");
        wprintf(L"      casper/minimal.squashfs into a freshly-built ext4 partition.\n");
        wprintf(L"      --size-gb N      virtual size in GiB (default 16, min 16)\n");
        wprintf(L"      --stage <file>   tab-separated manifest: <src>\\t<rootfs-path>\n\n");
        wprintf(L"Output defaults to next to iso-patch.exe.\n");
        return 1;
    }

    if (_wcsicmp(argv[1], L"--patch-grub-debug") == 0) {
        const wchar_t *vhdx;
        DWORD attrs;
        HRESULT pr;
        if (argc < 3) {
            log_err(L"--patch-grub-debug requires <vhdx-path>");
            return 1;
        }
        vhdx = argv[2];
        if (!enable_privilege(SE_MANAGE_VOLUME_NAME))
            log_debug(L"SE_MANAGE_VOLUME_NAME not enabled (run as Administrator)");

        /* AppSandbox marks cached base.vhdx read-only after creation —
           clear that for the duration of this call. The patcher writes
           into the disk via AttachVirtualDisk, not via the file handle,
           but the OPEN_VIRTUAL_DISK call will still be rejected on
           a read-only file. */
        attrs = GetFileAttributesW(vhdx);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(vhdx, attrs & ~FILE_ATTRIBUTE_READONLY);

        pr = patch_ubuntu_esp_grub_for_debug(vhdx);

        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(vhdx, attrs);

        return FAILED(pr) ? 1 : 0;
    }

    if (_wcsicmp(argv[1], L"--qcow2-to-vhdx") == 0) {
        const wchar_t *qcow2 = NULL;
        const wchar_t *output = NULL;
        int i;
        for (i = 2; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--output") == 0) {
                if (i + 1 < argc) { output = argv[++i]; }
                else { log_err(L"--output requires a path"); return 1; }
            } else if (!qcow2) {
                qcow2 = argv[i];
            }
        }
        if (!qcow2 || !output) {
            log_err(L"--qcow2-to-vhdx requires <qcow2-in> and --output <vhdx-out>");
            return 1;
        }
        return do_qcow2_to_vhdx(qcow2, output);
    }

    if (_wcsicmp(argv[1], L"--prefetch-repo") == 0) {
        const wchar_t *branch = NULL, *out_dir = NULL;
        for (int i = 2; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--branch") == 0 && i + 1 < argc) branch = argv[++i];
            else if (_wcsicmp(argv[i], L"--out-dir") == 0 && i + 1 < argc) out_dir = argv[++i];
        }
        if (!branch || !out_dir) {
            log_err(L"--prefetch-repo requires --branch and --out-dir");
            return 1;
        }
        return do_prefetch_repo(branch, out_dir);
    }

    if (_wcsicmp(argv[1], L"--prefetch-wsl-deps") == 0) {
        const wchar_t *out_dir = NULL;
        for (int i = 2; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--out-dir") == 0 && i + 1 < argc)
                out_dir = argv[++i];
        }
        if (!out_dir) {
            log_err(L"--prefetch-wsl-deps requires --out-dir");
            return 1;
        }
        return do_prefetch_wsl_deps(out_dir);
    }

    if (_wcsicmp(argv[1], L"--prefetch-build-deps") == 0) {
        const wchar_t *codename = NULL, *kver = NULL;
        const wchar_t *out_dir = NULL, *mirror = NULL;
        for (int i = 2; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--codename") == 0 && i + 1 < argc) {
                codename = argv[++i];
            } else if (_wcsicmp(argv[i], L"--kernel") == 0 && i + 1 < argc) {
                kver = argv[++i];
            } else if (_wcsicmp(argv[i], L"--out-dir") == 0 && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (_wcsicmp(argv[i], L"--mirror") == 0 && i + 1 < argc) {
                mirror = argv[++i];
            }
        }
        if (!codename || !kver || !out_dir) {
            log_err(L"--prefetch-build-deps requires --codename, --kernel, --out-dir");
            return 1;
        }
        return do_prefetch_build_deps(codename, kver, out_dir, mirror);
    }

    if (_wcsicmp(argv[1], L"--ubuntu-to-vhdx") == 0) {
        const wchar_t *iso = NULL;
        const wchar_t *output = NULL;
        const wchar_t *manifest = NULL;
        int size_gb = 16;
        for (int i = 2; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--output") == 0) {
                if (i + 1 < argc) { output = argv[++i]; }
                else { log_err(L"--output requires a path"); return 1; }
            } else if (_wcsicmp(argv[i], L"--stage") == 0) {
                if (i + 1 < argc) { manifest = argv[++i]; }
                else { log_err(L"--stage requires a manifest file"); return 1; }
            } else if (_wcsicmp(argv[i], L"--size-gb") == 0) {
                if (i + 1 < argc) { size_gb = _wtoi(argv[++i]); }
                else { log_err(L"--size-gb requires a number"); return 1; }
            } else if (!iso) {
                iso = argv[i];
            }
        }
        if (!iso || !output) {
            log_err(L"--ubuntu-to-vhdx requires <ubuntu.iso> and --output <vhdx>");
            return 1;
        }
        return do_ubuntu_to_vhdx(iso, output, size_gb, manifest);
    }

    if (_wcsicmp(argv[1], L"--to-vhdx") == 0) {
        int image_index = 1;
        int size_gb = DEFAULT_VHDX_SIZE_GB;
        const wchar_t *manifest = NULL;
        const wchar_t *output = NULL;
        int positional = 0;
        if (argc < 3) {
            log_err(L"--to-vhdx requires an ISO path");
            return 1;
        }
        /* Parse optional positional args and flags */
        for (int i = 3; i < argc; i++) {
            if (_wcsicmp(argv[i], L"--stage") == 0) {
                if (i + 1 < argc) { manifest = argv[++i]; }
                else { log_err(L"--stage requires a manifest file path"); return 1; }
            } else if (_wcsicmp(argv[i], L"--output") == 0) {
                if (i + 1 < argc) { output = argv[++i]; }
                else { log_err(L"--output requires a file path"); return 1; }
            } else if (positional == 0) {
                image_index = _wtoi(argv[i]);
                if (image_index < 1) image_index = 1;
                positional++;
            } else if (positional == 1) {
                size_gb = _wtoi(argv[i]);
                if (size_gb < 16) size_gb = 16;
                positional++;
            }
        }
        return do_to_vhdx(argv[2], image_index, size_gb, manifest, output);
    }

    return do_noprompt(argv[1]);
}
