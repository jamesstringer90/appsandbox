/*
 * p9copy.c — Reusable 9P2000.L client for copying Plan9 shares via HvSocket.
 * All state is local to each p9_copy_share() call (no globals except the logger).
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p9copy.h"

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

/* Vsock port -> service GUID template: {port-FACB-11E6-BD58-64006A7986D3} */
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

/* ---- Per-session state (no globals) ---- */

typedef struct {
    BYTE     sendbuf[P9_MSIZE];
    BYTE     recvbuf[P9_MSIZE];
    SOCKET   sock;
    UINT16   tag;
    UINT32   next_fid;
    UINT32   msize;
    int      files_copied;
} P9Session;

/* ---- Logging ---- */

static P9LogFn g_p9log = NULL;

void p9_set_log(P9LogFn fn)
{
    g_p9log = fn;
}

#define P9LOG(...) do { if (g_p9log) g_p9log(__VA_ARGS__); } while(0)

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

static BOOL sock_send(P9Session *s, BYTE *data, int size)
{
    int sent = 0;
    while (sent < size) {
        int n = send(s->sock, (char *)(data + sent), size - sent, 0);
        if (n <= 0) {
            P9LOG("9P send failed: %d", WSAGetLastError());
            return FALSE;
        }
        sent += n;
    }
    return TRUE;
}

static BOOL sock_recv_msg(P9Session *s)
{
    UINT32 size;
    int recvd = 0;

    /* Read 4-byte size header */
    while (recvd < 4) {
        int n = recv(s->sock, (char *)(s->recvbuf + recvd), 4 - recvd, 0);
        if (n <= 0) {
            P9LOG("9P recv failed: %d", WSAGetLastError());
            return FALSE;
        }
        recvd += n;
    }
    memcpy(&size, s->recvbuf, 4);
    if (size < 7 || size > s->msize) {
        P9LOG("9P bad message size: %u", size);
        return FALSE;
    }

    /* Read rest of message */
    while (recvd < (int)size) {
        int n = recv(s->sock, (char *)(s->recvbuf + recvd), (int)size - recvd, 0);
        if (n <= 0) {
            P9LOG("9P recv failed: %d", WSAGetLastError());
            return FALSE;
        }
        recvd += n;
    }
    return TRUE;
}

/* Parse response header. Returns message type. Sets *payload to start of payload. */
static UINT8 msg_type(P9Session *s, BYTE **payload)
{
    BYTE *p = s->recvbuf + 4;
    UINT8 type = unpack_u8(&p);
    p += 2; /* skip tag */
    *payload = p;
    return type;
}

static BOOL check_error(BYTE *payload, UINT8 type)
{
    if (type == P9_RLERROR) {
        UINT32 ecode = unpack_u32(&payload);
        P9LOG("9P error (errno %u)", ecode);
        return TRUE;
    }
    if (type == P9_RERROR) {
        char err[256];
        unpack_str(&payload, err, sizeof(err));
        P9LOG("9P error: %s", err);
        return TRUE;
    }
    return FALSE;
}

/* ---- 9P message builders ---- */

static UINT16 next_tag(P9Session *s)
{
    return s->tag++;
}

static BYTE *msg_begin(P9Session *s, UINT8 type, UINT16 tag)
{
    BYTE *p = s->sendbuf + 4;
    pack_u8(&p, type);
    pack_u16(&p, tag);
    return p;
}

static BOOL msg_send(P9Session *s, BYTE *end)
{
    UINT32 size = (UINT32)(end - s->sendbuf);
    memcpy(s->sendbuf, &size, 4);
    return sock_send(s, s->sendbuf, size);
}

static UINT32 alloc_fid(P9Session *s)
{
    return s->next_fid++;
}

/* ---- 9P protocol operations ---- */

static BOOL p9_version(P9Session *s)
{
    BYTE *p, *payload;
    UINT8 type;
    char ver[32];

    p = msg_begin(s, P9_TVERSION, P9_NOTAG);
    pack_u32(&p, s->msize);
    pack_str(&p, "9P2000.L");
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RVERSION) {
        P9LOG("9P expected Rversion, got %d", type);
        return FALSE;
    }

    s->msize = unpack_u32(&payload);
    unpack_str(&payload, ver, sizeof(ver));

    P9LOG("9P version: %s, msize: %u", ver, s->msize);

    if (strcmp(ver, "9P2000.L") != 0) {
        P9LOG("9P server version '%s' not supported", ver);
        return FALSE;
    }
    return TRUE;
}

static BOOL p9_attach(P9Session *s, UINT32 fid, const char *aname)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TATTACH, tag);
    pack_u32(&p, fid);
    pack_u32(&p, P9_NOFID);
    pack_str(&p, "nobody");
    pack_str(&p, aname);
    pack_u32(&p, 65534);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RATTACH) {
        P9LOG("9P expected Rattach, got %d", type);
        return FALSE;
    }
    return TRUE;
}

static BOOL p9_walk(P9Session *s, UINT32 fid, UINT32 newfid,
                    const char **names, int nnames, UINT8 *qid_type_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);
    UINT16 nwqid;
    int i;

    p = msg_begin(s, P9_TWALK, tag);
    pack_u32(&p, fid);
    pack_u32(&p, newfid);
    pack_u16(&p, (UINT16)nnames);
    for (i = 0; i < nnames; i++)
        pack_str(&p, names[i]);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RWALK) {
        P9LOG("9P expected Rwalk, got %d", type);
        return FALSE;
    }

    nwqid = unpack_u16(&payload);
    if (nwqid != nnames && nnames > 0) {
        P9LOG("9P walk: expected %d qids, got %d", nnames, nwqid);
        return FALSE;
    }

    if (qid_type_out && nwqid > 0) {
        payload += (nwqid - 1) * 13;
        *qid_type_out = unpack_u8(&payload);
    }
    return TRUE;
}

static BOOL p9_lopen(P9Session *s, UINT32 fid, UINT32 flags, UINT32 *iounit_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TLOPEN, tag);
    pack_u32(&p, fid);
    pack_u32(&p, flags);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RLOPEN) {
        P9LOG("9P expected Rlopen, got %d", type);
        return FALSE;
    }

    payload += 13; /* skip qid */
    if (iounit_out)
        *iounit_out = unpack_u32(&payload);
    return TRUE;
}

static BOOL p9_read(P9Session *s, UINT32 fid, UINT64 offset, UINT32 count,
                    BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TREAD, tag);
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RREAD) {
        P9LOG("9P expected Rread, got %d", type);
        return FALSE;
    }

    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_readdir(P9Session *s, UINT32 fid, UINT64 offset, UINT32 count,
                       BYTE **data_out, UINT32 *nread_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TREADDIR, tag);
    pack_u32(&p, fid);
    pack_u64(&p, offset);
    pack_u32(&p, count);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RREADDIR) {
        P9LOG("9P expected Rreaddir, got %d", type);
        return FALSE;
    }

    *nread_out = unpack_u32(&payload);
    *data_out = payload;
    return TRUE;
}

static BOOL p9_getattr(P9Session *s, UINT32 fid, UINT32 *mode_out, UINT64 *size_out)
{
    BYTE *p, *payload;
    UINT8 type;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TGETATTR, tag);
    pack_u32(&p, fid);
    pack_u64(&p, P9_GETATTR_BASIC);
    if (!msg_send(s, p)) return FALSE;
    if (!sock_recv_msg(s)) return FALSE;

    type = msg_type(s, &payload);
    if (check_error(payload, type)) return FALSE;
    if (type != P9_RGETATTR) {
        P9LOG("9P expected Rgetattr, got %d", type);
        return FALSE;
    }

    /* Rgetattr: valid[8] qid[13] mode[4] uid[4] gid[4] nlink[8] rdev[8] size[8] ... */
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

static void p9_clunk(P9Session *s, UINT32 fid)
{
    BYTE *p;
    UINT16 tag = next_tag(s);

    p = msg_begin(s, P9_TCLUNK, tag);
    pack_u32(&p, fid);
    msg_send(s, p);
    sock_recv_msg(s); /* consume response */
}

/* ---- High-level copy operations ---- */

static void ensure_dir(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    wchar_t *p;
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

/* Copy a single file from the 9P share to local disk. */
static BOOL copy_file(P9Session *s, UINT32 parent_fid, const char *name,
                      const wchar_t *local_path, UINT64 file_size)
{
    UINT32 fid = alloc_fid(s);
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
                P9LOG("9P skip (exists, same size): %s", name);
                return TRUE;
            }
        }
    }

    names[0] = name;
    if (!p9_walk(s, parent_fid, fid, names, 1, NULL))
        return FALSE;

    if (!p9_lopen(s, fid, 0 /* O_RDONLY */, &iounit)) {
        p9_clunk(s, fid);
        return FALSE;
    }

    if (iounit == 0 || iounit > s->msize - 24)
        iounit = s->msize - 24;

    hfile = CreateFileW(local_path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE) {
        P9LOG("9P cannot create file: %ls (error %lu)", local_path, GetLastError());
        p9_clunk(s, fid);
        return FALSE;
    }

    while (offset < file_size) {
        BYTE *data;
        UINT32 nread;
        DWORD written;

        if (!p9_read(s, fid, offset, iounit, &data, &nread)) {
            ok = FALSE;
            break;
        }
        if (nread == 0) break;

        if (!WriteFile(hfile, data, nread, &written, NULL) || written != nread) {
            P9LOG("9P write failed for %s (error %lu)", name, GetLastError());
            ok = FALSE;
            break;
        }
        offset += nread;
    }

    CloseHandle(hfile);
    p9_clunk(s, fid);

    if (ok) {
        s->files_copied++;
        P9LOG("9P copied: %s (%llu bytes)", name, (unsigned long long)file_size);
    }
    return ok;
}

/* Recursively copy a directory from the 9P share to local disk. */
static BOOL copy_dir_contents(P9Session *s, UINT32 dir_fid, const wchar_t *local_dir)
{
    UINT32 open_fid = alloc_fid(s);
    UINT32 iounit;
    UINT64 dir_offset = 0;
    BOOL ok = TRUE;
    BYTE *dirbuf = NULL;

    ensure_dir(local_dir);

    dirbuf = (BYTE *)malloc(s->msize);
    if (!dirbuf) {
        P9LOG("9P out of memory for dir %ls", local_dir);
        return FALSE;
    }

    /* Clone the dir fid for opening (walk with 0 names = clone) */
    if (!p9_walk(s, dir_fid, open_fid, NULL, 0, NULL)) {
        free(dirbuf);
        return FALSE;
    }

    if (!p9_lopen(s, open_fid, 0, &iounit)) {
        p9_clunk(s, open_fid);
        free(dirbuf);
        return FALSE;
    }

    /* Read directory entries */
    for (;;) {
        BYTE *data, *dp, *end;
        UINT32 nread;

        if (!p9_readdir(s, open_fid, dir_offset, s->msize - 24, &data, &nread)) {
            ok = FALSE;
            break;
        }
        if (nread == 0) break;

        /* Copy readdir data to our own buffer — recursive calls and
           subsequent 9P ops overwrite recvbuf */
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
            dp += 4 + 8; /* skip qid.version + qid.path */
            entry_offset = unpack_u64(&dp);
            dtype = unpack_u8(&dp);
            unpack_str(&dp, entry_name, sizeof(entry_name));

            dir_offset = entry_offset;

            if (strcmp(entry_name, ".") == 0 || strcmp(entry_name, "..") == 0)
                continue;

            utf8_to_wide(entry_name, wide_name, 512);
            swprintf_s(child_path, MAX_PATH, L"%s\\%s", local_dir, wide_name);

            if (dtype == 4 /* DT_DIR */ || (qid_type & QTDIR)) {
                UINT32 child_fid = alloc_fid(s);
                const char *names[1];
                names[0] = entry_name;

                P9LOG("9P dir: %s", entry_name);

                if (p9_walk(s, dir_fid, child_fid, names, 1, NULL)) {
                    copy_dir_contents(s, child_fid, child_path);
                    p9_clunk(s, child_fid);
                }
            } else {
                UINT32 child_fid = alloc_fid(s);
                const char *names[1];
                names[0] = entry_name;

                if (p9_walk(s, dir_fid, child_fid, names, 1, NULL)) {
                    UINT64 fsize = 0;
                    UINT32 fmode = 0;
                    p9_getattr(s, child_fid, &fmode, &fsize);
                    p9_clunk(s, child_fid);

                    copy_file(s, dir_fid, entry_name, child_path, fsize);
                }
            }
        }
    }

    p9_clunk(s, open_fid);
    free(dirbuf);
    return ok;
}

/* Copy specific files from the share root (semicolon-separated filter). */
static BOOL copy_filtered_files(P9Session *s, UINT32 root_fid,
                                const wchar_t *local_dir, const char *filter)
{
    char buf[4096];
    char *tok, *ctx;
    int failed = 0;

    ensure_dir(local_dir);

    strncpy_s(buf, sizeof(buf), filter, _TRUNCATE);
    tok = strtok_s(buf, ";", &ctx);
    while (tok) {
        UINT32 fid = alloc_fid(s);
        const char *names[1];
        UINT64 fsize = 0;
        UINT32 fmode = 0;
        wchar_t wide_name[512];
        wchar_t local_path[MAX_PATH];

        names[0] = tok;

        if (!p9_walk(s, root_fid, fid, names, 1, NULL)) {
            P9LOG("9P walk failed for '%s'", tok);
            failed++;
            tok = strtok_s(NULL, ";", &ctx);
            continue;
        }

        p9_getattr(s, fid, &fmode, &fsize);
        p9_clunk(s, fid);

        utf8_to_wide(tok, wide_name, 512);
        swprintf_s(local_path, MAX_PATH, L"%s\\%s", local_dir, wide_name);

        if (!copy_file(s, root_fid, tok, local_path, fsize))
            failed++;

        tok = strtok_s(NULL, ";", &ctx);
    }

    if (failed > 0)
        P9LOG("9P filtered copy: %d failures", failed);
    return failed == 0;
}

/* ---- Connection ---- */

static SOCKET connect_hvsocket(UINT32 port)
{
    SOCKADDR_HV addr;
    SOCKET sock;

    sock = socket(AF_HYPERV, SOCK_STREAM, 1 /* HV_PROTOCOL_RAW */);
    if (sock == INVALID_SOCKET) {
        P9LOG("9P socket(AF_HYPERV) failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    ZeroMemory(&addr, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = HV_GUID_PARENT;
    addr.ServiceId = make_vsock_guid(port);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        P9LOG("9P connect failed: %d", WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }

    return sock;
}

/* ---- Public API ---- */

int p9_copy_share(UINT32 port, const char *share_name,
                  const wchar_t *local_dir, const char *filter,
                  int *files_copied)
{
    P9Session s;
    UINT32 root_fid;
    int result = P9_OK;

    memset(&s, 0, sizeof(s));
    s.sock = INVALID_SOCKET;
    s.tag = 1;
    s.next_fid = 10;
    s.msize = P9_MSIZE;
    s.files_copied = 0;

    P9LOG("9P connecting to share '%s' on port %u...", share_name, port);

    s.sock = connect_hvsocket(port);
    if (s.sock == INVALID_SOCKET) {
        result = P9_ERR_CONN;
        goto done;
    }

    P9LOG("9P connected. Negotiating protocol...");

    if (!p9_version(&s)) {
        result = P9_ERR_PROTO;
        goto done;
    }

    root_fid = alloc_fid(&s);
    if (!p9_attach(&s, root_fid, share_name)) {
        P9LOG("9P attach to '%s' failed", share_name);
        result = P9_ERR_PROTO;
        goto done;
    }

    P9LOG("9P attached to '%s'. Copying to %ls...", share_name, local_dir);

    if (filter && filter[0]) {
        if (!copy_filtered_files(&s, root_fid, local_dir, filter))
            result = P9_ERR_IO;
    } else {
        if (!copy_dir_contents(&s, root_fid, local_dir))
            result = P9_ERR_IO;
    }

    p9_clunk(&s, root_fid);

    if (result == P9_OK)
        P9LOG("9P share '%s' copy complete (%d files).", share_name, s.files_copied);

done:
    if (s.sock != INVALID_SOCKET)
        closesocket(s.sock);
    if (files_copied)
        *files_copied = s.files_copied;
    return result;
}
