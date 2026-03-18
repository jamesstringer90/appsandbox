/*
 * p9client.exe - Minimal 9P2000.L client for Hyper-V socket Plan9 shares
 *
 * Connects to the host via Hyper-V socket (AF_HYPERV) on a given port
 * and recursively copies the contents of a named Plan9 share to a local directory.
 *
 * Usage: p9client.exe <share_name> <target_path> [port] [--filter file1;file2;...]
 *   port defaults to 50001
 *   --filter: only copy named files (semicolon-separated) from share root
 *
 * Example:
 *   p9client.exe AppSandbox.HostDrivers C:\Windows\System32\HostDriverStore\FileRepository
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

/* Parent partition GUID (connect to host from guest) */
static const GUID HV_GUID_PARENT = {
    0xa42e7cda, 0xd03f, 0x480c,
    {0x9c, 0xc2, 0xa4, 0xde, 0x20, 0xab, 0xb8, 0x78}
};

/* Vsock port → service GUID template: {port-FACB-11E6-BD58-64006A7986D3} */
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

/* ---- 9P2000.L protocol constants ---- */

#define P9_TVERSION     100
#define P9_RVERSION     101
#define P9_TATTACH      104
#define P9_RATTACH      105
#define P9_RERROR       107
#define P9_TWALK        110
#define P9_RWALK        111
#define P9_TREAD        116
#define P9_RREAD        117
#define P9_TCLUNK       120
#define P9_RCLUNK       121

/* 9P2000.L extensions */
#define P9_RLERROR      7
#define P9_TLOPEN       12
#define P9_RLOPEN       13
#define P9_TGETATTR     24
#define P9_RGETATTR     25
#define P9_TREADDIR     40
#define P9_RREADDIR     41

/* QID type bits */
#define QTDIR           0x80

/* Getattr request mask */
#define P9_GETATTR_MODE 0x00000001ULL
#define P9_GETATTR_SIZE 0x00000200ULL
#define P9_GETATTR_BASIC (P9_GETATTR_MODE | P9_GETATTR_SIZE | 0x000007ffULL)

/* File mode */
#define P9_S_IFDIR      0040000
#define P9_S_IFREG      0100000

/* Protocol limits */
#define P9_MSIZE        65536
#define P9_NOTAG        0xFFFF
#define P9_NOFID        0xFFFFFFFF

/* ---- Global state ---- */

static BYTE g_sendbuf[P9_MSIZE];
static BYTE g_recvbuf[P9_MSIZE];
/* g_dirbuf removed — each copy_dir_contents level allocates its own */
static SOCKET g_sock = INVALID_SOCKET;
static UINT16 g_tag = 1;
static UINT32 g_next_fid = 10;
static UINT32 g_msize = P9_MSIZE;
static int g_verbose = 0;

/* ---- Pack/unpack helpers ---- */

static void pack_u8(BYTE **p, UINT8 v)
{ **p = v; (*p)++; }

static void pack_u16(BYTE **p, UINT16 v)
{ memcpy(*p, &v, 2); *p += 2; }

static void pack_u32(BYTE **p, UINT32 v)
{ memcpy(*p, &v, 4); *p += 4; }

static void pack_u64(BYTE **p, UINT64 v)
{ memcpy(*p, &v, 8); *p += 8; }

static void pack_str(BYTE **p, const char *s)
{
    UINT16 len = (UINT16)strlen(s);
    pack_u16(p, len);
    memcpy(*p, s, len);
    *p += len;
}

static UINT8 unpack_u8(BYTE **p)
{ UINT8 v = **p; (*p)++; return v; }

static UINT16 unpack_u16(BYTE **p)
{ UINT16 v; memcpy(&v, *p, 2); *p += 2; return v; }

static UINT32 unpack_u32(BYTE **p)
{ UINT32 v; memcpy(&v, *p, 4); *p += 4; return v; }

static UINT64 unpack_u64(BYTE **p)
{ UINT64 v; memcpy(&v, *p, 8); *p += 8; return v; }

static void unpack_str(BYTE **p, char *out, int max)
{
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
        if (n <= 0) {
            fprintf(stderr, "send failed: %d\n", WSAGetLastError());
            return FALSE;
        }
        sent += n;
    }
    return TRUE;
}

static BOOL sock_recv_msg(void)
{
    UINT32 size;
    int recvd = 0;

    /* Read 4-byte size header */
    while (recvd < 4) {
        int n = recv(g_sock, (char *)(g_recvbuf + recvd), 4 - recvd, 0);
        if (n <= 0) {
            fprintf(stderr, "recv failed: %d\n", WSAGetLastError());
            return FALSE;
        }
        recvd += n;
    }
    memcpy(&size, g_recvbuf, 4);
    if (size < 7 || size > g_msize) {
        fprintf(stderr, "bad message size: %u\n", size);
        return FALSE;
    }

    /* Read rest of message */
    while (recvd < (int)size) {
        int n = recv(g_sock, (char *)(g_recvbuf + recvd), (int)size - recvd, 0);
        if (n <= 0) {
            fprintf(stderr, "recv failed: %d\n", WSAGetLastError());
            return FALSE;
        }
        recvd += n;
    }
    return TRUE;
}

/* Parse response header. Returns message type. Sets *payload to start of payload. */
static UINT8 msg_type(BYTE **payload)
{
    BYTE *p = g_recvbuf + 4;
    UINT8 type = unpack_u8(&p);
    /* skip tag */
    p += 2;
    *payload = p;
    return type;
}

/* Check for error responses */
static BOOL check_error(BYTE *payload, UINT8 type)
{
    if (type == P9_RLERROR) {
        UINT32 ecode = unpack_u32(&payload);
        fprintf(stderr, "9P error (errno %u)\n", ecode);
        return TRUE;
    }
    if (type == P9_RERROR) {
        char err[256];
        unpack_str(&payload, err, sizeof(err));
        fprintf(stderr, "9P error: %s\n", err);
        return TRUE;
    }
    return FALSE;
}

/* ---- 9P message builders ---- */

static UINT16 next_tag(void)
{
    return g_tag++;
}

/* Build message header. Returns pointer past header for payload. */
static BYTE *msg_begin(UINT8 type, UINT16 tag)
{
    BYTE *p = g_sendbuf + 4; /* skip size, fill in later */
    pack_u8(&p, type);
    pack_u16(&p, tag);
    return p;
}

/* Finalize and send message */
static BOOL msg_send(BYTE *end)
{
    UINT32 size = (UINT32)(end - g_sendbuf);
    memcpy(g_sendbuf, &size, 4);
    return sock_send(g_sendbuf, size);
}

/* ---- 9P protocol operations ---- */

static BOOL p9_version(void)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = P9_NOTAG;
    char ver[32];

    p = msg_begin(P9_TVERSION, tag);
    pack_u32(&p, g_msize);
    pack_str(&p, "9P2000.L");
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RVERSION) {
        fprintf(stderr, "expected Rversion, got %d\n", type);
        return FALSE;
    }

    g_msize = unpack_u32(&payload);
    unpack_str(&payload, ver, sizeof(ver));

    if (g_verbose)
        printf("version: %s, msize: %u\n", ver, g_msize);

    if (strcmp(ver, "9P2000.L") != 0) {
        fprintf(stderr, "server version '%s' not supported\n", ver);
        return FALSE;
    }
    return TRUE;
}

static BOOL p9_attach(UINT32 fid, const char *aname)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TATTACH, tag);
    pack_u32(&p, fid);          /* fid */
    pack_u32(&p, P9_NOFID);    /* afid (no auth) */
    pack_str(&p, "nobody");     /* uname */
    pack_str(&p, aname);        /* aname (share name) */
    pack_u32(&p, 65534);        /* n_uname (9P2000.L extension) */
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RATTACH) {
        fprintf(stderr, "expected Rattach, got %d\n", type);
        return FALSE;
    }
    /* skip qid[13] */
    return TRUE;
}

/* Walk from fid to newfid along path components.
   Returns TRUE on success. qid_type_out receives the QID type of the final element. */
static BOOL p9_walk(UINT32 fid, UINT32 newfid,
                    const char **names, int nnames, UINT8 *qid_type_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();
    UINT16 nwqid;
    int i;

    p = msg_begin(P9_TWALK, tag);
    pack_u32(&p, fid);
    pack_u32(&p, newfid);
    pack_u16(&p, (UINT16)nnames);
    for (i = 0; i < nnames; i++)
        pack_str(&p, names[i]);
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RWALK) {
        fprintf(stderr, "expected Rwalk, got %d\n", type);
        return FALSE;
    }

    nwqid = unpack_u16(&payload);
    if (nwqid != nnames && nnames > 0) {
        fprintf(stderr, "walk: expected %d qids, got %d\n", nnames, nwqid);
        return FALSE;
    }

    if (qid_type_out && nwqid > 0) {
        /* Skip to last QID: each is 13 bytes (type[1] version[4] path[8]) */
        payload += (nwqid - 1) * 13;
        *qid_type_out = unpack_u8(&payload);
    }
    return TRUE;
}

static BOOL p9_lopen(UINT32 fid, UINT32 flags, UINT32 *iounit_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TLOPEN, tag);
    pack_u32(&p, fid);
    pack_u32(&p, flags);
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RLOPEN) {
        fprintf(stderr, "expected Rlopen, got %d\n", type);
        return FALSE;
    }

    /* skip qid[13] */
    payload += 13;
    if (iounit_out)
        *iounit_out = unpack_u32(&payload);
    return TRUE;
}

static BOOL p9_read(UINT32 fid, UINT64 offset, UINT32 count,
                    BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TREAD, tag);
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RREAD) {
        fprintf(stderr, "expected Rread, got %d\n", type);
        return FALSE;
    }

    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_readdir(UINT32 fid, UINT64 offset, UINT32 count,
                       BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TREADDIR, tag);
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RREADDIR) {
        fprintf(stderr, "expected Rreaddir, got %d\n", type);
        return FALSE;
    }

    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_getattr(UINT32 fid, UINT32 *mode_out, UINT64 *size_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TGETATTR, tag);
    pack_u32(&p, fid);
    pack_u64(&p, P9_GETATTR_BASIC);
    if (!msg_send(p)) return FALSE;
    if (!sock_recv_msg()) return FALSE;

    type = msg_type(&payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RGETATTR) {
        fprintf(stderr, "expected Rgetattr, got %d\n", type);
        return FALSE;
    }

    /* Rgetattr layout:
       valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] ... */
    payload += 8;   /* valid */
    payload += 13;  /* qid */
    if (mode_out)
        *mode_out = unpack_u32(&payload);
    else
        payload += 4;
    payload += 4;   /* uid */
    payload += 4;   /* gid */
    payload += 8;   /* nlink */
    payload += 8;   /* rdev */
    if (size_out)
        *size_out = unpack_u64(&payload);
    return TRUE;
}

static void p9_clunk(UINT32 fid)
{
    BYTE *p;
    UINT16 tag = next_tag();

    p = msg_begin(P9_TCLUNK, tag);
    pack_u32(&p, fid);
    msg_send(p);
    sock_recv_msg(); /* consume response */
}

/* ---- High-level operations ---- */

static UINT32 alloc_fid(void)
{
    return g_next_fid++;
}

/* Create all directories along a path */
static void ensure_dir(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + 3; *p; p++) { /* skip drive letter C:\ */
        if (*p == L'\\' || *p == L'/') {
            *p = L'\0';
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, NULL);
}

/* Convert UTF-8 to wide string */
static void utf8_to_wide(const char *utf8, wchar_t *wide, int wide_max)
{
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, wide_max);
}

/* Copy a single file from the 9P share to local disk.
   parent_fid is the directory containing the file. */
static BOOL copy_file(UINT32 parent_fid, const char *name,
                      const wchar_t *local_path, UINT64 file_size)
{
    UINT32 fid = alloc_fid();
    UINT32 iounit;
    UINT64 offset = 0;
    HANDLE hfile;
    BOOL ok = TRUE;
    const char *names[1];

    /* Check if file exists and same size — skip if so */
    {
        WIN32_FILE_ATTRIBUTE_DATA attr;
        if (GetFileAttributesExW(local_path, GetFileExInfoStandard, &attr)) {
            UINT64 local_size = ((UINT64)attr.nFileSizeHigh << 32) | attr.nFileSizeLow;
            if (local_size == file_size) {
                if (g_verbose)
                    printf("  skip (exists): %s\n", name);
                return TRUE;
            }
        }
    }

    names[0] = name;
    if (!p9_walk(parent_fid, fid, names, 1, NULL))
        return FALSE;

    if (!p9_lopen(fid, 0 /* O_RDONLY */, &iounit)) {
        p9_clunk(fid);
        return FALSE;
    }

    if (iounit == 0 || iounit > g_msize - 24)
        iounit = g_msize - 24;

    hfile = CreateFileW(local_path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot create: %ls\n", local_path);
        p9_clunk(fid);
        return FALSE;
    }

    while (offset < file_size) {
        BYTE *data;
        UINT32 nread;
        DWORD written;

        if (!p9_read(fid, offset, iounit, &data, &nread)) {
            ok = FALSE;
            break;
        }
        if (nread == 0) break;

        if (!WriteFile(hfile, data, nread, &written, NULL) || written != nread) {
            ok = FALSE;
            break;
        }
        offset += nread;
    }

    CloseHandle(hfile);
    p9_clunk(fid);

    if (ok && g_verbose)
        printf("  copied: %s (%llu bytes)\n", name, (unsigned long long)file_size);
    return ok;
}

/* Recursively copy a directory from the 9P share to local disk.
   dir_fid must be an already-walked fid pointing to the directory.
   The caller is responsible for clunking dir_fid. */
static BOOL copy_dir_contents(UINT32 dir_fid, const wchar_t *local_dir)
{
    UINT32 open_fid = alloc_fid();
    UINT32 iounit;
    UINT64 dir_offset = 0;
    BOOL ok = TRUE;
    BYTE *dirbuf = NULL;

    ensure_dir(local_dir);

    dirbuf = (BYTE *)malloc(g_msize);
    if (!dirbuf) {
        fprintf(stderr, "out of memory for dir %ls\n", local_dir);
        return FALSE;
    }

    /* Clone the dir fid for opening (walk with 0 names = clone) */
    if (!p9_walk(dir_fid, open_fid, NULL, 0, NULL)) {
        free(dirbuf);
        return FALSE;
    }

    if (!p9_lopen(open_fid, 0, &iounit)) {
        p9_clunk(open_fid);
        free(dirbuf);
        return FALSE;
    }

    /* Read directory entries */
    for (;;) {
        BYTE *data, *dp, *end;
        UINT32 nread;

        if (!p9_readdir(open_fid, dir_offset, g_msize - 24, &data, &nread)) {
            ok = FALSE;
            break;
        }
        if (nread == 0) break;

        /* Copy readdir data to our own buffer — recursive calls and
           subsequent 9P ops overwrite g_recvbuf */
        memcpy(dirbuf, data, nread);
        dp = dirbuf;
        end = dirbuf + nread;

        while (dp < end) {
            /* Dirent: qid[13] offset[8] type[1] name[s] */
            UINT8 qid_type;
            UINT64 entry_offset;
            UINT8 dtype;
            char entry_name[512];
            wchar_t wide_name[512];
            wchar_t child_path[MAX_PATH];

            qid_type = unpack_u8(&dp);
            dp += 4 + 8; /* skip qid.version + qid.path */
            entry_offset = unpack_u64(&dp);
            dtype = unpack_u8(&dp);
            unpack_str(&dp, entry_name, sizeof(entry_name));

            dir_offset = entry_offset;

            /* Skip . and .. */
            if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
                continue;

            utf8_to_wide(entry_name, wide_name, 512);
            swprintf_s(child_path, MAX_PATH, L"%s\\%s", local_dir, wide_name);

            if (dtype == 4 /* DT_DIR */ || (qid_type & QTDIR)) {
                /* Recurse into subdirectory */
                UINT32 child_fid = alloc_fid();
                const char *names[1];
                names[0] = entry_name;

                if (g_verbose)
                    printf("  dir: %s\n", entry_name);

                if (p9_walk(dir_fid, child_fid, names, 1, NULL)) {
                    copy_dir_contents(child_fid, child_path);
                    p9_clunk(child_fid);
                }
            } else {
                /* Regular file — get size then copy */
                UINT32 child_fid = alloc_fid();
                const char *names[1];
                names[0] = entry_name;

                if (p9_walk(dir_fid, child_fid, names, 1, NULL)) {
                    UINT64 fsize = 0;
                    UINT32 fmode = 0;
                    p9_getattr(child_fid, &fmode, &fsize);
                    p9_clunk(child_fid);

                    copy_file(dir_fid, entry_name, child_path, fsize);
                }
            }
        }
    }

    p9_clunk(open_fid);
    free(dirbuf);
    return ok;
}

/* ---- Filtered copy (individual files only) ---- */

/* Copy specific files from the share root to local_dir.
   filter is a semicolon-separated list of filenames. */
static BOOL copy_filtered_files(UINT32 root_fid, const wchar_t *local_dir,
                                 const char *filter)
{
    char buf[4096];
    char *tok, *ctx;
    int copied = 0, failed = 0;

    ensure_dir(local_dir);

    /* Tokenize the semicolon-separated filter string */
    strncpy_s(buf, sizeof(buf), filter, _TRUNCATE);
    tok = strtok_s(buf, ";", &ctx);
    while (tok) {
        UINT32 fid = alloc_fid();
        const char *names[1];
        UINT64 fsize = 0;
        UINT32 fmode = 0;
        wchar_t wide_name[512];
        wchar_t local_path[MAX_PATH];

        names[0] = tok;

        /* Walk to file from root */
        if (!p9_walk(root_fid, fid, names, 1, NULL)) {
            fprintf(stderr, "walk failed for '%s'\n", tok);
            failed++;
            tok = strtok_s(NULL, ";", &ctx);
            continue;
        }

        /* Get file size */
        p9_getattr(fid, &fmode, &fsize);
        p9_clunk(fid);

        /* Build local path */
        utf8_to_wide(tok, wide_name, 512);
        swprintf_s(local_path, MAX_PATH, L"%s\\%s", local_dir, wide_name);

        /* Copy the file */
        if (copy_file(root_fid, tok, local_path, fsize))
            copied++;
        else
            failed++;

        tok = strtok_s(NULL, ";", &ctx);
    }

    printf("Filtered copy: %d copied, %d failed\n", copied, failed);
    return failed == 0;
}

/* ---- Connection ---- */

static BOOL connect_hvsocket(UINT32 port)
{
    SOCKADDR_HV addr;
    int ret;

    g_sock = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (g_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket(AF_HYPERV) failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    ZeroMemory(&addr, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_PARENT;
    addr.ServiceId = make_vsock_guid(port);

    ret = connect(g_sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        fprintf(stderr, "connect failed: %d\n", WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return FALSE;
    }

    return TRUE;
}

/* ---- TCP connection (for testing on host) ---- */

static BOOL connect_tcp(const char *host, UINT16 port)
{
    struct sockaddr_in addr;

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket(AF_INET) failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "TCP connect to %s:%d failed: %d\n", host, port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

/* ---- Main ---- */

int wmain(int argc, wchar_t *argv[])
{
    WSADATA wsa;
    char share_name[256];
    wchar_t target_path[MAX_PATH];
    UINT32 port = 50001;
    UINT32 root_fid;
    BOOL use_tcp = FALSE;
    char tcp_host[64] = "127.0.0.1";
    UINT16 tcp_port = 5640;
    char filter[4096] = "";
    int a;

    if (argc < 3) {
        fprintf(stderr, "Usage: p9client.exe <share_name> <target_path> [port] [--filter f1;f2;...]\n");
        fprintf(stderr, "       p9client.exe --tcp <host:port> <share_name> <target_path> [--filter f1;f2;...]\n");
        return 1;
    }

    /* Scan for -v/--verbose and --filter anywhere in args */
    for (a = 1; a < argc; a++) {
        if (wcscmp(argv[a], L"-v") == 0 || wcscmp(argv[a], L"--verbose") == 0)
            g_verbose = 1;
        if (wcscmp(argv[a], L"--filter") == 0 && a + 1 < argc)
            WideCharToMultiByte(CP_UTF8, 0, argv[a + 1], -1, filter, sizeof(filter), NULL, NULL);
    }

    /* Parse --tcp mode for testing */
    if (argc >= 5 && wcscmp(argv[1], L"--tcp") == 0) {
        char addr_buf[128];
        char *colon;
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, addr_buf, sizeof(addr_buf), NULL, NULL);
        colon = strchr(addr_buf, ':');
        if (colon) {
            *colon = '\0';
            strcpy_s(tcp_host, sizeof(tcp_host), addr_buf);
            tcp_port = (UINT16)atoi(colon + 1);
        }
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, share_name, sizeof(share_name), NULL, NULL);
        wcscpy_s(target_path, MAX_PATH, argv[4]);
        use_tcp = TRUE;
    } else {
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, share_name, sizeof(share_name), NULL, NULL);
        wcscpy_s(target_path, MAX_PATH, argv[2]);
        if (argc >= 4 && argv[3][0] != L'-')
            port = (UINT32)_wtoi(argv[3]);
    }

    WSAStartup(MAKEWORD(2, 2), &wsa);

    printf("Connecting to %s share '%s'...\n",
           use_tcp ? "TCP" : "HvSocket", share_name);

    if (use_tcp) {
        if (!connect_tcp(tcp_host, tcp_port)) goto fail;
    } else {
        if (!connect_hvsocket(port)) goto fail;
    }

    printf("Connected. Negotiating protocol...\n");

    if (!p9_version()) goto fail;

    root_fid = alloc_fid();
    if (!p9_attach(root_fid, share_name)) goto fail;

    if (filter[0])
        printf("Attached to share '%s'. Copying filtered files to %ls...\n", share_name, target_path);
    else
        printf("Attached to share '%s'. Copying to %ls...\n", share_name, target_path);

    if (filter[0]) {
        if (!copy_filtered_files(root_fid, target_path, filter)) {
            fprintf(stderr, "Filtered copy had failures (partial data may exist at target)\n");
            p9_clunk(root_fid);
            goto fail;
        }
    } else if (!copy_dir_contents(root_fid, target_path)) {
        fprintf(stderr, "Copy failed (partial data may exist at target)\n");
        p9_clunk(root_fid);
        goto fail;
    }

    p9_clunk(root_fid);
    printf("Done.\n");

    closesocket(g_sock);
    WSACleanup();
    return 0;

fail:
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    return 1;
}
