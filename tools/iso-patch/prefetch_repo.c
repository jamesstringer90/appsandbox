#include "prefetch_repo.h"
#include "prefetch_build_deps.h"
#include "iso_patch_log.h"
#include "target_arch.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

/* ---- shared helpers (small; not extracted to a common header
       because the three prefetch modules each only need 2-3 of them) */

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
        log_err(L"prefetch-repo: CreateProcess failed: %lu (%s)",
                GetLastError(), cmdline);
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)ec;
}

static BOOL u_mkdir_p(const wchar_t *path)
{
    wchar_t buf[MAX_PATH];
    wcsncpy_s(buf, MAX_PATH, path, _TRUNCATE);
    wchar_t *p = buf;
    if (wcslen(buf) >= 3 && buf[1] == L':' && buf[2] == L'\\') p = buf + 3;
    for (; *p; p++) {
        if (*p == L'\\') { *p = 0; CreateDirectoryW(buf, NULL); *p = L'\\'; }
    }
    CreateDirectoryW(buf, NULL);
    return TRUE;
}

static int http_download_secure(const wchar_t *host, INTERNET_PORT port,
                                const wchar_t *path, const wchar_t *out_path)
{
    HINTERNET hSession = WinHttpOpen(L"AppSandbox-iso-patch/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { log_err(L"prefetch-repo: WinHttpOpen failed: %lu", GetLastError()); return -1; }
    int rc = -1;
    HINTERNET hConn = WinHttpConnect(hSession, host, port, 0);
    if (!hConn) {
        log_err(L"prefetch-repo: WinHttpConnect %s:%u failed: %lu", host, port, GetLastError());
        goto cleanup_sess;
    }
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        log_err(L"prefetch-repo: WinHttpOpenRequest failed: %lu", GetLastError());
        goto cleanup_conn;
    }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        log_err(L"prefetch-repo: SendRequest failed: %lu", GetLastError());
        goto cleanup_req;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        log_err(L"prefetch-repo: ReceiveResponse failed: %lu", GetLastError());
        goto cleanup_req;
    }
    /* codeload.github.com redirects (302) onto an objects.githubusercontent.com
     * CDN URL; without WINHTTP_OPTION_DISABLE_FEATURE the auto-follow happens
     * silently, but for completeness handle a non-2xx ourselves. */
    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status / 100 != 2) {
        log_err(L"prefetch-repo: HTTP %lu", status);
        rc = (int)status;
        goto cleanup_req;
    }

    HANDLE hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        log_err(L"prefetch-repo: CreateFileW(%s) failed: %lu", out_path, GetLastError());
        goto cleanup_req;
    }
    BYTE buf[65536];
    DWORD total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) { CloseHandle(hFile); goto cleanup_req; }
        if (avail == 0) break;
        DWORD n = avail > sizeof(buf) ? sizeof(buf) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, n, &read)) { CloseHandle(hFile); goto cleanup_req; }
        if (read == 0) break;
        DWORD wr = 0;
        if (!WriteFile(hFile, buf, read, &wr, NULL) || wr != read) {
            CloseHandle(hFile); goto cleanup_req;
        }
        total += read;
    }
    CloseHandle(hFile);
    log_msg(L"prefetch-repo: downloaded %lu bytes", total);
    rc = 0;

cleanup_req:  WinHttpCloseHandle(hReq);
cleanup_conn: WinHttpCloseHandle(hConn);
cleanup_sess: WinHttpCloseHandle(hSession);
    return rc;
}

/* Copy one regular file. Creates intermediate dirs in dst path. */
static int u_cp_file(const wchar_t *src, const wchar_t *dst)
{
    wchar_t parent[MAX_PATH];
    wcsncpy_s(parent, MAX_PATH, dst, _TRUNCATE);
    wchar_t *slash = wcsrchr(parent, L'\\');
    if (slash) { *slash = 0; u_mkdir_p(parent); }
    if (!CopyFileW(src, dst, FALSE)) {
        log_err(L"prefetch-repo: copy %s -> %s failed: %lu",
                src, dst, GetLastError());
        return -1;
    }
    return 0;
}

/* Recursive copy: src_dir/* -> dst_dir/. Creates dst_dir. */
static int u_cp_tree(const wchar_t *src_dir, const wchar_t *dst_dir)
{
    u_mkdir_p(dst_dir);
    wchar_t spec[MAX_PATH];
    swprintf_s(spec, MAX_PATH, L"%s\\*", src_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    int count = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%s\\%s", src_dir, fd.cFileName);
        swprintf_s(d, MAX_PATH, L"%s\\%s", dst_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int sub = u_cp_tree(s, d);
            if (sub < 0) { FindClose(h); return -1; }
            if (sub > 0) count += sub;
        } else {
            if (CopyFileW(s, d, FALSE)) count++;
            else {
                log_err(L"prefetch-repo: CopyFileW %s -> %s failed: %lu",
                        s, d, GetLastError());
                FindClose(h);
                return -1;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

static BOOL valid_tag(const wchar_t *tag)
{
    return tag && tag[0] && wcslen(tag) < 128 &&
        wcsspn(tag, L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == wcslen(tag);
}

static void remove_tree(const wchar_t *directory)
{
    wchar_t spec[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW entry;
    swprintf_s(spec, MAX_PATH, L"%s\\*", directory);
    HANDLE find = FindFirstFileW(spec, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
            swprintf_s(path, MAX_PATH, L"%s\\%s", directory, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(path);
                else remove_tree(path);
            } else {
                SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path);
            }
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}

static BOOL repository_layout_valid(const wchar_t *repository, const wchar_t *directory)
{
    const wchar_t *app_files[] = {
        L"tools\\linux\\agent\\Makefile",
        L"tools\\linux\\agent\\appsandbox-agent.c",
        L"tools\\linux\\agent\\appsandbox-display.c",
        L"tools\\linux\\agent\\appsandbox-input.c",
        L"tools\\linux\\agent\\appsandbox-audio.c",
        L"tools\\linux\\agent\\appsandbox-clipboard.c",
        L"tools\\linux\\asb_drm\\Kbuild",
        L"tools\\linux\\asb_drm\\dkms.conf"
    };
    const wchar_t *dxg_files[] = {
        L"src\\Kbuild", L"src\\dkms.conf", L"src\\dxgmodule.c",
        L"src\\include\\uapi\\misc\\d3dkmthk.h"
    };
    BOOL app = wcscmp(repository, L"appsandbox") == 0;
    const wchar_t **files = app ? app_files : dxg_files;
    size_t count = app ? ARRAYSIZE(app_files) : ARRAYSIZE(dxg_files);
    for (size_t i = 0; i < count; i++) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%s\\%s", directory, files[i]);
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || attributes & FILE_ATTRIBUTE_DIRECTORY) return FALSE;
    }
    return TRUE;
}

static void cache_repository(const wchar_t *repository, const wchar_t *tag,
                              const wchar_t *cache_directory, const wchar_t *directory)
{
    wchar_t archive[MAX_PATH], cached[MAX_PATH], pending[MAX_PATH];
    swprintf_s(archive, MAX_PATH, L"%s.tar.gz", directory);
    swprintf_s(cached, MAX_PATH, L"%s\\%s-%s.tar.gz", cache_directory, repository, tag);
    swprintf_s(pending, MAX_PATH, L"%s.%lu.%lu.tmp", cached, GetCurrentProcessId(), GetTickCount());
    if (!CopyFileW(archive, pending, FALSE) ||
        !MoveFileExW(pending, cached, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log_msg(L"prefetch-repo: could not save cache %s (%lu)", cached, GetLastError());
        DeleteFileW(pending);
    } else {
        log_msg(L"prefetch-repo: cached %s tag %s", repository, tag);
    }
}

static int fetch_repository(const wchar_t *repository, const wchar_t *tag,
                            const wchar_t *cache_directory, const wchar_t *directory,
                            BOOL *save_cache)
{
    wchar_t path[1024], archive[MAX_PATH], cached[MAX_PATH], sys_dir[MAX_PATH], cmd[2048];
    BOOL cache_hit = FALSE, used_main = FALSE, retried = FALSE;
    *save_cache = FALSE;
    swprintf_s(archive, MAX_PATH, L"%s.tar.gz", directory);
    swprintf_s(cached, MAX_PATH, L"%s\\%s-%s.tar.gz", cache_directory, repository, tag);
retry:
    cache_hit = FALSE;
    used_main = FALSE;
    if (!retried && GetFileAttributesW(cached) != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(cached, archive, FALSE)) return -1;
        cache_hit = TRUE;
        log_msg(L"prefetch-repo: using cached %s tag %s (%s)", repository, tag, cached);
    }
    swprintf_s(path, 1024, L"/jamesstringer90/%s/tar.gz/refs/tags/%s", repository, tag);
    int rc = 0;
    if (!cache_hit) {
        log_msg(L"prefetch-repo: %s tag %s", repository, tag);
        rc = http_download_secure(L"codeload.github.com", 443, path, archive);
    }
    if (rc == 404 || rc == 410) {
        log_msg(L"prefetch-repo: %s tag %s unavailable; trying main", repository, tag);
        swprintf_s(path, 1024, L"/jamesstringer90/%s/tar.gz/refs/heads/main", repository);
        used_main = TRUE;
        rc = http_download_secure(L"codeload.github.com", 443, path, archive);
    }
    if (rc != 0) return -1;
    if (!CreateDirectoryW(directory, NULL)) return -1;
    if (!GetSystemDirectoryW(sys_dir, MAX_PATH)) return -1;
    swprintf_s(cmd, 2048, L"\"%s\\tar.exe\" -xzf \"%s\" -C \"%s\" --strip-components=1",
               sys_dir, archive, directory);
    if (u_run_cmd(cmd) != 0 || !repository_layout_valid(repository, directory)) {
        if (!cache_hit) return -1;
        log_msg(L"prefetch-repo: invalid cached %s tag %s; downloading again", repository, tag);
        DeleteFileW(cached);
        remove_tree(directory);
        retried = TRUE;
        goto retry;
    }
    *save_cache = !cache_hit && !used_main;
    return 0;
}

static int read_manifest_value(const wchar_t *path, const char *key,
                               wchar_t *value, size_t capacity)
{
    FILE *file = NULL;
    char contents[8192], field[128];
    if (_wfopen_s(&file, path, L"rb") != 0 || !file) return -1;
    size_t length = fread(contents, 1, sizeof(contents) - 1, file);
    BOOL invalid = ferror(file) || !feof(file);
    fclose(file);
    if (invalid) return -1;
    contents[length] = 0;
    sprintf_s(field, sizeof(field), "\"%s\"", key);
    char *start = strstr(contents, field);
    if (!start) return -1;
    start += strlen(field);
    start += strspn(start, " \r\n\t");
    if (*start++ != ':') return -1;
    start += strspn(start, " \r\n\t");
    if (*start++ != '"') return -1;
    char *end = strchr(start, '"');
    if (!end || end == start) return -1;
    for (char *p = start; p < end; p++)
        if ((unsigned char)*p < 32 || *p == '\\') return -1;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start,
                                   (int)(end - start), value, (int)capacity - 1);
    if (!count) return -1;
    value[count] = 0;
    return 0;
}

/* ====================================================================
 * Main flow
 * ==================================================================== */

int do_prefetch_repo(const wchar_t *tag, const wchar_t *mesa_tag,
                     const wchar_t *dxgkrnl_tag, const wchar_t *cache_dir,
                     const wchar_t *out_dir)
{
    if (!valid_tag(tag) || !valid_tag(mesa_tag) || !valid_tag(dxgkrnl_tag) ||
        !out_dir || !out_dir[0]) {
        log_err(L"prefetch-repo: invalid dependency tag or output directory");
        return -1;
    }
    log_msg(L"prefetch-repo: client=%s mesa=%s dxgkrnl=%s out=%s",
            tag, mesa_tag, dxgkrnl_tag, out_dir);

    wchar_t default_cache[MAX_PATH];
    if (!cache_dir) {
        DWORD length = GetModuleFileNameW(NULL, default_cache, ARRAYSIZE(default_cache));
        if (!length || length >= ARRAYSIZE(default_cache)) return -1;
        wchar_t *slash = wcsrchr(default_cache, L'\\');
        if (!slash) return -1;
        *slash = L'\0';
        cache_dir = default_cache;
    }
    if (!cache_dir[0]) return -1;

    wchar_t tmp[MAX_PATH], temp_base[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp_base);
    if (!n || n >= MAX_PATH || !GetTempFileNameW(temp_base, L"asb", 0, tmp) ||
        !DeleteFileW(tmp) || !CreateDirectoryW(tmp, NULL)) return -1;

    wchar_t extracted_root[MAX_PATH], dxg_root[MAX_PATH];
    BOOL save_client_cache = FALSE, save_dxg_cache = FALSE;
    swprintf_s(extracted_root, MAX_PATH, L"%s\\appsandbox", tmp);
    if (fetch_repository(L"appsandbox", tag, cache_dir, extracted_root, &save_client_cache) != 0) {
        remove_tree(tmp);
        return -1;
    }
    swprintf_s(dxg_root, MAX_PATH, L"%s\\dxgkrnl", tmp);
    if (fetch_repository(L"asb-dxgkrnl", dxgkrnl_tag, cache_dir, dxg_root, &save_dxg_cache) != 0) {
        remove_tree(tmp);
        return -1;
    }
    log_msg(L"prefetch-repo: extracted to %s", extracted_root);

    /* ---- 3. Copy each piece into the final layout under <out_dir>. ----
     *
     * The layout matches what generate_vhdx_manifest_ubuntu walks and
     * what firstboot.sh expects at /opt/appsandbox/<...> in the guest.
     */
    u_mkdir_p(out_dir);

    /* agent-src/ */
    {
        wchar_t agent_src[MAX_PATH], dst[MAX_PATH];
        swprintf_s(agent_src, MAX_PATH,
                   L"%s\\tools\\linux\\agent", extracted_root);
        const wchar_t *files[] = {
            L"appsandbox-agent.c", L"appsandbox-audio.c",
            L"appsandbox-clipboard.c", L"appsandbox-display.c",
            L"appsandbox-input.c", L"Makefile"
        };
        wchar_t out_agent[MAX_PATH];
        swprintf_s(out_agent, MAX_PATH, L"%s\\agent-src", out_dir);
        u_mkdir_p(out_agent);
        for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
            wchar_t s[MAX_PATH];
            swprintf_s(s, MAX_PATH, L"%s\\%s", agent_src, files[i]);
            swprintf_s(dst, MAX_PATH, L"%s\\%s", out_agent, files[i]);
            if (u_cp_file(s, dst) != 0) goto failed;
        }
        wchar_t gpu_src[MAX_PATH];
        swprintf_s(gpu_src, MAX_PATH, L"%s\\display-gpu.c", agent_src);
        if (GetFileAttributesW(gpu_src) != INVALID_FILE_ATTRIBUTES) {
            swprintf_s(dst, MAX_PATH, L"%s\\display-gpu.c", out_agent);
            if (u_cp_file(gpu_src, dst) != 0) goto failed;
            swprintf_s(gpu_src, MAX_PATH, L"%s\\display-gpu.h", agent_src);
            swprintf_s(dst, MAX_PATH, L"%s\\display-gpu.h", out_agent);
            if (u_cp_file(gpu_src, dst) != 0) goto failed;
        }
        wchar_t capture_src[MAX_PATH];
        swprintf_s(capture_src, MAX_PATH, L"%s\\tools\\transport\\asb_capture_socket.c", extracted_root);
        if (GetFileAttributesW(capture_src) != INVALID_FILE_ATTRIBUTES) {
            const wchar_t *capture_files[] = {
                L"asb_capture_socket.c", L"asb_capture_socket.h",
                L"asb_capture_service.c", L"asb_capture_service.h", L"asb_automation.h"
            };
            for (size_t i = 0; i < ARRAYSIZE(capture_files); ++i) {
                swprintf_s(capture_src, MAX_PATH, L"%s\\tools\\transport\\%s", extracted_root, capture_files[i]);
                swprintf_s(dst, MAX_PATH, L"%s\\transport\\%s", out_agent, capture_files[i]);
                if (u_cp_file(capture_src, dst) != 0) goto failed;
            }
        }
        wchar_t protocol_src[MAX_PATH];
        swprintf_s(protocol_src, MAX_PATH, L"%s\\src\\core\\protocol.h", extracted_root);
        swprintf_s(dst, MAX_PATH, L"%s\\protocol.h", out_agent);
        if (u_cp_file(protocol_src, dst) != 0) goto failed;
        wchar_t gnome_src[MAX_PATH];
        swprintf_s(gnome_src, MAX_PATH,
                   L"%s\\gnome\\appsandbox-pointer@appsandbox", agent_src);
        if (GetFileAttributesW(gnome_src) != INVALID_FILE_ATTRIBUTES) {
            const wchar_t *gnome_files[] = { L"metadata.json", L"extension.js" };
            for (int i = 0; i < 2; i++) {
                wchar_t s[MAX_PATH];
                swprintf_s(s, MAX_PATH, L"%s\\%s", gnome_src, gnome_files[i]);
                swprintf_s(dst, MAX_PATH,
                           L"%s\\gnome\\appsandbox-pointer@appsandbox\\%s", out_agent, gnome_files[i]);
                if (u_cp_file(s, dst) != 0) goto failed;
            }
        }
        log_msg(L"prefetch-repo: staged agent-src/");
    }

    /* asb_drm-src/: full asb_drm tree */
    {
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%s\\tools\\linux\\asb_drm", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\asb_drm-src", out_dir);
        int c = u_cp_tree(s, d);
        if (c <= 0) goto failed;
        log_msg(L"prefetch-repo: staged asb_drm-src/ (%d files)", c);
    }

    {
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%s\\src", dxg_root);
        swprintf_s(d, MAX_PATH, L"%s\\dxgkrnl-src", out_dir);
        int c = u_cp_tree(s, d);
        if (c <= 0) goto failed;
        log_msg(L"prefetch-repo: staged dxgkrnl-src/ (%d files)", c);
    }

    /* systemd/: 6 service files + asb-evict-simpledrm + 2 modules-load.d */
    {
        wchar_t systemd_dst[MAX_PATH], s[MAX_PATH], d[MAX_PATH];
        swprintf_s(systemd_dst, MAX_PATH, L"%s\\systemd", out_dir);
        u_mkdir_p(systemd_dst);

        const wchar_t *units[] = {
            L"appsandbox-agent.service", L"appsandbox-audio.service",
            L"appsandbox-clipboard.service", L"appsandbox-display.service",
            L"appsandbox-input.service", L"appsandbox-firstboot.service"
        };
        for (int i = 0; i < (int)(sizeof(units) / sizeof(units[0])); i++) {
            swprintf_s(s, MAX_PATH,
                L"%s\\tools\\linux\\agent\\systemd\\%s", extracted_root, units[i]);
            swprintf_s(d, MAX_PATH, L"%s\\%s", systemd_dst, units[i]);
            if (u_cp_file(s, d) != 0) goto failed;
        }
        /* asb-evict-simpledrm: rename from systemd-asb-evict-simpledrm.service */
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\systemd-asb-evict-simpledrm.service",
            extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\asb-evict-simpledrm.service", systemd_dst);
        if (u_cp_file(s, d) != 0) goto failed;

        /* modules-load.d configs (renamed to match what setup expects). */
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\agent\\modules-load.d-snd-aloop.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modules-load.d-snd-aloop.conf", systemd_dst);
        if (u_cp_file(s, d) != 0) goto failed;

        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modules-load.d-asb_drm.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modules-load.d-asb_drm.conf", systemd_dst);
        if (u_cp_file(s, d) != 0) goto failed;

        log_msg(L"prefetch-repo: staged systemd/");
    }

    /* modprobe.d-asb_drm.conf at extras root */
    {
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modprobe.d-asb_drm.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modprobe.d-asb_drm.conf", out_dir);
        if (u_cp_file(s, d) != 0) goto failed;
    }

    /* wsl-mesa pieces */
    {
        const wchar_t *files[] = {
            L"50-appsandbox-gpu",
            L"org.gnome.Shell-no-gpu.conf",
            L"appsandbox-gpu"
        };
        wchar_t s[MAX_PATH], d[MAX_PATH];
        for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
            swprintf_s(s, MAX_PATH,
                L"%s\\tools\\linux\\wsl-mesa\\%s", extracted_root, files[i]);
            swprintf_s(d, MAX_PATH, L"%s\\%s", out_dir, files[i]);
            if (u_cp_file(s, d) != 0) goto failed;
        }
#if !IP_IS_ARM64
        wchar_t manifest[MAX_PATH], cached_tag[128], expected_hash[65], path[1024];
        swprintf_s(manifest, MAX_PATH,
            L"%s\\vendor\\mesa-d3d12\\source-version.json", extracted_root);
        swprintf_s(s, MAX_PATH,
            L"%s\\vendor\\mesa-d3d12\\prebuilt\\ubuntu-26.04-" IP_DEB_ARCH L"\\wsl-mesa.tar.zst",
            extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\wsl-mesa.tar.zst", out_dir);
        BOOL cached = GetFileAttributesW(s) != INVALID_FILE_ATTRIBUTES &&
            read_manifest_value(manifest, "tag", cached_tag, ARRAYSIZE(cached_tag)) == 0 &&
            wcscmp(cached_tag, mesa_tag) == 0;
        if (cached) {
            log_msg(L"prefetch-repo: using cached Mesa %s from AppSandbox %s", mesa_tag, tag);
            if (u_cp_file(s, d) != 0) goto failed;
        } else {
            swprintf_s(path, ARRAYSIZE(path),
                L"/jamesstringer90/asb-mesa/releases/download/%s/wsl-mesa.tar.zst", mesa_tag);
            int rc = http_download_secure(L"github.com", 443, path, d);
            BOOL use_main = rc == 404 || rc == 410;
            swprintf_s(manifest, MAX_PATH, L"%s\\mesa-source-version.json", tmp);
            if (use_main) {
                log_msg(L"prefetch-repo: Mesa package tag %s unavailable; trying main", mesa_tag);
                rc = http_download_secure(L"raw.githubusercontent.com", 443,
                    L"/jamesstringer90/asb-mesa/main/prebuilt/ubuntu-26.04-" IP_DEB_ARCH L"/wsl-mesa.tar.zst", d);
                if (rc != 0 || http_download_secure(L"raw.githubusercontent.com", 443,
                    L"/jamesstringer90/asb-mesa/main/prebuilt/source-version.json", manifest) != 0) goto failed;
            } else {
                if (rc != 0) goto failed;
                swprintf_s(path, ARRAYSIZE(path),
                    L"/jamesstringer90/asb-mesa/releases/download/%s/source-version.json", mesa_tag);
                if (http_download_secure(L"github.com", 443, path, manifest) != 0) goto failed;
                if (read_manifest_value(manifest, "tag", cached_tag, ARRAYSIZE(cached_tag)) != 0 ||
                    wcscmp(cached_tag, mesa_tag) != 0) goto failed;
            }
        }
        if (read_manifest_value(manifest, "archiveSha256", expected_hash, ARRAYSIZE(expected_hash)) != 0 ||
            wcslen(expected_hash) != 64 ||
            wcsspn(expected_hash, L"0123456789abcdefABCDEF") != 64) goto failed;
        char actual_hash[65];
        wchar_t actual_hash_w[65];
        if (prefetch_sha256_file(d, actual_hash) != 0) goto failed;
        if (!MultiByteToWideChar(CP_UTF8, 0, actual_hash, -1, actual_hash_w, ARRAYSIZE(actual_hash_w)) ||
            _wcsicmp(actual_hash_w, expected_hash) != 0) {
            log_err(L"prefetch-repo: Mesa package checksum mismatch");
            goto failed;
        }
        swprintf_s(d, MAX_PATH, L"%s\\mesa-source-version.json", out_dir);
        if (u_cp_file(manifest, d) != 0) goto failed;
#endif
        log_msg(L"prefetch-repo: staged wsl-mesa pieces");
    }

    if (save_client_cache) cache_repository(L"appsandbox", tag, cache_dir, extracted_root);
    if (save_dxg_cache) cache_repository(L"asb-dxgkrnl", dxgkrnl_tag, cache_dir, dxg_root);
    remove_tree(tmp);

    log_msg(L"prefetch-repo: OK -> %s", out_dir);
    return 0;

failed:
    remove_tree(tmp);
    return -1;
}
