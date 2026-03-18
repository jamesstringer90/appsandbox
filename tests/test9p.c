/*
 * test9p.c - End-to-end test for p9client
 *
 * Connects to the live Plan9 share via Hyper-V socket on port 50001,
 * copies all files to a temp directory, then re-reads every file from
 * the share and compares byte-by-byte against the local copy.
 *
 * Run inside the VM while the share is active:
 *   test9p.exe [share_name] [port]
 *   Defaults: share_name=AppSandbox.HostDrivers  port=50001
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2def.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID HV_GUID_PARENT = {
    0xa42e7cda, 0xd03f, 0x480c,
    {0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78}
};

static GUID make_vsock_guid(UINT32 port)
{
    GUID g;
    g.Data1 = port;
    g.Data2 = 0xFACB;
    g.Data3 = 0x11E6;
    g.Data4[0] = 0xBD; g.Data4[1] = 0x58;
    g.Data4[2] = 0x64; g.Data4[3] = 0x00;
    g.Data4[4] = 0x6A; g.Data4[5] = 0x79;
    g.Data4[6] = 0x86; g.Data4[7] = 0xD3;
    return g;
}

/* ---- 9P2000.L protocol ---- */

#define P9_TVERSION     100
#define P9_RVERSION     101
#define P9_TATTACH      104
#define P9_RATTACH      105
#define P9_RLERROR      7
#define P9_RERROR       107
#define P9_TWALK        110
#define P9_RWALK        111
#define P9_TLOPEN       12
#define P9_RLOPEN       13
#define P9_TREAD        116
#define P9_RREAD        117
#define P9_TGETATTR     24
#define P9_RGETATTR     25
#define P9_TREADDIR     40
#define P9_RREADDIR     41
#define P9_TCLUNK       120
#define P9_RCLUNK       121

#define QTDIR           0x80
#define P9_MSIZE        65536
#define P9_NOTAG        0xFFFF
#define P9_NOFID        0xFFFFFFFF
#define P9_GETATTR_BASIC 0x000007ffULL

/* ---- Global state ---- */

static BYTE g_sendbuf[P9_MSIZE];
static BYTE g_recvbuf[P9_MSIZE];
/* g_dirbuf removed — each test_directory level allocates its own */
static SOCKET g_sock = INVALID_SOCKET;
static UINT16 g_tag = 1;
static UINT32 g_next_fid = 10;
static UINT32 g_msize = P9_MSIZE;

/* Test counters */
static int g_files_copied = 0;
static int g_files_verified = 0;
static int g_files_failed = 0;
static int g_dirs_found = 0;
static int g_errors = 0;
static UINT64 g_bytes_total = 0;

/* Current path context for error messages */
static wchar_t g_context_path[MAX_PATH] = L"";

/* ---- Pack/unpack ---- */

static void pack_u8(BYTE **p, UINT8 v) { **p = v; (*p)++; }
static void pack_u16(BYTE **p, UINT16 v) { memcpy(*p, &v, 2); *p += 2; }
static void pack_u32(BYTE **p, UINT32 v) { memcpy(*p, &v, 4); *p += 4; }
static void pack_u64(BYTE **p, UINT64 v) { memcpy(*p, &v, 8); *p += 8; }
static void pack_str(BYTE **p, const char *s) {
    UINT16 len = (UINT16)strlen(s);
    pack_u16(p, len);
    memcpy(*p, s, len);
    *p += len;
}
static UINT8 unpack_u8(BYTE **p) { UINT8 v = **p; (*p)++; return v; }
static UINT16 unpack_u16(BYTE **p) { UINT16 v; memcpy(&v, *p, 2); *p += 2; return v; }
static UINT32 unpack_u32(BYTE **p) { UINT32 v; memcpy(&v, *p, 4); *p += 4; return v; }
static UINT64 unpack_u64(BYTE **p) { UINT64 v; memcpy(&v, *p, 8); *p += 8; return v; }
static void unpack_str(BYTE **p, char *out, int max) {
    UINT16 len = unpack_u16(p);
    int copy = (len < max - 1) ? len : max - 1;
    memcpy(out, *p, copy);
    out[copy] = '\0';
    *p += len;
}

/* ---- Socket I/O ---- */

static BOOL sock_send(BYTE *data, int size)
{
    int sent = 0;
    while (sent < size) {
        int n = send(g_sock, (char *)(data + sent), size - sent, 0);
        if (n <= 0) return FALSE;
        sent += n;
    }
    return TRUE;
}

static BOOL sock_recv_msg(void)
{
    UINT32 size;
    int recvd = 0;
    while (recvd < 4) {
        int n = recv(g_sock, (char *)(g_recvbuf + recvd), 4 - recvd, 0);
        if (n <= 0) return FALSE;
        recvd += n;
    }
    memcpy(&size, g_recvbuf, 4);
    if (size < 7 || size > g_msize) return FALSE;
    while (recvd < (int)size) {
        int n = recv(g_sock, (char *)(g_recvbuf + recvd), (int)size - recvd, 0);
        if (n <= 0) return FALSE;
        recvd += n;
    }
    return TRUE;
}

static UINT8 msg_type(BYTE **payload)
{
    BYTE *p = g_recvbuf + 4;
    UINT8 type = unpack_u8(&p);
    p += 2; /* skip tag */
    *payload = p;
    return type;
}

static BOOL check_error(BYTE *payload, UINT8 type, const char *op)
{
    if (type == P9_RLERROR) {
        UINT32 ecode = unpack_u32(&payload);
        fprintf(stderr, "  FAIL: %s -> 9P error (errno %u) in %ls\n",
                op, ecode, g_context_path);
        g_errors++;
        return TRUE;
    }
    if (type == P9_RERROR) {
        char err[256];
        unpack_str(&payload, err, sizeof(err));
        fprintf(stderr, "  FAIL: %s -> 9P error: %s in %ls\n",
                op, err, g_context_path);
        g_errors++;
        return TRUE;
    }
    return FALSE;
}

/* ---- 9P message helpers ---- */

static UINT16 next_tag(void) { return g_tag++; }
static UINT32 alloc_fid(void) { return g_next_fid++; }

static BYTE *msg_begin(UINT8 type, UINT16 tag)
{
    BYTE *p = g_sendbuf + 4;
    pack_u8(&p, type);
    pack_u16(&p, tag);
    return p;
}

static BOOL msg_send(BYTE *end)
{
    UINT32 size = (UINT32)(end - g_sendbuf);
    memcpy(g_sendbuf, &size, 4);
    return sock_send(g_sendbuf, size);
}

/* ---- 9P operations ---- */

static BOOL p9_version(void)
{
    BYTE *p, *payload; UINT8 type; char ver[32];
    p = msg_begin(P9_TVERSION, P9_NOTAG);
    pack_u32(&p, g_msize);
    pack_str(&p, "9P2000.L");
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "version")) return FALSE;
    if (type != P9_RVERSION) return FALSE;
    g_msize = unpack_u32(&payload);
    unpack_str(&payload, ver, sizeof(ver));
    if (strcmp(ver, "9P2000.L") != 0) {
        fprintf(stderr, "  FAIL: server version '%s' not supported\n", ver);
        return FALSE;
    }
    printf("  Protocol: %s, msize: %u\n", ver, g_msize);
    return TRUE;
}

static BOOL p9_attach(UINT32 fid, const char *aname)
{
    BYTE *p, *payload; UINT8 type;
    p = msg_begin(P9_TATTACH, next_tag());
    pack_u32(&p, fid);
    pack_u32(&p, P9_NOFID);
    pack_str(&p, "nobody");
    pack_str(&p, aname);
    pack_u32(&p, 65534);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "attach")) return FALSE;
    return type == P9_RATTACH;
}

static BOOL p9_walk(UINT32 fid, UINT32 newfid, const char **names, int nnames)
{
    BYTE *p, *payload; UINT8 type; int i;
    p = msg_begin(P9_TWALK, next_tag());
    pack_u32(&p, fid);
    pack_u32(&p, newfid);
    pack_u16(&p, (UINT16)nnames);
    for (i = 0; i < nnames; i++) pack_str(&p, names[i]);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "walk")) return FALSE;
    if (type != P9_RWALK) return FALSE;
    return unpack_u16(&payload) == (UINT16)nnames || nnames == 0;
}

static BOOL p9_lopen(UINT32 fid, UINT32 flags, UINT32 *iounit_out)
{
    BYTE *p, *payload; UINT8 type;
    p = msg_begin(P9_TLOPEN, next_tag());
    pack_u32(&p, fid);
    pack_u32(&p, flags);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "lopen")) return FALSE;
    if (type != P9_RLOPEN) return FALSE;
    payload += 13; /* skip qid */
    if (iounit_out) *iounit_out = unpack_u32(&payload);
    return TRUE;
}

static BOOL p9_read(UINT32 fid, UINT64 offset, UINT32 count,
                    BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload; UINT8 type;
    p = msg_begin(P9_TREAD, next_tag());
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "read")) return FALSE;
    if (type != P9_RREAD) return FALSE;
    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_readdir(UINT32 fid, UINT64 offset, UINT32 count,
                       BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload; UINT8 type;
    p = msg_begin(P9_TREADDIR, next_tag());
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "readdir")) return FALSE;
    if (type != P9_RREADDIR) return FALSE;
    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_getattr(UINT32 fid, UINT32 *mode_out, UINT64 *size_out)
{
    BYTE *p, *payload; UINT8 type;
    p = msg_begin(P9_TGETATTR, next_tag());
    pack_u32(&p, fid);
    pack_u64(&p, P9_GETATTR_BASIC);
    if (!msg_send(p) || !sock_recv_msg()) return FALSE;
    type = msg_type(&payload);
    if (check_error(payload, type, "getattr")) return FALSE;
    if (type != P9_RGETATTR) return FALSE;
    payload += 8 + 13; /* valid + qid */
    if (mode_out) *mode_out = unpack_u32(&payload); else payload += 4;
    payload += 4 + 4 + 8 + 8; /* uid + gid + nlink + rdev */
    if (size_out) *size_out = unpack_u64(&payload);
    return TRUE;
}

static void p9_clunk(UINT32 fid)
{
    BYTE *p;
    p = msg_begin(P9_TCLUNK, next_tag());
    pack_u32(&p, fid);
    msg_send(p);
    sock_recv_msg();
}

/* ---- File helpers ---- */

static void ensure_dir(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH]; wchar_t *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + 3; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            *p = L'\0';
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
}

static void utf8_to_wide(const char *utf8, wchar_t *wide, int wide_max)
{
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wide_max);
}

/* ---- Copy and verify a single file ---- */

static BOOL copy_and_verify_file(UINT32 parent_fid, const char *name,
                                  const wchar_t *local_path, UINT64 file_size)
{
    UINT32 fid = alloc_fid();
    UINT32 verify_fid = alloc_fid();
    UINT32 iounit;
    UINT64 offset;
    HANDLE hfile;
    BOOL ok = TRUE;
    const char *names[1];
    wchar_t saved_context[MAX_PATH];

    /* Set context for any 9P errors during copy/verify */
    wcscpy_s(saved_context, MAX_PATH, g_context_path);
    swprintf_s(g_context_path, MAX_PATH, L"%ls\\%hs", saved_context, name);

    names[0] = name;

    /* Step 1: Copy file from share to local disk */
    if (!p9_walk(parent_fid, fid, names, 1)) { ok = FALSE; goto done; }
    if (!p9_lopen(fid, 0, &iounit)) { p9_clunk(fid); ok = FALSE; goto done; }
    if (iounit == 0 || iounit > g_msize - 24) iounit = g_msize - 24;

    hfile = CreateFileW(local_path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  FAIL: cannot create %ls\n", local_path);
        p9_clunk(fid);
        ok = FALSE; goto done;
    }

    offset = 0;
    while (offset < file_size) {
        BYTE *data; UINT32 nread; DWORD written;
        if (!p9_read(fid, offset, iounit, &data, &nread)) { ok = FALSE; break; }
        if (nread == 0) break;
        if (!WriteFile(hfile, data, nread, &written, NULL) || written != nread) { ok = FALSE; break; }
        offset += nread;
    }
    CloseHandle(hfile);
    p9_clunk(fid);

    if (!ok) {
        fprintf(stderr, "  FAIL: copy error for %s at offset %llu\n",
                name, (unsigned long long)offset);
        goto done;
    }

    g_files_copied++;
    g_bytes_total += file_size;

    /* Step 2: Re-read from share and compare with local copy */
    if (!p9_walk(parent_fid, verify_fid, names, 1)) { ok = FALSE; goto done; }
    if (!p9_lopen(verify_fid, 0, &iounit)) { p9_clunk(verify_fid); ok = FALSE; goto done; }
    if (iounit == 0 || iounit > g_msize - 24) iounit = g_msize - 24;

    hfile = CreateFileW(local_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  FAIL: cannot reopen %ls for verify\n", local_path);
        p9_clunk(verify_fid);
        ok = FALSE; goto done;
    }

    offset = 0;
    while (offset < file_size) {
        BYTE *share_data; UINT32 nread;
        BYTE local_buf[65536]; DWORD local_read;

        if (!p9_read(verify_fid, offset, iounit, &share_data, &nread)) { ok = FALSE; break; }
        if (nread == 0) break;
        if (!ReadFile(hfile, local_buf, nread, &local_read, NULL) || local_read != nread) {
            fprintf(stderr, "  FAIL: local read mismatch at offset %llu for %s\n",
                    (unsigned long long)offset, name);
            ok = FALSE; break;
        }
        if (memcmp(share_data, local_buf, nread) != 0) {
            fprintf(stderr, "  FAIL: content mismatch at offset %llu for %s\n",
                    (unsigned long long)offset, name);
            ok = FALSE; break;
        }
        offset += nread;
    }

    CloseHandle(hfile);
    p9_clunk(verify_fid);

    if (ok) g_files_verified++;

done:
    wcscpy_s(g_context_path, MAX_PATH, saved_context);
    return ok;
}

/* ---- Recursive directory copy + verify ---- */

static BOOL test_directory(UINT32 dir_fid, const wchar_t *local_dir)
{
    UINT32 open_fid = alloc_fid();
    UINT64 dir_offset = 0;
    BOOL ok = TRUE;
    wchar_t saved_context[MAX_PATH];
    BYTE *dirbuf = NULL;

    /* Save and set path context for error messages */
    wcscpy_s(saved_context, MAX_PATH, g_context_path);
    wcscpy_s(g_context_path, MAX_PATH, local_dir);

    printf("  DIR: %ls\n", local_dir);
    ensure_dir(local_dir);

    dirbuf = (BYTE *)malloc(g_msize);
    if (!dirbuf) {
        fprintf(stderr, "  FAIL: out of memory for dir %ls\n", local_dir);
        g_errors++;
        wcscpy_s(g_context_path, MAX_PATH, saved_context);
        return FALSE;
    }

    if (!p9_walk(dir_fid, open_fid, NULL, 0)) {
        fprintf(stderr, "  FAIL: walk(clone) failed for dir %ls\n", local_dir);
        g_errors++;
        free(dirbuf);
        wcscpy_s(g_context_path, MAX_PATH, saved_context);
        return FALSE;
    }
    if (!p9_lopen(open_fid, 0, NULL)) {
        fprintf(stderr, "  FAIL: lopen failed for dir %ls\n", local_dir);
        g_errors++;
        p9_clunk(open_fid);
        free(dirbuf);
        wcscpy_s(g_context_path, MAX_PATH, saved_context);
        return FALSE;
    }

    for (;;) {
        BYTE *data, *dp, *end;
        UINT32 nread;

        if (!p9_readdir(open_fid, dir_offset, g_msize - 24, &data, &nread)) {
            ok = FALSE; break;
        }
        if (nread == 0) break;

        memcpy(dirbuf, data, nread);
        dp = dirbuf;
        end = dirbuf + nread;

        while (dp < end) {
            UINT8 qid_type;
            UINT64 entry_offset;
            UINT8 dtype;
            char entry_name[512];
            wchar_t wide_name[512];
            wchar_t child_path[MAX_PATH];

            qid_type = unpack_u8(&dp);
            dp += 4 + 8; /* qid.version + qid.path */
            entry_offset = unpack_u64(&dp);
            dtype = unpack_u8(&dp);
            unpack_str(&dp, entry_name, sizeof(entry_name));
            dir_offset = entry_offset;

            if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
                continue;

            utf8_to_wide(entry_name, wide_name, 512);
            swprintf_s(child_path, MAX_PATH, L"%s\\%s", local_dir, wide_name);

            if (dtype == 4 || (qid_type & QTDIR)) {
                UINT32 child_fid = alloc_fid();
                const char *names[1];
                names[0] = entry_name;
                g_dirs_found++;

                if (p9_walk(dir_fid, child_fid, names, 1)) {
                    test_directory(child_fid, child_path);
                    p9_clunk(child_fid);
                } else {
                    fprintf(stderr, "  FAIL: walk('%s') failed in %ls\n",
                            entry_name, local_dir);
                    g_errors++;
                }
            } else {
                UINT32 child_fid = alloc_fid();
                const char *names[1];
                names[0] = entry_name;

                if (p9_walk(dir_fid, child_fid, names, 1)) {
                    UINT64 fsize = 0;
                    if (!p9_getattr(child_fid, NULL, &fsize)) {
                        fprintf(stderr, "  FAIL: getattr('%s') failed in %ls\n",
                                entry_name, local_dir);
                        g_errors++;
                        p9_clunk(child_fid);
                        continue;
                    }
                    p9_clunk(child_fid);
                    if (!copy_and_verify_file(dir_fid, entry_name, child_path, fsize)) {
                        fprintf(stderr, "  FAIL: copy/verify '%s' failed in %ls\n",
                                entry_name, local_dir);
                        g_files_failed++;
                    }
                } else {
                    fprintf(stderr, "  FAIL: walk('%s') failed in %ls\n",
                            entry_name, local_dir);
                    g_errors++;
                }
            }
        }
    }

    p9_clunk(open_fid);
    free(dirbuf);
    wcscpy_s(g_context_path, MAX_PATH, saved_context);
    return ok;
}

/* ---- Cleanup ---- */

static void remove_dir_recursive(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_dir_recursive(full);
            else
                DeleteFileW(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* ---- Main ---- */

int wmain(int argc, wchar_t *argv[])
{
    WSADATA wsa;
    SOCKADDR_HV addr;
    char share_name[256] = "AppSandbox.HostDrivers";
    UINT32 port = 50001;
    UINT32 root_fid;
    wchar_t temp_dir[MAX_PATH];
    int ret;

    if (argc >= 2)
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, share_name, sizeof(share_name), NULL, NULL);
    if (argc >= 3)
        port = (UINT32)_wtoi(argv[2]);

    printf("=== p9client end-to-end test ===\n\n");
    printf("Share: %s\n", share_name);
    printf("Port:  %u\n\n", port);

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Connect via Hyper-V socket */
    printf("[1] Connecting to hvsocket port %u...\n", port);
    g_sock = socket(AF_HYPERV, SOCK_STREAM, 1);
    if (g_sock == INVALID_SOCKET) {
        fprintf(stderr, "  FAIL: socket(AF_HYPERV) error %d\n", WSAGetLastError());
        fprintf(stderr, "  (Are you running this inside a Hyper-V VM?)\n");
        return 1;
    }

    ZeroMemory(&addr, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_PARENT;
    addr.ServiceId = make_vsock_guid(port);

    ret = connect(g_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        fprintf(stderr, "  FAIL: connect error %d\n", WSAGetLastError());
        fprintf(stderr, "  (Is the VM running with Plan9 shares configured?)\n");
        closesocket(g_sock);
        return 1;
    }
    printf("  OK: connected\n\n");

    /* Version negotiation */
    printf("[2] Protocol negotiation...\n");
    if (!p9_version()) {
        fprintf(stderr, "  FAIL: version negotiation failed\n");
        goto fail;
    }
    printf("  OK\n\n");

    /* Attach to share */
    printf("[3] Attaching to share '%s'...\n", share_name);
    root_fid = alloc_fid();
    if (!p9_attach(root_fid, share_name)) {
        fprintf(stderr, "  FAIL: attach failed\n");
        goto fail;
    }
    printf("  OK: attached\n\n");

    /* Copy + verify */
    swprintf_s(temp_dir, MAX_PATH, L"C:\\temp\\test9p_%u", GetCurrentProcessId());
    printf("[4] Copying share to %ls and verifying...\n", temp_dir);

    test_directory(root_fid, temp_dir);
    p9_clunk(root_fid);

    /* Results */
    printf("\n=== Results ===\n");
    printf("  Directories:  %d\n", g_dirs_found);
    printf("  Files copied: %d\n", g_files_copied);
    printf("  Files verified (byte-by-byte): %d\n", g_files_verified);
    printf("  Files FAILED: %d\n", g_files_failed);
    printf("  9P errors:    %d\n", g_errors);
    printf("  Total bytes:  %llu\n", (unsigned long long)g_bytes_total);

    if (g_files_failed == 0 && g_errors == 0 && g_files_copied > 0
        && g_files_copied == g_files_verified)
        printf("\n  ALL TESTS PASSED\n");
    else if (g_files_copied == 0)
        printf("\n  FAIL: no files were copied\n");
    else
        printf("\n  FAIL: %d file failures, %d 9P errors\n",
               g_files_failed, g_errors);

    /* Cleanup temp dir */
    printf("\nCleaning up %ls...\n", temp_dir);
    remove_dir_recursive(temp_dir);

    closesocket(g_sock);
    WSACleanup();
    return (g_files_failed == 0 && g_errors == 0 && g_files_copied > 0) ? 0 : 1;

fail:
    closesocket(g_sock);
    WSACleanup();
    return 1;
}
