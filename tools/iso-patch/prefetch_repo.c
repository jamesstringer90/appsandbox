/* prefetch_repo.c - see header.
 *
 * Pipeline:
 *   1. WinHTTP-download
 *      https://codeload.github.com/jamesstringer90/appsandbox/tar.gz/refs/heads/<branch>
 *      → <tmp>/repo.tar.gz
 *   2. Shell out to tar.exe (libarchive handles .tar.gz natively):
 *      `tar -xzf repo.tar.gz -C <tmp>` → <tmp>/appsandbox-<branch>/...
 *   3. Copy each named subdir / file from the extracted tree to <out_dir>
 *      in the final layout. Renames where needed.
 *   4. Clean up <tmp>.
 *
 * No staging dance / atomic rename: <out_dir> is the per-VM staging extras
 * folder which is fresh for this VM-create.
 *
 * Uses HTTPS via WinHTTP — github.com requires TLS.
 */

#include "prefetch_repo.h"
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
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) continue;
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%s\\%s", src_dir, fd.cFileName);
        swprintf_s(d, MAX_PATH, L"%s\\%s", dst_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int sub = u_cp_tree(s, d);
            if (sub > 0) count += sub;
        } else {
            if (CopyFileW(s, d, FALSE)) count++;
            else log_err(L"prefetch-repo: CopyFileW %s -> %s failed: %lu",
                         s, d, GetLastError());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

/* ====================================================================
 * Main flow
 * ==================================================================== */

int do_prefetch_repo(const wchar_t *branch, const wchar_t *out_dir)
{
    log_msg(L"prefetch-repo: branch=%s out=%s", branch, out_dir);

    /* Temp dir for download + extract. Wiped on completion. */
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return -1;
    swprintf_s(tmp + wcslen(tmp), MAX_PATH - wcslen(tmp),
               L"isopatch-repo-%lu", GetTickCount());
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
        CreateDirectoryW(tmp, NULL);
    }

    /* ---- 1. Download tarball via codeload.github.com (HTTPS). ---- */
    wchar_t tgz[MAX_PATH];
    swprintf_s(tgz, MAX_PATH, L"%s\\repo.tar.gz", tmp);
    {
        wchar_t path[1024];
        swprintf_s(path, 1024,
            L"/jamesstringer90/appsandbox/tar.gz/refs/heads/%s", branch);
        log_msg(L"prefetch-repo: GET https://codeload.github.com%s", path);
        if (http_download_secure(L"codeload.github.com", 443, path, tgz) != 0) {
            log_err(L"prefetch-repo: download failed");
            return -1;
        }
    }

    /* ---- 2. Extract via tar.exe. ---- */
    {
        wchar_t sys_dir[MAX_PATH], cmd[2048];
        GetSystemDirectoryW(sys_dir, MAX_PATH);
        swprintf_s(cmd, 2048,
            L"\"%s\\tar.exe\" -xzf \"%s\" -C \"%s\"",
            sys_dir, tgz, tmp);
        if (u_run_cmd(cmd) != 0) {
            log_err(L"prefetch-repo: tar -xzf failed");
            return -1;
        }
    }

    /* GitHub names the top-level dir "appsandbox-<branch>". Find it
     * (just glob, in case '/' in branch name became '-'). */
    wchar_t extracted_root[MAX_PATH] = L"";
    {
        wchar_t spec[MAX_PATH];
        swprintf_s(spec, MAX_PATH, L"%s\\appsandbox-*", tmp);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(spec, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;
                swprintf_s(extracted_root, MAX_PATH, L"%s\\%s", tmp, fd.cFileName);
                break;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!extracted_root[0]) {
            log_err(L"prefetch-repo: no appsandbox-* dir under %s", tmp);
            return -1;
        }
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
            u_cp_file(s, dst);
        }
        wchar_t protocol_src[MAX_PATH];
        swprintf_s(protocol_src, MAX_PATH, L"%s\\src\\core\\protocol.h", extracted_root);
        if (GetFileAttributesW(protocol_src) != INVALID_FILE_ATTRIBUTES) {
            swprintf_s(dst, MAX_PATH, L"%s\\protocol.h", out_agent);
            if (u_cp_file(protocol_src, dst) != 0) return -1;
        }
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
                if (u_cp_file(s, dst) != 0) return -1;
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
        log_msg(L"prefetch-repo: staged asb_drm-src/ (%d files)", c);
    }

    /* dxgkrnl-src/: contents of tools/linux/dxgkrnl/src/ */
    {
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH, L"%s\\tools\\linux\\dxgkrnl\\src", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\dxgkrnl-src", out_dir);
        int c = u_cp_tree(s, d);
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
            u_cp_file(s, d);
        }
        /* asb-evict-simpledrm: rename from systemd-asb-evict-simpledrm.service */
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\systemd-asb-evict-simpledrm.service",
            extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\asb-evict-simpledrm.service", systemd_dst);
        u_cp_file(s, d);

        /* modules-load.d configs (renamed to match what setup expects). */
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\agent\\modules-load.d-snd-aloop.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modules-load.d-snd-aloop.conf", systemd_dst);
        u_cp_file(s, d);

        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modules-load.d-asb_drm.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modules-load.d-asb_drm.conf", systemd_dst);
        u_cp_file(s, d);

        log_msg(L"prefetch-repo: staged systemd/");
    }

    /* modprobe.d-asb_drm.conf at extras root */
    {
        wchar_t s[MAX_PATH], d[MAX_PATH];
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\asb_drm\\modprobe.d-asb_drm.conf", extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\modprobe.d-asb_drm.conf", out_dir);
        u_cp_file(s, d);
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
            u_cp_file(s, d);
        }
        /* wsl-mesa.tar.zst from the prebuilt dir (large, ~21 MB). */
        swprintf_s(s, MAX_PATH,
            L"%s\\tools\\linux\\wsl-mesa\\prebuilt\\ubuntu-26.04-" IP_DEB_ARCH L"\\wsl-mesa.tar.zst",
            extracted_root);
        swprintf_s(d, MAX_PATH, L"%s\\wsl-mesa.tar.zst", out_dir);
        u_cp_file(s, d);
        log_msg(L"prefetch-repo: staged wsl-mesa pieces");
    }

    /* Clean up temp. */
    {
        wchar_t cmd[1024];
        swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", tmp);
        u_run_cmd(cmd);
    }

    log_msg(L"prefetch-repo: OK -> %s", out_dir);
    return 0;
}
