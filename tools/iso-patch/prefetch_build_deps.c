/* prefetch_build_deps.c - see header for design.
 *
 * Pipeline:
 *   1. WinHTTP-download Packages.xz for <codename>/main/binary-<arch>
 *   2. Decompress it in-process with the vendored xz-embedded decoder
 *      (xz_decompress_file_to_file below; Packages.gz is raw-gzipped
 *      text that tar.exe rejects, so we target the .xz instead).
 *   3. Parse the resulting text into a hashtable of stanzas keyed by
 *      Package name. Each stanza is stored verbatim so we can write
 *      it back into the synthetic Packages we ship.
 *   4. BFS the Depends:/Pre-Depends: closure from seed packages.
 *      Disjunctions ("a | b"): take the first alternative that exists.
 *   5. For each pkg in closure: WinHTTP-download the .deb, verify
 *      its SHA256 against the Packages metadata.
 *   6. Write synthetic Packages with the closure stanzas, but
 *      rewriting Filename: to point at the local basename so
 *      apt's file:/// source resolves correctly.
 *   7. Write .closure.json (free-form, easy enough to hand-format).
 *      Output is written directly into out_dir (per-VM staging, no
 *      host cache layer).
 *
 * Memory: the decompressed Packages file is a few MiB (resolute/main).
 * We slurp the whole thing into memory and parse in-place. Stanzas
 * point into that single buffer (no per-stanza allocations).
 */

#include "prefetch_build_deps.h"
#include "iso_patch_log.h"
#include "target_arch.h"
#include "engine/xz/xz.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s)  (((NTSTATUS)(s)) >= 0)
#endif

/* ====================================================================
 * Small helpers
 * ==================================================================== */

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
        log_err(L"prefetch: CreateProcess failed: %lu (%s)", GetLastError(), cmdline);
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
    /* skip past "X:\" */
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
    return TRUE;
}

static BOOL u_rmdir_recursive(const wchar_t *path)
{
    wchar_t cmd[1024];
    swprintf_s(cmd, 1024, L"cmd.exe /c rd /s /q \"%s\" 2>nul", path);
    u_run_cmd(cmd);
    return TRUE;
}

/* ====================================================================
 * In-process XZ decompression. Reads .xz file, writes uncompressed
 * bytes to dst. Uses the engine/xz/ vendored xz-embedded decoder
 * (same code our squashfs reader uses for casper ingest).
 *
 * We use this instead of shelling out to tar.exe for gunzip because
 * Ubuntu's archive's Packages.gz is a raw-gzipped text file (not a
 * tar.gz), and `tar -xz` rejects it with "Unrecognized archive
 * format". The archive also publishes Packages.xz which we can
 * decode directly with the xz_dec already linked in iso-patch.exe.
 *
 * Streams the decompressed output to disk in 4 MiB chunks so the
 * Packages file (a few MiB uncompressed for Ubuntu's main component)
 * never has to fit in RAM as a single buffer. */
static int xz_decompress_file_to_file(const wchar_t *src,
                                      const wchar_t *dst)
{
    HANDLE hf = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_err(L"xz_decompress: open %s failed: %lu", src, GetLastError());
        return -1;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart > 64 * 1024 * 1024) {
        log_err(L"xz_decompress: bad/too-large input file (%lld bytes)", sz.QuadPart);
        CloseHandle(hf);
        return -1;
    }
    void *in_buf = malloc((size_t)sz.QuadPart);
    if (!in_buf) { CloseHandle(hf); return -1; }
    DWORD br = 0;
    if (!ReadFile(hf, in_buf, (DWORD)sz.QuadPart, &br, NULL) || br != sz.QuadPart) {
        log_err(L"xz_decompress: ReadFile failed: %lu", GetLastError());
        free(in_buf); CloseHandle(hf); return -1;
    }
    CloseHandle(hf);

    xz_crc32_init();
    xz_crc64_init();
    struct xz_dec *dec = xz_dec_init(XZ_DYNALLOC, 1u << 26);
    if (!dec) {
        log_err(L"xz_decompress: xz_dec_init failed");
        free(in_buf);
        return -1;
    }

    HANDLE hOut = CreateFileW(dst, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        log_err(L"xz_decompress: CreateFileW(%s) failed: %lu", dst, GetLastError());
        xz_dec_end(dec); free(in_buf); return -1;
    }

    enum { OUT_CHUNK = 4 * 1024 * 1024 };
    void *out_buf = malloc(OUT_CHUNK);
    if (!out_buf) { CloseHandle(hOut); xz_dec_end(dec); free(in_buf); return -1; }

    struct xz_buf b;
    b.in       = (uint8_t *)in_buf;
    b.in_pos   = 0;
    b.in_size  = (size_t)sz.QuadPart;
    b.out      = (uint8_t *)out_buf;
    b.out_pos  = 0;
    b.out_size = OUT_CHUNK;

    int rc = -1;
    unsigned long long total_out = 0;
    int done = 0;
    while (!done) {
        enum xz_ret r = xz_dec_run(dec, &b);
        int flush = 0;
        if (r == XZ_STREAM_END) { flush = 1; done = 1; rc = 0; }
        else if (r == XZ_OK)    { flush = (b.out_pos == b.out_size); }
        else {
            log_err(L"xz_decompress: xz_dec_run returned %d (input pos %zu / %zu)",
                    (int)r, b.in_pos, b.in_size);
            break;
        }
        if (flush && b.out_pos > 0) {
            DWORD wr = 0;
            if (!WriteFile(hOut, out_buf, (DWORD)b.out_pos, &wr, NULL) ||
                wr != (DWORD)b.out_pos) {
                log_err(L"xz_decompress: WriteFile failed: %lu", GetLastError());
                rc = -1; break;
            }
            total_out += b.out_pos;
            b.out_pos = 0;
        }
    }
    xz_dec_end(dec);
    free(in_buf);
    free(out_buf);
    CloseHandle(hOut);
    if (rc != 0) { DeleteFileW(dst); return -1; }
    log_msg(L"xz_decompress: %s (%llu bytes) -> %s (%llu bytes)",
            src, (unsigned long long)sz.QuadPart, dst, total_out);
    return 0;
}

/* ====================================================================
 * WinHTTP wrapper: download URL -> file
 * ==================================================================== */

/* Parses "http://host[:port]/path..." into the three components. Only
 * http:// is supported (we point at archive.ubuntu.com which serves
 * the apt archive over plain HTTP; SHA256 verify is the security
 * check, not TLS). */
static int parse_url(const wchar_t *url,
                     wchar_t *host_out, size_t host_cap,
                     INTERNET_PORT *port_out,
                     wchar_t *path_out, size_t path_cap)
{
    const wchar_t *p = url;
    if (_wcsnicmp(p, L"http://", 7) == 0)       { p += 7; *port_out = 80;  }
    else if (_wcsnicmp(p, L"https://", 8) == 0) { p += 8; *port_out = 443; }
    else { return -1; }

    const wchar_t *slash = wcschr(p, L'/');
    size_t host_len = slash ? (size_t)(slash - p) : wcslen(p);
    if (host_len >= host_cap) return -1;
    memcpy(host_out, p, host_len * sizeof(wchar_t));
    host_out[host_len] = 0;

    /* explicit :port? */
    wchar_t *colon = wcschr(host_out, L':');
    if (colon) { *colon = 0; *port_out = (INTERNET_PORT)_wtoi(colon + 1); }

    if (slash) {
        wcsncpy_s(path_out, path_cap, slash, _TRUNCATE);
    } else {
        wcscpy_s(path_out, path_cap, L"/");
    }
    return 0;
}

/* log_err() or log_msg() depending on `quiet`: a failed attempt that is
 * going to be retried, or an optional resource, should not surface as an
 * ERROR: line in the app log. */
static void dl_log(int quiet, const wchar_t *fmt, ...)
{
    wchar_t buf[2304];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (quiet) log_msg(L"%s", buf);
    else       log_err(L"%s", buf);
}

/* HTTP GET <url> -> file. Returns 0 on success. Uses a persistent
 * connection per-call (simple; for one-shot fetches this is fine).
 * quiet_http: report failures as STATUS: instead of ERROR: - for
 * optional resources (e.g. a -updates pocket that may not exist) and
 * for attempts that will be retried. */
static int http_download_ex(const wchar_t *url, const wchar_t *out_path, int quiet_http)
{
    wchar_t host[256], path[2048];
    INTERNET_PORT port = 80;
    if (parse_url(url, host, ARRAYSIZE(host), &port, path, ARRAYSIZE(path)) != 0) {
        dl_log(quiet_http, L"prefetch: bad URL: %s", url);
        return -1;
    }

    HINTERNET hSession = WinHttpOpen(L"AppSandbox-iso-patch/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { dl_log(quiet_http, L"prefetch: WinHttpOpen failed: %lu", GetLastError()); return -1; }

    int rc = -1;
    HINTERNET hConn = WinHttpConnect(hSession, host, port, 0);
    if (!hConn) { dl_log(quiet_http, L"prefetch: WinHttpConnect %s:%u failed: %lu", host, port, GetLastError()); goto cleanup_sess; }

    DWORD reqFlags = (port == 443) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, NULL,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        reqFlags);
    if (!hReq) { dl_log(quiet_http, L"prefetch: WinHttpOpenRequest failed: %lu", GetLastError()); goto cleanup_conn; }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        dl_log(quiet_http, L"prefetch: WinHttpSendRequest %s failed: %lu", url, GetLastError());
        goto cleanup_req;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        dl_log(quiet_http, L"prefetch: WinHttpReceiveResponse failed: %lu", GetLastError());
        goto cleanup_req;
    }

    /* Status code check. */
    DWORD status = 0, statusLen = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (quiet_http) log_msg(L"prefetch: HTTP %lu for %s", status, url);
        else            dl_log(quiet_http, L"prefetch: HTTP %lu for %s", status, url);
        goto cleanup_req;
    }

    HANDLE hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        dl_log(quiet_http, L"prefetch: CreateFileW(%s) failed: %lu", out_path, GetLastError());
        goto cleanup_req;
    }

    BYTE buf[16384];
    DWORD total = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            dl_log(quiet_http, L"prefetch: WinHttpQueryDataAvailable failed: %lu", GetLastError());
            CloseHandle(hFile); goto cleanup_req;
        }
        if (avail == 0) break;
        DWORD n = avail > sizeof(buf) ? sizeof(buf) : avail;
        DWORD read = 0;
        if (!WinHttpReadData(hReq, buf, n, &read)) {
            dl_log(quiet_http, L"prefetch: WinHttpReadData failed: %lu", GetLastError());
            CloseHandle(hFile); goto cleanup_req;
        }
        if (read == 0) break;
        DWORD wrote = 0;
        if (!WriteFile(hFile, buf, read, &wrote, NULL) || wrote != read) {
            dl_log(quiet_http, L"prefetch: WriteFile failed: %lu", GetLastError());
            CloseHandle(hFile); goto cleanup_req;
        }
        total += read;
    }
    CloseHandle(hFile);
    rc = 0;

cleanup_req:  WinHttpCloseHandle(hReq);
cleanup_conn: WinHttpCloseHandle(hConn);
cleanup_sess: WinHttpCloseHandle(hSession);
    return rc;
}

/* Retry transient failures (DNS hiccups, dropped connections, mirror
 * 5xx): 3 attempts with a short back-off. Only the last attempt is
 * allowed to log ERROR:. One flaky GET out of ~180 used to abort the
 * whole prefetch and leave the guest without any build tools. */
#define DL_ATTEMPTS 3
static int http_download_retry(const wchar_t *url, const wchar_t *out_path, int quiet_http)
{
    for (int attempt = 1; attempt <= DL_ATTEMPTS; attempt++) {
        int last = (attempt == DL_ATTEMPTS);
        if (http_download_ex(url, out_path, quiet_http || !last) == 0) return 0;
        DeleteFileW(out_path);
        if (!last) {
            log_msg(L"prefetch: retry %d/%d for %s", attempt + 1, DL_ATTEMPTS, url);
            Sleep(2000UL * (DWORD)attempt);
        }
    }
    return -1;
}

static int http_download(const wchar_t *url, const wchar_t *out_path)
{
    return http_download_retry(url, out_path, 0);
}

/* Read a whole file into a malloc'd, NUL-terminated buffer. */
static int slurp_file(const wchar_t *path, char **buf_out, size_t *len_out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7fffffff) { CloseHandle(h); return -1; }
    size_t len = (size_t)sz.QuadPart;
    char *buf = (char *)malloc(len + 1);
    if (!buf) { CloseHandle(h); return -1; }
    DWORD br = 0;
    if (len > 0 && (!ReadFile(h, buf, (DWORD)len, &br, NULL) || br != len)) {
        free(buf); CloseHandle(h); return -1;
    }
    CloseHandle(h);
    buf[len] = 0;
    *buf_out = buf;
    *len_out = len;
    return 0;
}

/* ====================================================================
 * SHA256 via BCrypt — verify the .debs against Packages metadata.
 * ==================================================================== */

static int sha256_file(const wchar_t *path, char *hex_out /* 65 bytes */)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD hashLen = 0, prop = 0;
    BYTE hash[32];
    int rc = -1;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return -1;
    if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                       (PBYTE)&hashLen, sizeof(hashLen), &prop, 0)))
        goto cleanup;
    if (hashLen != 32) goto cleanup;
    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0))) goto cleanup;

    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) goto cleanup;

    BYTE buf[65536];
    DWORD br;
    while (ReadFile(hFile, buf, sizeof(buf), &br, NULL) && br > 0) {
        if (!NT_SUCCESS(BCryptHashData(hHash, buf, br, 0))) goto cleanup;
    }
    if (!NT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0))) goto cleanup;

    for (int i = 0; i < 32; i++)
        sprintf_s(hex_out + i * 2, 3, "%02x", hash[i]);
    hex_out[64] = 0;
    rc = 0;

cleanup:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return rc;
}

/* ====================================================================
 * Packages parser + dep closure walker.
 *
 * Stanzas are separated by blank lines. Within a stanza, lines are
 * "Key: value" with optional continuation lines that start with
 * whitespace. We only care about Package, Filename, Size, SHA256,
 * Version, Depends, Pre-Depends (and optionally Recommends).
 *
 * Memory model: read the entire Packages file into one big malloc'd
 * buffer; each pkg_record_t holds pointers + lengths INTO that buffer.
 * The pkg_table_t hashtable maps Package name (uppercased+hashed) to
 * the first record with that name.
 * ==================================================================== */

typedef struct pkg_record {
    const char *name;        /* not null-terminated; use name_len */
    size_t      name_len;
    const char *filename;
    size_t      filename_len;
    const char *sha256_hex;
    size_t      sha256_len;
    const char *depends;
    size_t      depends_len;
    const char *predepends;
    size_t      predepends_len;
    const char *stanza_start;   /* first char of stanza in the buffer */
    size_t      stanza_len;     /* length of the stanza (no trailing blank) */
    int         in_closure;     /* set during BFS */
    struct pkg_record *bucket_next;
} pkg_record_t;

#define PKG_HASH_BUCKETS 4096   /* >> 4500 packages */

typedef struct {
    char         *buf;          /* slurped Packages text */
    size_t        buf_len;
    pkg_record_t *records;
    size_t        n_records;
    pkg_record_t *buckets[PKG_HASH_BUCKETS];
} pkg_table_t;

static size_t hash_name(const char *s, size_t len)
{
    /* FNV-1a, lowercased so we match case-insensitively. */
    size_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        h ^= (unsigned char)c;
        h *= 0x100000001b3ULL;
    }
    return h % PKG_HASH_BUCKETS;
}

static int name_eq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

static pkg_record_t *lookup_pkg(pkg_table_t *t, const char *name, size_t len)
{
    size_t h = hash_name(name, len);
    for (pkg_record_t *r = t->buckets[h]; r; r = r->bucket_next) {
        if (name_eq(r->name, r->name_len, name, len)) return r;
    }
    return NULL;
}

/* For a "Key: value" line, return (key, val) views or NULL if not a
 * "Key:" line. Continuation lines (leading whitespace) are not
 * handled here — the parser skips them.  */
static int parse_field(const char *line, size_t line_len,
                       const char **key, size_t *klen,
                       const char **val, size_t *vlen)
{
    /* skip leading whitespace? No — top-level fields don't have it. */
    if (line_len == 0 || line[0] == ' ' || line[0] == '\t') return -1;
    const char *colon = memchr(line, ':', line_len);
    if (!colon) return -1;
    *key = line; *klen = (size_t)(colon - line);
    const char *v = colon + 1;
    while (v < line + line_len && (*v == ' ' || *v == '\t')) v++;
    *val = v;
    *vlen = (size_t)(line + line_len - v);
    /* strip trailing whitespace */
    while (*vlen > 0 && ((*val)[*vlen - 1] == ' ' || (*val)[*vlen - 1] == '\t')) (*vlen)--;
    return 0;
}

/* Parse the entire Packages text. Mutates the buffer (replaces some
 * separators with 0s — no, actually it doesn't; we keep stanzas
 * intact). Returns 0 on success. */
static int parse_packages(pkg_table_t *t)
{
    /* Count stanzas first. */
    const char *p = t->buf, *end = t->buf + t->buf_len;
    size_t count = 0;
    const char *line = p;
    int saw_pkg_line = 0;
    for (const char *q = p; q <= end; q++) {
        if (q == end || *q == '\n') {
            size_t llen = (size_t)(q - line);
            /* trim trailing \r */
            if (llen > 0 && line[llen - 1] == '\r') llen--;
            if (llen == 0) {
                if (saw_pkg_line) count++;
                saw_pkg_line = 0;
            } else if (llen > 8 && memcmp(line, "Package:", 8) == 0) {
                saw_pkg_line = 1;
            }
            line = q + 1;
        }
    }
    if (saw_pkg_line) count++;  /* trailing stanza without blank */

    t->n_records = count;
    t->records = (pkg_record_t *)calloc(count, sizeof(pkg_record_t));
    if (!t->records) return -1;

    /* Second pass: actually parse. */
    size_t ri = 0;
    pkg_record_t *cur = NULL;
    const char *stanza_start = p;
    line = p;
    for (const char *q = p; q <= end; q++) {
        if (q == end || *q == '\n') {
            size_t llen = (size_t)(q - line);
            if (llen > 0 && line[llen - 1] == '\r') llen--;
            if (llen == 0) {
                /* End of stanza. */
                if (cur && cur->name) {
                    cur->stanza_start = stanza_start;
                    cur->stanza_len = (size_t)(line - stanza_start);
                    /* trim trailing \n from stanza_len: the blank line
                     * we just hit is OUTSIDE the stanza. */
                    while (cur->stanza_len > 0 &&
                           (cur->stanza_start[cur->stanza_len - 1] == '\n' ||
                            cur->stanza_start[cur->stanza_len - 1] == '\r'))
                        cur->stanza_len--;
                    size_t h = hash_name(cur->name, cur->name_len);
                    cur->bucket_next = t->buckets[h];
                    t->buckets[h] = cur;
                    ri++;
                }
                cur = NULL;
                stanza_start = q + 1;
            } else if (line[0] != ' ' && line[0] != '\t') {
                /* New field line. */
                const char *k, *v; size_t kl, vl;
                if (parse_field(line, llen, &k, &kl, &v, &vl) == 0) {
                    if (!cur && ri < t->n_records) cur = &t->records[ri];
                    if (cur) {
                        if (kl == 7 && memcmp(k, "Package", 7) == 0) {
                            cur->name = v; cur->name_len = vl;
                        } else if (kl == 8 && memcmp(k, "Filename", 8) == 0) {
                            cur->filename = v; cur->filename_len = vl;
                        } else if (kl == 6 && memcmp(k, "SHA256", 6) == 0) {
                            cur->sha256_hex = v; cur->sha256_len = vl;
                        } else if (kl == 7 && memcmp(k, "Depends", 7) == 0) {
                            cur->depends = v; cur->depends_len = vl;
                        } else if (kl == 11 && memcmp(k, "Pre-Depends", 11) == 0) {
                            cur->predepends = v; cur->predepends_len = vl;
                        }
                    }
                }
            }
            /* continuation lines (start with space/tab): ignored */
            line = q + 1;
        }
    }
    /* trailing stanza without blank */
    if (cur && cur->name && !cur->stanza_start) {
        cur->stanza_start = stanza_start;
        cur->stanza_len = (size_t)(end - stanza_start);
        size_t h = hash_name(cur->name, cur->name_len);
        cur->bucket_next = t->buckets[h];
        t->buckets[h] = cur;
    }
    return 0;
}

/* BFS one pass: for each pkg already in_closure, walk Depends + Pre-Depends.
 * Returns count of NEW pkgs added in this pass. */
static int bfs_add_deps(pkg_table_t *t, pkg_record_t *r, int *added_count)
{
    const char *fields[2] = { r->depends, r->predepends };
    size_t lens[2]        = { r->depends_len, r->predepends_len };
    for (int fi = 0; fi < 2; fi++) {
        const char *p = fields[fi];
        size_t n = lens[fi];
        if (!p || n == 0) continue;
        const char *q = p;
        size_t i = 0;
        while (i <= n) {
            /* Walk to next ',' or end -> one Depends element. */
            if (i == n || p[i] == ',') {
                size_t elen = (size_t)(p + i - q);
                /* Take first alternative (split on '|'). */
                const char *alt_end = q;
                while (alt_end < q + elen && *alt_end != '|') alt_end++;
                size_t alen = (size_t)(alt_end - q);
                /* Trim trailing/leading WS. */
                while (alen > 0 && (q[0] == ' ' || q[0] == '\t')) { q++; alen--; }
                while (alen > 0 && (q[alen - 1] == ' ' || q[alen - 1] == '\t' ||
                                    q[alen - 1] == '\n')) alen--;
                /* Strip version constraint "(...)" */
                const char *paren = q;
                while (paren < q + alen && *paren != ' ' && *paren != '(') paren++;
                size_t name_len = (size_t)(paren - q);
                if (name_len > 0) {
                    pkg_record_t *dep = lookup_pkg(t, q, name_len);
                    if (dep && !dep->in_closure) {
                        dep->in_closure = 1;
                        (*added_count)++;
                    }
                }
                q = p + i + 1;
            }
            i++;
        }
    }
    return 0;
}

/* ====================================================================
 * Synthetic Packages writer: copies closure stanzas verbatim but
 * rewrites Filename: to point at the local basename.
 * ==================================================================== */

static int write_synth_packages(pkg_table_t *t,
                                const wchar_t *out_path)
{
    HANDLE h = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    for (size_t i = 0; i < t->n_records; i++) {
        pkg_record_t *r = &t->records[i];
        if (!r->in_closure || !r->stanza_start || !r->filename) continue;
        /* Copy stanza, but when we hit "Filename: ..." rewrite to local. */
        const char *p = r->stanza_start, *end = p + r->stanza_len;
        const char *line = p;
        char buf[2048];
        DWORD wr;
        for (const char *q = p; q <= end; q++) {
            if (q == end || *q == '\n') {
                size_t llen = (size_t)(q - line);
                size_t orig_llen = llen;
                if (llen > 0 && line[llen - 1] == '\r') llen--;
                if (llen > 9 && memcmp(line, "Filename:", 9) == 0) {
                    /* Find basename. */
                    const char *fnp = r->filename;
                    const char *bp = fnp + r->filename_len;
                    while (bp > fnp && *(bp - 1) != '/' && *(bp - 1) != '\\') bp--;
                    size_t blen = (size_t)(fnp + r->filename_len - bp);
                    int n = snprintf(buf, sizeof(buf), "Filename: ./%.*s\n",
                                     (int)blen, bp);
                    DWORD wlen = (n < 0) ? 0u :
                                 (n >= (int)sizeof(buf) ? (DWORD)(sizeof(buf) - 1)
                                                        : (DWORD)n);
                    WriteFile(h, buf, wlen, &wr, NULL);
                } else if (orig_llen > 0) {
                    WriteFile(h, line, (DWORD)orig_llen, &wr, NULL);
                    WriteFile(h, "\n", 1, &wr, NULL);
                }
                line = q + 1;
            }
        }
        WriteFile(h, "\n", 1, &wr, NULL);  /* blank line between stanzas */
    }
    CloseHandle(h);
    return 0;
}

static int write_closure_json(pkg_table_t *t,
                              const wchar_t *out_path,
                              const wchar_t *codename,
                              const wchar_t *kver)
{
    HANDLE h = CreateFileW(out_path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char buf[2048];
    DWORD wr;
    /* Hand-rolled JSON; trivial content. */
    char codename_utf8[64], kver_utf8[64];
    WideCharToMultiByte(CP_UTF8, 0, codename, -1, codename_utf8, sizeof(codename_utf8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, kver, -1, kver_utf8, sizeof(kver_utf8), NULL, NULL);
    int n = snprintf(buf, sizeof(buf),
        "{\n  \"codename\": \"%s\",\n  \"kernel_version\": \"%s\",\n  \"closure\": [\n",
        codename_utf8, kver_utf8);
    DWORD wlen = (n < 0) ? 0u :
                 (n >= (int)sizeof(buf) ? (DWORD)(sizeof(buf) - 1) : (DWORD)n);
    WriteFile(h, buf, wlen, &wr, NULL);
    int first = 1;
    for (size_t i = 0; i < t->n_records; i++) {
        pkg_record_t *r = &t->records[i];
        if (!r->in_closure) continue;
        n = snprintf(buf, sizeof(buf),
            "%s    {\"name\": \"%.*s\"}",
            first ? "" : ",\n",
            (int)r->name_len, r->name);
        wlen = (n < 0) ? 0u :
               (n >= (int)sizeof(buf) ? (DWORD)(sizeof(buf) - 1) : (DWORD)n);
        WriteFile(h, buf, wlen, &wr, NULL);
        first = 0;
    }
    WriteFile(h, "\n  ]\n}\n", 7, &wr, NULL);
    CloseHandle(h);
    return 0;
}

/* ====================================================================
 * Main entry point
 * ==================================================================== */

static int prefetch_build_deps_inner(const wchar_t *codename,
                                     const wchar_t *kernel_ver,
                                     const wchar_t *out_dir,
                                     const wchar_t *mirror_arg)
{
    const wchar_t *mirror = mirror_arg ? mirror_arg
                                       : L"http://archive.ubuntu.com/ubuntu";

    log_msg(L"prefetch: codename=%s kver=%s mirror=%s out=%s",
            codename, kernel_ver, mirror, out_dir);

    /* Direct write to out_dir — no host cache, per-VM staging. */
    u_rmdir_recursive(out_dir);
    u_mkdir_p(out_dir);
    const wchar_t *staging = out_dir;

    /* ---- 1. Download Packages.xz ---- */
    wchar_t pkgs_xz[MAX_PATH], pkgs[MAX_PATH];
    swprintf_s(pkgs_xz, MAX_PATH, L"%s\\Packages.xz", staging);
    swprintf_s(pkgs,    MAX_PATH, L"%s\\Packages",    staging);

    wchar_t url[1024];
    swprintf_s(url, 1024, L"%s/dists/%s/main/binary-" IP_DEB_ARCH L"/Packages.xz",
               mirror, codename);
    log_msg(L"prefetch: GET %s", url);
    if (http_download(url, pkgs_xz) != 0) {
        log_err(L"prefetch: download Packages.xz failed");
        return -1;
    }

    /* ---- 2. In-process xz decompression via vendored xz-embedded ---- */
    if (xz_decompress_file_to_file(pkgs_xz, pkgs) != 0) {
        log_err(L"prefetch: xz decompress failed");
        return -1;
    }

    /* ---- 2b. <codename>-updates as well.
       Point-release ISOs (24.04.4, ...) are built from the -updates
       pocket, so the base system already carries e.g. libasound2t64
       1.2.11-1ubuntu0.1 while the release pocket's libasound2-dev pins
       "= 1.2.11-1build2": apt in the guest then refuses the whole build
       tool set. Merging the -updates index (preferred over the release
       one, see step 3) keeps the closure in step with the ISO. A fresh
       LTS without a -updates pocket yet simply skips this. ---- */
    wchar_t pkgs_upd_xz[MAX_PATH], pkgs_upd[MAX_PATH];
    int have_updates = 0;
    swprintf_s(pkgs_upd_xz, MAX_PATH, L"%s\\Packages-updates.xz", staging);
    swprintf_s(pkgs_upd,    MAX_PATH, L"%s\\Packages-updates",    staging);
    swprintf_s(url, 1024, L"%s/dists/%s-updates/main/binary-" IP_DEB_ARCH L"/Packages.xz",
               mirror, codename);
    log_msg(L"prefetch: GET %s", url);
    if (http_download_retry(url, pkgs_upd_xz, 1) == 0 &&
        xz_decompress_file_to_file(pkgs_upd_xz, pkgs_upd) == 0) {
        have_updates = 1;
    } else {
        log_msg(L"prefetch: WARN no usable %s-updates index - using the release pocket only",
                codename);
    }

    /* ---- 3. Slurp Packages (+ Packages-updates) into memory + parse ---- */
    pkg_table_t T = { 0 };
    {
        char *base_buf = NULL, *upd_buf = NULL;
        size_t base_len = 0, upd_len = 0;
        if (slurp_file(pkgs, &base_buf, &base_len) != 0) {
            log_err(L"prefetch: read Packages failed");
            return -1;
        }
        if (have_updates && slurp_file(pkgs_upd, &upd_buf, &upd_len) != 0) {
            log_msg(L"prefetch: WARN read Packages-updates failed - ignoring it");
            upd_buf = NULL; upd_len = 0;
        }
        /* Release pocket first, -updates after it. parse_packages()
           prepends every stanza to its hash chain, so for a name present
           in both pockets lookup_pkg() returns the -updates record. */
        T.buf_len = base_len + 2 + upd_len;
        T.buf = (char *)malloc(T.buf_len + 1);
        if (!T.buf) { free(base_buf); free(upd_buf); return -1; }
        memcpy(T.buf, base_buf, base_len);
        T.buf[base_len] = '\n';
        T.buf[base_len + 1] = '\n';
        if (upd_len) memcpy(T.buf + base_len + 2, upd_buf, upd_len);
        T.buf[T.buf_len] = 0;
        free(base_buf);
        free(upd_buf);
    }
    if (parse_packages(&T) != 0) { log_err(L"prefetch: parse failed"); return -1; }
    log_msg(L"prefetch: parsed %zu package records", T.n_records);

    /* ---- 4. Mark seeds in_closure ---- */
    const char *seeds[] = {
        "libasound2-dev", "libxcb1-dev", "libxcb-xfixes0-dev",
        "libdrm-dev", "pkg-config",
        "openssh-server",  /* for ssh_enabled VMs; firstboot installs conditionally */
        /* The ISO pool carries build-essential/gcc/dkms but not always their
           whole closure (24.04.4: no libstdc++-13-dev / libgcc-13-dev /
           cpp-13), and on point releases the pool is a -updates snapshot,
           so the closure has to come from the same pocket to satisfy the
           strict "= version" Depends. Seeding them here (rather than
           relying on the pool) makes firstboot STEP 7.4 self-contained. */
        "build-essential", "dkms",
        /* Layered ISOs (24.04): the kernel is staged from the ISO's live layer
           and firstboot STEP 7.6 tries to register it with dpkg. linux-image
           needs initramfs-tools + linux-base, which the 24.04 ISO pool lacks. */
        "initramfs-tools", "linux-base"
    };
    int closure_count = 0;
    for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
        pkg_record_t *r = lookup_pkg(&T, seeds[i], strlen(seeds[i]));
        if (!r) { log_msg(L"prefetch: WARN seed '%hs' not in archive", seeds[i]); continue; }
        if (!r->in_closure) { r->in_closure = 1; closure_count++; }
    }
    /* Also linux-headers-<kver>. */
    {
        char hdr[128];
        char kver_utf8[64];
        WideCharToMultiByte(CP_UTF8, 0, kernel_ver, -1, kver_utf8, sizeof(kver_utf8), NULL, NULL);
        snprintf(hdr, sizeof(hdr), "linux-headers-%s", kver_utf8);
        pkg_record_t *r = lookup_pkg(&T, hdr, strlen(hdr));
        if (r && !r->in_closure) { r->in_closure = 1; closure_count++; }
    }

    /* ---- 5. BFS until stable ---- */
    int total_added = closure_count;
    for (int iter = 0; iter < 32; iter++) {
        int added = 0;
        for (size_t i = 0; i < T.n_records; i++) {
            pkg_record_t *r = &T.records[i];
            if (!r->in_closure) continue;
            bfs_add_deps(&T, r, &added);
        }
        if (added == 0) break;
        total_added += added;
    }
    log_msg(L"prefetch: closure = %d packages", total_added);

    /* ---- 6. Download each .deb in closure, SHA256 verify ---- */
    int downloaded = 0;
    for (size_t i = 0; i < T.n_records; i++) {
        pkg_record_t *r = &T.records[i];
        if (!r->in_closure) continue;
        if (!r->filename) { log_msg(L"prefetch: WARN %.*hs has no Filename", (int)r->name_len, r->name); continue; }

        /* Compose URL + local path. */
        char fn_utf8[1024], sha_utf8[128];
        if (r->filename_len >= sizeof(fn_utf8)) {
            log_err(L"prefetch: Filename too long (%zu) for %.*hs",
                    r->filename_len, (int)r->name_len, r->name);
            return -1;
        }
        memcpy(fn_utf8, r->filename, r->filename_len);  fn_utf8[r->filename_len] = 0;
        if (r->sha256_hex && r->sha256_len < sizeof(sha_utf8)) {
            memcpy(sha_utf8, r->sha256_hex, r->sha256_len); sha_utf8[r->sha256_len] = 0;
        } else { sha_utf8[0] = 0; }

        wchar_t fn_wide[1024];
        MultiByteToWideChar(CP_UTF8, 0, fn_utf8, -1, fn_wide, ARRAYSIZE(fn_wide));
        const wchar_t *basename = wcsrchr(fn_wide, L'/');
        basename = basename ? basename + 1 : fn_wide;

        wchar_t url2[2048], dst[MAX_PATH];
        swprintf_s(url2, 2048, L"%s/%s", mirror, fn_wide);
        swprintf_s(dst, MAX_PATH, L"%s\\%s", staging, basename);

        log_msg(L"prefetch: GET %s", basename);
        if (http_download(url2, dst) != 0) {
            log_err(L"prefetch: download %ls failed", basename);
            return -1;
        }
        if (sha_utf8[0]) {
            char actual[65];
            if (sha256_file(dst, actual) != 0) {
                log_err(L"prefetch: SHA256 hash compute failed for %ls", basename);
                return -1;
            }
            if (_stricmp(actual, sha_utf8) != 0) {
                log_err(L"prefetch: SHA256 mismatch for %ls (got %hs, want %hs)",
                        basename, actual, sha_utf8);
                return -1;
            }
        }
        downloaded++;
    }
    log_msg(L"prefetch: downloaded %d .debs", downloaded);

    /* ---- 7. Write synthetic Packages + .closure.json ---- */
    {
        wchar_t synth[MAX_PATH];
        swprintf_s(synth, MAX_PATH, L"%s\\Packages", staging);
        /* Delete the gunzipped temp Packages (we want the synthetic in its place). */
        DeleteFileW(synth);
        if (write_synth_packages(&T, synth) != 0) {
            log_err(L"prefetch: write synthetic Packages failed");
            return -1;
        }
    }
    {
        wchar_t cj[MAX_PATH];
        swprintf_s(cj, MAX_PATH, L"%s\\.closure.json", staging);
        write_closure_json(&T, cj, codename, kernel_ver);
    }

    /* Clean up Packages.xz — we don't ship it (we wrote our own
       synthetic Packages with closure entries only). Same for the
       -updates index. */
    DeleteFileW(pkgs_xz);
    DeleteFileW(pkgs_upd_xz);
    DeleteFileW(pkgs_upd);

    free(T.buf);
    free(T.records);
    log_msg(L"prefetch: OK -> %s", out_dir);
    return 0;
}

int do_prefetch_build_deps(const wchar_t *codename,
                           const wchar_t *kernel_ver,
                           const wchar_t *out_dir,
                           const wchar_t *mirror_arg)
{
    int rc = prefetch_build_deps_inner(codename, kernel_ver, out_dir, mirror_arg);
    if (rc != 0) {
        /* A partial staging dir is worse than none: the raw Packages index
           downloaded in step 1 lists the release-pocket versions with
           archive Filename: paths, and apt in the guest happily picks
           those "= version" candidates over the ISO pool - then fails on
           every build tool. Leave an empty dir so firstboot reports
           "no local-apt-extras (host prefetch failed)" instead. */
        log_msg(L"prefetch: failed - discarding partial output in %s", out_dir);
        u_rmdir_recursive(out_dir);
        u_mkdir_p(out_dir);
    }
    return rc;
}
