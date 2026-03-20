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

static void log_msg(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    wprintf(L"STATUS:");
    vwprintf(fmt, ap);
    wprintf(L"\n");
    fflush(stdout);
    va_end(ap);
}

static void log_progress(int pct, const wchar_t *step)
{
    wprintf(L"PROGRESS:%d:%s\n", pct, step);
    fflush(stdout);
}

static void log_done(const wchar_t *path)
{
    wprintf(L"DONE:%s\n", path);
    fflush(stdout);
}

static void log_err(const wchar_t *fmt, ...)
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
        if (FAILED(hr) || bytes_read == 0) break;
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
            L"%s\\dism.exe /Get-WimInfo /WimFile:\"%s\" /Index:%d",
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
        wprintf(L"Output defaults to next to iso-patch.exe.\n");
        return 1;
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
