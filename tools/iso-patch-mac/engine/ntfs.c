/*
 * ntfs.c -- NTFS volume writer. See ntfs.h. Mirrors ext4.c's three-phase
 * discipline: open() plans + reserves metafiles, add_*() buffers FILE records
 * and streams non-resident $DATA, close() serializes everything with USA fixups.
 *
 * Scope: single contiguous $DATA run per file; directories in $INDEX_ROOT, with
 * an $INDEX_ALLOCATION B-tree for larger directories; a small interned $Secure
 * descriptor set; NTFS 3.1.
 */
#include "ntfs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ helpers */
static void w16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
static void w32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void w64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint16_t r16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }
static uint32_t r32(const uint8_t *p){ return (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24)); }
static uint32_t align8u(uint32_t x){ return (x+7)&~7u; }
static uint64_t align_up(uint64_t x, uint64_t a){ return (x + a - 1) / a * a; }

static int min_bytes_u(uint64_t v){ int n=1; while (v >>= 8) n++; return n; }
static int min_bytes_s(int64_t v){
    int n=1; uint64_t u=(uint64_t)v;
    /* smallest n in [1,8] s.t. sign-extending the low n bytes reproduces v */
    for (n=1;n<8;n++){ int64_t lo = (int64_t)(u << (64-8*n)) >> (64-8*n); if (lo==v) break; }
    return n;
}
/* Encode one contiguous run [first_lcn,count] as a single-run mapping-pairs list
 * (prev lcn assumed 0). Returns bytes written (incl the 0 terminator). */
static int enc_single_run(uint8_t *out, uint64_t count, uint64_t first_lcn){
    /* The run-LENGTH field is read SIGNED by strict NTFS drivers (incl. the
     * bootmgr/winload micro-reader): a length byte >= 0x80 with no pad byte
     * sign-extends NEGATIVE and the runlist is rejected (-> winload load fails
     * 0xc00000bb). Size it signed-significant, exactly like the offset field
     * and the signed-significant byte count. count is always >= 0. */
    int lb = min_bytes_s((int64_t)count), ob = min_bytes_s((int64_t)first_lcn);
    out[0] = (uint8_t)(lb | (ob<<4));
    int p=1;
    for (int i=0;i<lb;i++) out[p++] = (uint8_t)(count >> (8*i));
    for (int i=0;i<ob;i++) out[p++] = (uint8_t)(first_lcn >> (8*i));
    out[p++] = 0;
    return p;
}

/* USA / fixup: stamp the last 2 bytes of each `sectsz` sector with `usn`, saving
 * the originals into the USA. `size` and usa fields already set in the header. */
static void apply_fixup(uint8_t *rec, uint32_t size, uint32_t sectsz, uint16_t usn){
    if (usn == 0 || usn == 0xFFFF) usn = 1;   /* 0 and 0xFFFF are reserved USN sentinels (recno 65534/65535, +65536) */
    uint16_t usa_off = r16(rec+4), usa_cnt = r16(rec+6);
    uint8_t *usa = rec + usa_off;
    w16(usa, usn);
    for (uint32_t i=0; i+1 < usa_cnt; i++){
        uint8_t *q = rec + (i+1)*sectsz - 2;
        usa[2+2*i] = q[0]; usa[2+2*i+1] = q[1];
        q[0] = (uint8_t)usn; q[1] = (uint8_t)(usn>>8);
    }
    (void)size;
}

/* NTFS security-descriptor hash (the $SDH/$SII hash Windows computes). */
static uint32_t sec_hash(const uint8_t *sd, uint32_t len){
    uint32_t h=0, n=len/4;
    for (uint32_t i=0;i<n;i++){
        uint32_t d = sd[4*i]|(sd[4*i+1]<<8)|(sd[4*i+2]<<16)|((uint32_t)sd[4*i+3]<<24);
        h = d + ((h<<3)|(h>>29));
    }
    return h;
}

/* ------------------------------------------------------------ in-memory model */
typedef struct {
    uint8_t  *key;          /* $FILE_NAME body (also the index key) */
    uint16_t  key_len;
    uint64_t  file_ref;
} idx_entry_t;

typedef struct {
    uint8_t   buf[NTFS_MFT_RECSZ];
    uint32_t  used;         /* offset where the next attribute goes */
    uint16_t  attr_id;
    uint16_t  seq;
    uint16_t  flags;        /* IN_USE | DIRECTORY */
    uint16_t  link_count;
    int       is_dir;
    /* directory index accumulator (dirs only) */
    idx_entry_t *entries;
    uint32_t  nentries, cap_entries;
    /* optional $REPARSE_POINT (0xC0) value: full attribute body
     * (tag|len|reserved|data), appended at close after the index attrs. */
    uint8_t  *reparse; uint32_t reparse_len;
    /* overflow $FILE_NAME bodies for a heavily hard-linked inode: when more names
     * are added than fit in the 1024B base record, they accumulate here and are
     * spilled into $ATTRIBUTE_LIST + extension records at close (matches the
     * reference instead of flattening into duplicate inodes). */
    uint8_t **ov; uint32_t *ov_len; uint32_t n_ov, cap_ov;
    /* extension record: MFT reference of its base inode (0 = base/normal). Written
     * to the FILE header base_record_ref (offset 0x20) at finalize. */
    uint64_t  base_ref;
} mftrec_t;

typedef struct {
    uint8_t  *sd; uint32_t sd_len;
    uint32_t  hash; int32_t id;
    uint64_t  sds_offset;
} sec_desc_t;

struct ntfs_writer {
    blockio_t *io;
    uint64_t   part_off;          /* byte offset of partition 0 in the image */
    uint64_t   part_sectors;
    uint32_t   sector_size, cluster_size, spc;
    uint64_t   total_clusters;
    uint64_t   next_lcn;          /* data-zone bump allocator (starts at mft_zone_end) */
    uint64_t   serial;

    /* NTFS-faithful cluster layout: keep the MFT low with a FREE zone after it
     * (so Windows, which derives the MFT zone as mft_lcn + nr_clusters/8, has a
     * large contiguous region to grow the MFT into during specialize), and place
     * all file data after the zone. A real allocation bitmap tracks every cluster
     * (the on-disk $Bitmap is this verbatim) since the layout is no longer a single
     * contiguous span from 0. */
    uint8_t   *clbmp;             /* in-memory cluster allocation bitmap */
    uint64_t   clbmp_bytes;       /* its size = on-disk $Bitmap data length */
    uint64_t   mft_zone_end;      /* data allocations start here */
    uint64_t   reserve_lcn, reserve_len;  /* block the bump must skip ($MFTMirr @ midpoint) */

    mftrec_t **rec;               /* rec[i] = MFT record i (or NULL) */
    uint32_t   nrec, cap_rec;

    sec_desc_t *sec; uint32_t nsec, cap_sec;

    /* reparse points -> $Extend\$Reparse:$R index ({tag,file_ref} keys) */
    struct { uint32_t tag; uint64_t ref; } *rpl; uint32_t n_rpl, cap_rpl;

    char      *label; int label_chars;
    int32_t    root_sec_id;       /* security_id for the root dir (WIM root's SD) */
    uint32_t   root_attrs;        /* root dir file_attributes (from the WIM root) */

    /* resolved at close */
    uint64_t mft_lcn, mft_clusters, mftmirr_lcn, logfile_lcn, logfile_clusters;
    uint64_t upcase_lcn, attrdef_lcn, attrdef_clusters, sds_lcn, sds_clusters, bitmap_lcn, bitmap_clusters;
};

static void bmp_mark(ntfs_writer_t *w, uint64_t lcn, uint64_t n){
    for (uint64_t c=lcn; c<lcn+n; c++) w->clbmp[c>>3] |= 1u << (c&7);
}
static uint64_t alloc_clusters(ntfs_writer_t *w, uint64_t n){
    /* skip the reserved midpoint block ($MFTMirr) if this run would intersect it */
    if (w->reserve_len && w->next_lcn < w->reserve_lcn + w->reserve_len &&
        w->next_lcn + n > w->reserve_lcn)
        w->next_lcn = w->reserve_lcn + w->reserve_len;
    uint64_t lcn = w->next_lcn; w->next_lcn += n;
    bmp_mark(w, lcn, n);
    return lcn;
}
static uint64_t cluster_byte(ntfs_writer_t *w, uint64_t lcn){ return w->part_off + lcn * w->cluster_size; }

/* ------------------------------------------------------------ record builders */
static mftrec_t *rec_new(ntfs_writer_t *w, uint32_t recno, int is_dir){
    if (recno >= w->cap_rec){
        uint32_t nc = w->cap_rec ? w->cap_rec*2 : 64;
        while (nc <= recno) nc *= 2;
        w->rec = realloc(w->rec, nc*sizeof(*w->rec));
        for (uint32_t i=w->cap_rec;i<nc;i++) w->rec[i]=NULL;
        w->cap_rec = nc;
    }
    mftrec_t *r = calloc(1, sizeof *r);
    /* sequence_number: system records 1..23 use their record number (matches
     * Windows); $MFT(0) and user records (>=24) use 1. All file references
     * are built from r->seq, so this stays self-consistent. */
    r->seq = (recno >= 1 && recno <= 23) ? (uint16_t)recno : 1;
    r->link_count = 1; r->is_dir = is_dir;
    r->flags = NTFS_FR_IN_USE | (is_dir ? NTFS_FR_DIRECTORY : 0);
    uint8_t *b = r->buf;
    memcpy(b, "FILE", 4);
    w16(b+4, 0x30);                 /* usa_offset */
    w16(b+6, 3);                    /* usa_count (1024B = 2 sectors -> 3) */
    w64(b+8, 0);                    /* $LogFile LSN */
    w16(b+0x10, r->seq);
    w16(b+0x12, r->link_count);
    w16(b+0x14, 0x38);              /* first_attr_offset */
    w16(b+0x16, r->flags);
    w32(b+0x1C, NTFS_MFT_RECSZ);    /* bytes_allocated */
    w64(b+0x20, 0);                 /* base_record_ref */
    w16(b+0x28, 0);                 /* next_attribute_id (set at finalize) */
    w32(b+0x2C, recno);             /* this record number */
    r->used = 0x38;
    r->attr_id = 0;
    if (recno >= w->nrec) w->nrec = recno+1;
    w->rec[recno] = r;
    return r;
}

/* Append a resident attribute. Returns 0/-1 (overflow). */
static int attr_resident(mftrec_t *r, uint32_t type, const uint16_t *name, int name_chars,
                         const void *val, uint32_t val_len, uint8_t indexed){
    uint32_t off = r->used;
    uint32_t name_off = 0x18;
    uint32_t val_off  = align8u(name_off + 2*name_chars);
    uint32_t total    = align8u(val_off + val_len);
    if (off + total + 8 > NTFS_MFT_RECSZ) return -1;   /* +8 for end marker headroom */
    uint8_t *a = r->buf + off;
    memset(a, 0, total);
    w32(a+0x00, type);
    w32(a+0x04, total);
    a[0x08] = 0;                    /* resident */
    a[0x09] = (uint8_t)name_chars;
    w16(a+0x0A, (uint16_t)name_off);
    w16(a+0x0E, r->attr_id++);
    w32(a+0x10, val_len);
    w16(a+0x14, (uint16_t)val_off);
    /* $FILE_NAME (0x30) is ALWAYS indexed in the parent directory's $I30, so its
     * resident attribute MUST carry RESIDENT_ATTR_IS_INDEXED (0x01) -- Windows sets
     * this on every resident $FILE_NAME. Emitting 0x00 here would be an
     * inconsistency the kernel rejects at mount. */
    a[0x16] = indexed | (type == 0x30 ? 0x01 : 0x00);
    if (name_chars) memcpy(a+name_off, name, 2*name_chars);
    if (val_len)    memcpy(a+val_off, val, val_len);
    r->used = off + total;
    return 0;
}

/* Insert a $FILE_NAME (0x30) resident attribute into a record that already has
 * its $DATA/index attributes, keeping attributes in ascending type order (the new
 * 0x30 goes after existing $STD_INFO/$FILE_NAME, before $DATA/$INDEX_*). Used to
 * add hard-link names to an existing inode. Returns 0, or -1 if it would overflow
 * (caller falls back to flattening this link). */
static int attr_insert_filename(mftrec_t *r, const uint8_t *fn, uint32_t fn_len){
    uint32_t val_off = 0x18;                         /* no attribute name */
    uint32_t total   = align8u(val_off + fn_len);
    if (r->used + total + 8 > NTFS_MFT_RECSZ) return -1;
    /* insertion point: first attribute whose type > 0x30 (i.e. $DATA/$INDEX_*) */
    uint32_t p = 0x38;
    while (p < r->used){
        uint32_t t = r->buf[p]|(r->buf[p+1]<<8)|(r->buf[p+2]<<16)|((uint32_t)r->buf[p+3]<<24);
        if (t == 0xFFFFFFFF || t > 0x30) break;
        uint32_t l = r->buf[p+4]|(r->buf[p+5]<<8)|(r->buf[p+6]<<16)|((uint32_t)r->buf[p+7]<<24);
        if (l == 0) break;
        p += l;
    }
    memmove(r->buf + p + total, r->buf + p, r->used - p);
    uint8_t *a = r->buf + p;
    memset(a, 0, total);
    w32(a+0x00, NTFS_AT_FILE_NAME);
    w32(a+0x04, total);
    a[0x08] = 0;                    /* resident */
    a[0x09] = 0;                    /* no name */
    w16(a+0x0A, (uint16_t)val_off);
    w16(a+0x0E, r->attr_id++);
    w32(a+0x10, fn_len);
    w16(a+0x14, (uint16_t)val_off);
    a[0x16] = 0x01;                 /* RESIDENT_ATTR_IS_INDEXED ($FILE_NAME) */
    memcpy(a+val_off, fn, fn_len);
    r->used += total;
    return 0;
}

/* Stash an overflow $FILE_NAME body (for a heavily hard-linked inode) to be
 * spilled into $ATTRIBUTE_LIST + extension records at close. */
static void store_ov(mftrec_t *r, const uint8_t *body, uint32_t len){
    if (r->n_ov == r->cap_ov){
        r->cap_ov = r->cap_ov ? r->cap_ov*2 : 8;
        r->ov     = realloc(r->ov, r->cap_ov*sizeof(*r->ov));
        r->ov_len = realloc(r->ov_len, r->cap_ov*sizeof(*r->ov_len));
    }
    uint8_t *b = malloc(len); memcpy(b, body, len);
    r->ov[r->n_ov] = b; r->ov_len[r->n_ov] = len; r->n_ov++;
}

/* Append a non-resident attribute mapping a single contiguous run. */
static int attr_nonres(mftrec_t *r, uint32_t type, const uint16_t *name, int name_chars,
                       uint64_t first_lcn, uint64_t nclusters, uint32_t cluster_size,
                       uint64_t data_size, uint64_t init_size, uint16_t attr_flags){
    uint32_t off = r->used;
    uint32_t name_off = 0x40;
    uint32_t mp_off   = align8u(name_off + 2*name_chars);
    uint8_t  run[32];
    int      run_len = enc_single_run(run, nclusters, first_lcn);
    uint32_t total   = align8u(mp_off + run_len);
    if (off + total + 8 > NTFS_MFT_RECSZ) return -1;
    uint8_t *a = r->buf + off;
    memset(a, 0, total);
    w32(a+0x00, type);
    w32(a+0x04, total);
    a[0x08] = 1;                    /* non-resident */
    a[0x09] = (uint8_t)name_chars;
    w16(a+0x0A, (uint16_t)name_off);
    w16(a+0x0C, attr_flags);
    w16(a+0x0E, r->attr_id++);
    w64(a+0x10, 0);                 /* lowest_vcn */
    w64(a+0x18, nclusters ? nclusters-1 : 0); /* highest_vcn */
    w16(a+0x20, (uint16_t)mp_off);
    a[0x22] = 0;                    /* compression unit */
    w64(a+0x28, nclusters * (uint64_t)cluster_size); /* allocated */
    w64(a+0x30, data_size);
    w64(a+0x38, init_size);
    if (name_chars) memcpy(a+name_off, name, 2*name_chars);
    memcpy(a+mp_off, run, run_len);
    r->used = off + total;
    return 0;
}

static void rec_finalize(mftrec_t *r){
    w32(r->buf + r->used, NTFS_AT_END);
    uint32_t in_use = align8u(r->used + 4);
    w32(r->buf + 0x18, in_use);
    w16(r->buf + 0x16, r->flags);
    w16(r->buf + 0x12, r->link_count);
    w16(r->buf + 0x28, r->attr_id);
    w64(r->buf + 0x20, r->base_ref);   /* extension records point at their base */
}

/* ------------------------------------------------------------- attr payloads */
static uint32_t to_disk_attrs(uint32_t win_attrs, int is_dir){
    /* Keep ONLY the FILE_ATTR_SETTABLE bits (0x3127 = READONLY |
     * HIDDEN | SYSTEM | ARCHIVE | TEMPORARY | OFFLINE | NOT_CONTENT_INDEXED) of
     * the requested DOS attributes. The structural bits (DIRECTORY, REPARSE,
     * COMPRESSED, SPARSE, ENCRYPTED) are NOT taken from the WIM word; the driver
     * derives them from the file's actual on-disk state. We write every $DATA
     * PLAIN, so we never set COMPRESSED/SPARSE/ENCRYPTED; we DO honor REPARSE
     * (set on the 2 reparse points) and the internal directory/I30 bit.
     * Non-settable bits like 0x80 NORMAL and 0x800 COMPRESSED do not belong in
     * the settable set and are masked off. */
    uint32_t a = win_attrs & 0x3127u;       /* FILE_ATTR_SETTABLE */
    if (win_attrs & 0x400u) a |= 0x400u;     /* FILE_ATTR_REPARSE_POINT (structural, honored) */
    if (is_dir)             a |= 0x10000000u;/* FILE_ATTR_I30_INDEX_PRESENT */
    return a;
}

/* Build $STANDARD_INFORMATION. Returns the byte length: 48 (NTFS 1.2 layout, no
 * owner_id/security_id/quota/usn) when security_id==0, else 72 (NTFS 3.x). Records
 * use 48 iff security_id==0: metafiles
 * that carry a standalone $SECURITY_DESCRIPTOR get security_id 0 + 48-byte SI. */
static uint32_t build_std_info(uint8_t out[72], uint64_t c,uint64_t a,uint64_t m,
                               uint32_t disk_attrs, int32_t security_id){
    memset(out,0,72);
    w64(out+0x00, c); w64(out+0x08, m); w64(out+0x10, m); w64(out+0x18, a);
    /* FILE_ATTR_I30_INDEX_PRESENT (0x10000000) belongs ONLY in $FILE_NAME, never in
     * $STANDARD_INFORMATION. Real Windows omits it here; emitting it on a dir
     * (root + $Extend included) is a malformed dos-attr the kernel rejects at mount. */
    w32(out+0x20, disk_attrs & ~0x10000000u);
    if (security_id == 0) return 48;
    w32(out+0x34, (uint32_t)security_id);
    return 72;
}

/* Build the metafile $SECURITY_DESCRIPTOR (the system-file default SD):
 * 0x64 bytes, Owner=SYSTEM, Group=BUILTIN\Administrators, DACL = {SYSTEM, Admins}. */
static uint32_t build_metafile_sd(uint8_t out[0x64], int read_only) __attribute__((unused));
static uint32_t build_metafile_sd(uint8_t out[0x64], int read_only){
    memset(out, 0, 0x64);
    /* SECURITY_DESCRIPTOR_RELATIVE */
    out[0]=1;                         /* revision */
    w16(out+2, 0x8004);               /* control = SE_SELF_RELATIVE (0x8000) | SE_DACL_PRESENT (0x0004) */
    w32(out+4, 0x48);                 /* owner offset */
    w32(out+8, 0x54);                 /* group offset */
    w32(out+0x0C, 0);                 /* sacl */
    w32(out+0x10, 0x14);              /* dacl offset */
    /* ACL @0x14 */
    uint8_t *acl = out+0x14;
    acl[0]=2;                         /* ACL revision */
    w16(acl+2, 0x34);                 /* acl size */
    w16(acl+4, 2);                    /* ace_count */
    /* exact masks from sd.c init_system_file_sd: SYNCHRONIZE|STD|FILE_* */
    uint32_t full_mask = 0x0012019f;  /* SYNC|STD_R/W|WRITE_ATTR|READ_ATTR|W_EA|R_EA|APPEND|WRITE|READ */
    uint32_t read_mask = 0x00120089;  /* SYNC|STD_READ|READ_ATTR|R_EA|READ */
    /* ACE1 @0x1c: ACCESS_ALLOWED, SID = S-1-5-18 (LOCAL_SYSTEM) */
    uint8_t *ace = out+0x1c;
    ace[0]=0; ace[1]=0; w16(ace+2, 0x14);
    w32(ace+4, read_only ? read_mask : full_mask);
    ace[8]=1; ace[9]=1;               /* SID rev=1, sub_auth_count=1 */
    ace[14]=5;                        /* identifier authority = 5 */
    w32(ace+16, 18);                  /* SECURITY_LOCAL_SYSTEM_RID */
    /* ACE2 @0x30: ACCESS_ALLOWED, SID = S-1-5-32-544 (BUILTIN\Administrators) */
    ace = out+0x30;
    ace[0]=0; ace[1]=0; w16(ace+2, 0x18);
    w32(ace+4, read_only ? read_mask : full_mask);
    ace[8]=1; ace[9]=2;               /* SID rev=1, sub_auth_count=2 */
    ace[14]=5;
    w32(ace+16, 32);                  /* SECURITY_BUILTIN_DOMAIN_RID */
    w32(ace+20, 544);                 /* DOMAIN_ALIAS_RID_ADMINS */
    /* Owner SID @0x48: S-1-5-18 */
    uint8_t *sid = out+0x48;
    sid[0]=1; sid[1]=1; sid[6]=5; w32(sid+8, 18);
    /* Group SID @0x54: S-1-5-32-544 */
    sid = out+0x54;
    sid[0]=1; sid[1]=2; sid[6]=5; w32(sid+8, 32); w32(sid+12, 544);
    return 0x64;
}

/* Build a $FILE_NAME body (also the index key). Returns body length. */
static uint32_t build_file_name(uint8_t *out, uint64_t parent_ref,
                                uint64_t c,uint64_t a,uint64_t m,
                                uint64_t alloc_size, uint64_t real_size,
                                uint32_t disk_attrs, const uint16_t *name, int name_chars,
                                uint8_t name_space){
    memset(out,0,0x42);
    w64(out+0x00, parent_ref);
    w64(out+0x08, c); w64(out+0x10, m); w64(out+0x18, m); w64(out+0x20, a);
    w64(out+0x28, alloc_size); w64(out+0x30, real_size);
    w32(out+0x38, disk_attrs);
    out[0x40] = (uint8_t)name_chars;
    out[0x41] = name_space;
    memcpy(out+0x42, name, 2*name_chars);
    return 0x42 + 2*name_chars;
}

/* The NTFS namespace keys solely on whether the WIM supplies a DOS alias:
 * none -> POSIX, else WIN32+DOS or a collapsed WIN32_AND_DOS. See add_common
 * and names_collapsible. */

/* ------------------------------------------------------------- index entries */
/* Collation COLLATION_FILE_NAME: compare two $FILE_NAME keys case-insensitively
 * via uppercase. We use a self-consistent upcase (see build_upcase). */
static const uint16_t *g_upcase;   /* 65536 entries, set during open */
static int collate_filename(const uint8_t *a, const uint8_t *b){
    int la=a[0x40], lb=b[0x40];
    const uint8_t *na=a+0x42, *nb=b+0x42;
    int n = la<lb?la:lb;
    for (int i=0;i<n;i++){
        uint16_t ca=g_upcase[na[2*i]|(na[2*i+1]<<8)];
        uint16_t cb=g_upcase[nb[2*i]|(nb[2*i+1]<<8)];
        if (ca!=cb) return ca<cb?-1:1;
    }
    return la==lb?0:(la<lb?-1:1);
}
static int idx_cmp(const void *x, const void *y){
    const idx_entry_t *a=x,*b=y;
    return collate_filename(a->key,b->key);
}

/* Can a long name and its DOS short name share ONE ns=3 (WIN32_AND_DOS) entry?
 * Collapsible-name test (same $UpCase folding): same length AND every
 * char equal under $UpCase. When true NTFS stores a single ns=3 name;
 * otherwise it stores ns=1 (long) + ns=2 (short). */
static int names_collapsible(const uint16_t *l, int ll, const uint16_t *s, int sl){
    if (ll != sl) return 0;
    for (int i=0;i<ll;i++) if (g_upcase[l[i]] != g_upcase[s[i]]) return 0;
    return 1;
}

/* --------------------------------------------------------------- $UpCase gen */
/* Self-consistent uppercase table: identity + ASCII a-z and Latin-1 lowercase.
 * (A fuller Windows-identical table is a later fidelity upgrade; collation only
 * needs writer/$UpCase agreement, which this guarantees.) */
/* The canonical NTFS $UpCase table (md5 7ff498a4..., Windows 8+). chkdsk rejects a
 * non-standard on-disk table ("bad on-disk uppercase table"), so the canonical
 * Windows $UpCase table is embedded verbatim (a fixed Windows constant). g_upcase
 * also drives the writer's own index collation, so writer and $UpCase stay in sync. */
#include "upcase_table.h"
/* ECMA-182 reflected CRC-64: poly 0x9a6c9329ac4bc9b5, init &
 * xorout all-ones. Used for $UpCase:$Info.crc so chkdsk doesn't flag the table
 * as obsolete. */
static uint64_t crc64_ecma(const uint8_t *data, size_t size){
    static const uint64_t poly=0x9a6c9329ac4bc9b5ULL;
    uint64_t table[256];
    for (int i=0;i<256;i++){ uint64_t c=(uint64_t)i; for (int j=0;j<8;j++) c=(c&1)?(poly^(c>>1)):(c>>1); table[i]=c; }
    uint64_t crc=~0ULL;
    while (size--){ crc=table[(crc ^ *data++) & 0xff] ^ (crc>>8); }
    return crc ^ ~0ULL;
}
static uint16_t *build_upcase(void){
    uint16_t *t = malloc(65536*sizeof(uint16_t));
    for (int c=0;c<65536;c++) t[c]=NTFS_UPCASE[c];
    return t;
}

/* --------------------------------------------------------------- $AttrDef gen */
/* Standard NTFS 3.1 $AttrDef:
 * 15 real entries + 1 zero terminator = 16 x 160 = 2560 bytes. The max_size for
 * unbounded attributes ($DATA, $ATTRIBUTE_LIST, $SECURITY_DESCRIPTOR, $INDEX_ROOT,
 * $INDEX_ALLOCATION, $BITMAP) MUST be 0xFFFFFFFFFFFFFFFF, NOT 0 -- the kernel
 * validates every attribute's size against $AttrDef at mount; a 0 max for $DATA
 * makes it reject any non-empty file -> bugcheck NTFS_FILE_SYSTEM (0x24). */
#define ADMAX (~(uint64_t)0)
static const struct { const char *name; uint32_t type, disp, coll, flags; uint64_t mn, mx; } ATTRDEF[] = {
    {"$STANDARD_INFORMATION",0x10,0,0,0x40, 48,72},
    {"$ATTRIBUTE_LIST",      0x20,0,0,0x80, 0, ADMAX},
    {"$FILE_NAME",           0x30,0,0,0x42, 68,578},
    {"$OBJECT_ID",           0x40,0,0,0x40, 0,256},
    {"$SECURITY_DESCRIPTOR", 0x50,0,0,0x80, 0,ADMAX},
    {"$VOLUME_NAME",         0x60,0,0,0x40, 2,256},
    {"$VOLUME_INFORMATION",  0x70,0,0,0x40, 12,12},
    {"$DATA",                0x80,0,0,0x00, 0,ADMAX},
    {"$INDEX_ROOT",          0x90,0,0,0x40, 0,ADMAX},
    {"$INDEX_ALLOCATION",    0xA0,0,0,0x80, 0,ADMAX},
    {"$BITMAP",              0xB0,0,0,0x80, 0,ADMAX},
    {"$REPARSE_POINT",       0xC0,0,0,0x80, 0,16384},
    {"$EA_INFORMATION",      0xD0,0,0,0x40, 8,8},
    {"$EA",                  0xE0,0,0,0x00, 0,65536},
    {"$LOGGED_UTILITY_STREAM",0x100,0,0,0x80, 0,65536},
};
static uint8_t *build_attrdef(uint32_t *len_out){
    int n = sizeof(ATTRDEF)/sizeof(ATTRDEF[0]);
    uint32_t len = (uint32_t)((n+1)*160);   /* +1 = trailing all-zero terminator entry */
    uint8_t *b = calloc(1, len);
    for (int i=0;i<n;i++){
        uint8_t *e = b + i*160;
        for (int j=0; ATTRDEF[i].name[j] && j<64; j++) w16(e+2*j, (uint16_t)ATTRDEF[i].name[j]);
        w32(e+0x80, ATTRDEF[i].type);
        w32(e+0x84, ATTRDEF[i].disp);
        w32(e+0x88, ATTRDEF[i].coll);
        w32(e+0x8C, ATTRDEF[i].flags);
        w64(e+0x90, ATTRDEF[i].mn);
        w64(e+0x98, ATTRDEF[i].mx);
    }
    *len_out = len;
    return b;
}

/* --------------------------------------------------------- default security */
static uint8_t *build_default_sd(uint32_t *len_out){
    uint8_t *sd = calloc(1,256);
    /* header */
    sd[0]=1; sd[1]=0; w16(sd+2, 0x8004);          /* rev, sbz, control: self-rel|dacl-present */
    uint32_t dacl_off=20;
    /* DACL */
    uint8_t *acl = sd+dacl_off;
    acl[0]=2; acl[1]=0;                            /* acl revision */
    w16(acl+4, 3);                                 /* ace count */
    uint32_t p=8;
    /* ACE1 SYSTEM full (S-1-5-18) */
    acl[p+0]=0; acl[p+1]=0; w16(acl+p+2, 8+12); w32(acl+p+4, 0x001F01FF);
    acl[p+8]=1; acl[p+9]=1; acl[p+10]=0;acl[p+11]=0;acl[p+12]=0;acl[p+13]=0;acl[p+14]=0;acl[p+15]=5;
    w32(acl+p+16, 18); p+=20;
    /* ACE2 Administrators full (S-1-5-32-544) */
    acl[p+0]=0; acl[p+1]=0; w16(acl+p+2, 8+16); w32(acl+p+4, 0x001F01FF);
    acl[p+8]=1; acl[p+9]=2; acl[p+15]=5; w32(acl+p+16,32); w32(acl+p+20,544); p+=24;
    /* ACE3 Users r+x (S-1-5-32-545) */
    acl[p+0]=0; acl[p+1]=0; w16(acl+p+2, 8+16); w32(acl+p+4, 0x001200A9);
    acl[p+8]=1; acl[p+9]=2; acl[p+15]=5; w32(acl+p+16,32); w32(acl+p+20,545); p+=24;
    uint32_t acl_size=p; w16(acl+2, (uint16_t)acl_size);
    uint32_t owner_off = dacl_off + acl_size;
    /* Owner SID = Administrators */
    uint8_t *o = sd+owner_off; o[0]=1;o[1]=2;o[7]=5; w32(o+8,32); w32(o+12,544);
    uint32_t group_off = owner_off + 16;
    uint8_t *g = sd+group_off; g[0]=1;g[1]=1;g[7]=5; w32(g+8,18);
    uint32_t total = group_off + 12;
    w32(sd+4, owner_off); w32(sd+8, group_off); w32(sd+12, 0); w32(sd+16, dacl_off);
    *len_out = total;
    return sd;
}

int32_t ntfs_secure_intern(ntfs_writer_t *w, const void *sd, uint32_t sd_len){
    uint8_t *use; uint32_t use_len;
    if (!sd || !sd_len){ use = build_default_sd(&use_len); }
    else { use = malloc(sd_len); memcpy(use, sd, sd_len); use_len = sd_len; }
    uint32_t h = sec_hash(use, use_len);
    for (uint32_t i=0;i<w->nsec;i++)
        if (w->sec[i].hash==h && w->sec[i].sd_len==use_len && !memcmp(w->sec[i].sd,use,use_len)){
            free(use); return w->sec[i].id;
        }
    if (w->nsec==w->cap_sec){ w->cap_sec = w->cap_sec?w->cap_sec*2:8; w->sec=realloc(w->sec,w->cap_sec*sizeof(*w->sec)); }
    sec_desc_t *s = &w->sec[w->nsec++];
    s->sd=use; s->sd_len=use_len; s->hash=h; s->id = 0x100 + (int32_t)(w->nsec-1);
    return s->id;
}

/* Apply the WIM root directory's own security_id to the root record; default
 * is the shared SD. */
void ntfs_set_root_security(ntfs_writer_t *w, int32_t sec_id){ if (sec_id>=0) w->root_sec_id = sec_id; }

/* --------------------------------------------------------------- public API */
uint64_t ntfs_root_ref(const ntfs_writer_t *w){
    uint16_t seq = w->rec[NTFS_REC_ROOT] ? w->rec[NTFS_REC_ROOT]->seq : 1;
    return (uint64_t)NTFS_REC_ROOT | ((uint64_t)seq << 48);
}

ntfs_writer_t *ntfs_writer_open(blockio_t *io, uint64_t part_lba, uint64_t part_sectors,
                                const char *label){
    ntfs_writer_t *w = calloc(1, sizeof *w);
    w->io = io;
    w->sector_size = NTFS_SECTOR;
    w->cluster_size = 4096;
    w->spc = w->cluster_size / w->sector_size;
    w->part_off = part_lba * w->sector_size;
    w->part_sectors = part_sectors;
    w->total_clusters = (part_sectors - 1) / w->spc;
    w->serial = 0x5346534D00000000ull ^ (part_sectors * 2654435761u);
    w->root_sec_id = 0x100; w->root_attrs = 0x10000006;   /* defaults; overridden from the WIM root */
    g_upcase = build_upcase();

    if (label && *label){
        w->label_chars = (int)strlen(label);
        if (w->label_chars > 32) w->label_chars = 32;
        w->label = strdup(label);
    }

    /* ---- cluster layout (NTFS-faithful) ----------------------------------
     * Low region [0, mft_zone_end): $Boot@0, MFT-bitmap@2, $MFT@~4 (the last two
     * placed at close once their sizes are known), then a FREE MFT zone. The data
     * bump starts at mft_zone_end so the zone stays empty for MFT growth. $MFTMirr
     * sits at the volume midpoint, reserved here so the bump steps over it. */
    uint64_t boot_clusters = (8192 + w->cluster_size - 1)/w->cluster_size;
    w->clbmp_bytes = align_up((w->total_clusters + 7)/8, w->cluster_size);
    w->clbmp = calloc(1, w->clbmp_bytes);
    bmp_mark(w, 0, boot_clusters);                       /* $Boot @ LCN 0 */
    uint64_t mirror_clusters = (4*NTFS_MFT_RECSZ + w->cluster_size-1)/w->cluster_size;
    w->mftmirr_lcn = (part_sectors/2) / w->spc;          /* volume midpoint */
    w->reserve_lcn = w->mftmirr_lcn; w->reserve_len = mirror_clusters;
    bmp_mark(w, w->mftmirr_lcn, mirror_clusters);
    /* MFT zone = (planned base) + 12.5% of the volume (the NTFS default). The base
     * 64 is a safe upper bound on the real mft_lcn (assigned at close); the small
     * slack between the real MFT end and mft_zone_end simply stays free in-zone. */
    w->mft_zone_end = 64 + (w->total_clusters >> 3);
    w->next_lcn = w->mft_zone_end;

    /* default shared security descriptor -> id 0x100 */
    ntfs_secure_intern(w, NULL, 0);

    /* Create the 27 reserved/system records; attributes filled at close.
     *  0-11 named metafiles, 12-23 reserved, 24/25/26 = $Quota/$ObjId/$Reparse
     *  ($Extend children, VIEW_INDEX records). User files MUST start at 27 so they
     *  never squat the $Extend metafile inode range (which the kernel opens by name
     *  at mount -> NTFS_FILE_SYSTEM 0x24 if those inodes hold the wrong objects). */
    for (uint32_t i=0;i<27;i++){
        int is_dir = (i==NTFS_REC_ROOT || i==NTFS_REC_EXTEND);
        rec_new(w, i, is_dir);
    }
    w->nrec = 27;
    return w;
}

/* Grow a directory record's index accumulator. */
static void dir_push(mftrec_t *d, uint64_t file_ref, const uint8_t *key, uint16_t key_len){
    if (d->nentries==d->cap_entries){ d->cap_entries=d->cap_entries?d->cap_entries*2:8; d->entries=realloc(d->entries,d->cap_entries*sizeof(idx_entry_t)); }
    idx_entry_t *e=&d->entries[d->nentries++];
    e->key=malloc(key_len); memcpy(e->key,key,key_len); e->key_len=key_len; e->file_ref=file_ref;
}

static uint32_t parent_recno(uint64_t ref){ return (uint32_t)(ref & 0xFFFFFFFFFFFFull); }

static uint64_t add_common(ntfs_writer_t *w, uint64_t parent_ref, const uint16_t *name, int name_chars,
                           const uint16_t *short_name, int short_name_chars,
                           uint32_t attributes, int32_t security_id,
                           uint64_t c,uint64_t a,uint64_t m, int is_dir,
                           uint64_t alloc_size, uint64_t real_size,
                           mftrec_t **out_rec){
    uint32_t recno = w->nrec;
    mftrec_t *r = rec_new(w, recno, is_dir);
    uint32_t disk_attrs = to_disk_attrs(attributes, is_dir);

    uint8_t si[72]; build_std_info(si, c,a,m, disk_attrs, security_id);
    if (attr_resident(r, NTFS_AT_STANDARD_INFORMATION, NULL,0, si,72, 0)) return 0;

    uint64_t my_ref = (uint64_t)recno | ((uint64_t)r->seq << 48);
    mftrec_t *parent = w->rec[parent_recno(parent_ref)];

    /* $FILE_NAME namespace, per the NTFS format (by DOS-alias presence,
     * NOT is_8dot3): a name with no WIM DOS alias is POSIX
     * (ns=0). When the WIM supplies a DOS short name, the long name is WIN32 (1)
     * and a second DOS (2) $FILE_NAME is emitted -- unless the two collapse onto
     * one (equal under $UpCase), giving a single WIN32_AND_DOS (3) entry. Each
     * emitted name is pushed into the parent $I30 index (both -> this record),
     * and link_count counts EVERY emitted name including the DOS one. */
    int has_short  = short_name && short_name_chars > 0;
    int collapsed  = has_short && names_collapsible(name, name_chars, short_name, short_name_chars);
    uint8_t long_ns = !has_short ? 0 /*POSIX*/ : (collapsed ? 3 /*WIN32_AND_DOS*/ : 1 /*WIN32*/);
    uint8_t fn[0x42 + 512];
    uint32_t fn_len = build_file_name(fn, parent_ref, c,a,m, alloc_size, real_size, disk_attrs, name, name_chars, long_ns);
    if (attr_resident(r, NTFS_AT_FILE_NAME, NULL,0, fn, fn_len, 0)) return 0;
    dir_push(parent, my_ref, fn, (uint16_t)fn_len);

    if (has_short && !collapsed){
        uint8_t sn[0x42 + 512];
        uint32_t sn_len = build_file_name(sn, parent_ref, c,a,m, alloc_size, real_size, disk_attrs, short_name, short_name_chars, 2);
        if (attr_resident(r, NTFS_AT_FILE_NAME, NULL,0, sn, sn_len, 0)) return 0;
        dir_push(parent, my_ref, sn, (uint16_t)sn_len);
        r->link_count++;                           /* the DOS name is a second hard link */
    }

    *out_rec = r;
    return my_ref;
}

/* Find a direct child named `name` (UTF-16LE, case-insensitive via $UpCase) in
 * directory `parent_ref`; return its MFT ref or 0 if absent. Used by the
 * post-apply injector to resolve-or-create nested paths (\Windows\Panther, ...). */
uint64_t ntfs_lookup_child(ntfs_writer_t *w, uint64_t parent_ref, const uint16_t *name, int name_chars){
    mftrec_t *p = w->rec[parent_recno(parent_ref)];
    if (!p) return 0;
    for (uint32_t i=0;i<p->nentries;i++){
        idx_entry_t *e=&p->entries[i];
        if (e->key[0x40] != name_chars) continue;
        const uint8_t *kn = e->key+0x42; int match=1;
        for (int j=0;j<name_chars;j++){
            uint16_t c = kn[2*j] | (kn[2*j+1]<<8);
            if (g_upcase[c] != g_upcase[name[j]]) { match=0; break; }
        }
        if (match) return e->file_ref;
    }
    return 0;
}

uint64_t ntfs_add_dir(ntfs_writer_t *w, uint64_t parent_ref, const uint16_t *name, int name_chars,
                      const uint16_t *short_name, int short_name_chars,
                      uint32_t attributes, int32_t security_id,
                      uint64_t c,uint64_t a,uint64_t m){
    mftrec_t *r;
    uint64_t ref = add_common(w, parent_ref, name, name_chars, short_name, short_name_chars,
                              attributes|0x10, security_id, c,a,m, 1, 0,0, &r);
    /* $INDEX_ROOT is appended at close once entries are known */
    return ref;
}

/* Attach a reparse point to the record `ref` (a dir or file already added). `tag`
 * is the IO_REPARSE_TAG_*; `data`/`data_len` is the tag-specific buffer EXACTLY as
 * the WIM stores it (the bytes after the 8-byte REPARSE_DATA_BUFFER header). We
 * prepend tag|len|reserved to form the on-disk $REPARSE_POINT value, stored on the
 * record and emitted (after the index attrs) at close. The caller must also leave
 * FILE_ATTRIBUTE_REPARSE_POINT set in the record's attributes. */
void ntfs_set_reparse(ntfs_writer_t *w, uint64_t ref, uint32_t tag,
                      const void *data, uint32_t data_len){
    mftrec_t *r = w->rec[parent_recno(ref)];
    if (!r) return;
    uint32_t total = 8 + data_len;
    uint8_t *buf = malloc(total);
    w32(buf+0, tag);
    w16(buf+4, (uint16_t)data_len);
    w16(buf+6, 0);                     /* reserved */
    if (data_len) memcpy(buf+8, data, data_len);
    free(r->reparse);
    r->reparse = buf; r->reparse_len = total;
    /* record (tag, ref) for the $Extend\$Reparse:$R enumeration index (built at close) */
    if (w->n_rpl == w->cap_rpl){ w->cap_rpl = w->cap_rpl?w->cap_rpl*2:8; w->rpl = realloc(w->rpl, w->cap_rpl*sizeof(*w->rpl)); }
    w->rpl[w->n_rpl].tag = tag; w->rpl[w->n_rpl].ref = ref; w->n_rpl++;
}

uint64_t ntfs_add_file(ntfs_writer_t *w, uint64_t parent_ref, const uint16_t *name, int name_chars,
                       const uint16_t *short_name, int short_name_chars,
                       uint32_t attributes, int32_t security_id,
                       uint64_t c,uint64_t a,uint64_t m, const void *data, uint64_t size){
    uint64_t nclu = size ? (size + w->cluster_size - 1)/w->cluster_size : 0;
    uint64_t alloc_size = nclu * w->cluster_size;
    mftrec_t *r;
    uint64_t ref = add_common(w, parent_ref, name, name_chars, short_name, short_name_chars,
                              attributes, security_id, c,a,m, 0,
                              alloc_size, size, &r);
    if (!ref) return 0;

    /* $DATA: resident if it fits, else a single non-resident run streamed now. */
    uint32_t resident_room = NTFS_MFT_RECSZ - r->used - 0x18 - 8;
    if (size <= resident_room && size <= 700){
        if (attr_resident(r, NTFS_AT_DATA, NULL,0, data, (uint32_t)size, 0)) return 0;
    } else {
        uint64_t lcn = alloc_clusters(w, nclu);
        static int g_skip_data = -1;
        if (g_skip_data < 0) g_skip_data = getenv("NTFS_SKIP_DATA") ? 1 : 0;  /* debug: fast close-path repro */
        if (!g_skip_data){
        if (size) blockio_write(w->io, cluster_byte(w, lcn), data, size);
        /* zero-fill the slack of the last cluster */
        if (alloc_size > size){
            static uint8_t z[4096]={0};
            uint64_t pad = alloc_size - size, base = cluster_byte(w,lcn)+size;
            while (pad){ uint64_t k=pad<sizeof z?pad:sizeof z; blockio_write(w->io, base, z, k); base+=k; pad-=k; }
        }
        }
        if (attr_nonres(r, NTFS_AT_DATA, NULL,0, lcn, nclu, w->cluster_size, size, size, 0)) return 0;
    }
    return ref;
}

uint64_t ntfs_add_file_deferred(ntfs_writer_t *w, uint64_t parent_ref,
                       const uint16_t *name, int name_chars,
                       const uint16_t *short_name, int short_name_chars,
                       uint32_t attributes, int32_t security_id,
                       uint64_t c,uint64_t a,uint64_t m, uint64_t size, uint64_t *out_byte_off){
    /* caller guarantees size>700 -> always non-resident; mirrors the else-branch
     * of ntfs_add_file but emits NO data (a worker writes it via blockio_pwrite). */
    *out_byte_off = 0;
    uint64_t nclu = (size + w->cluster_size - 1)/w->cluster_size;
    uint64_t alloc_size = nclu * w->cluster_size;
    mftrec_t *r;
    uint64_t ref = add_common(w, parent_ref, name, name_chars, short_name, short_name_chars,
                              attributes, security_id, c,a,m, 0,
                              alloc_size, size, &r);
    if (!ref) return 0;
    uint64_t lcn = alloc_clusters(w, nclu);
    *out_byte_off = cluster_byte(w, lcn);   /* worker writes `size` bytes here */
    if (attr_nonres(r, NTFS_AT_DATA, NULL,0, lcn, nclu, w->cluster_size, size, size, 0)){
        *out_byte_off = 0; return 0;
    }
    return ref;
}

/* Add a hard link: a new directory entry (`name` under `parent_ref`) for the
 * EXISTING inode `target_ref` (returned by a prior ntfs_add_file). No new record
 * and no $DATA -- a $FILE_NAME (+ optional DOS partner) is inserted into the
 * target record (sorted before $DATA) and indexed in the parent, and the inode's
 * hard-link count is bumped. This is the NTFS hard-link model, used instead
 * of duplicating content. `size` is the inode's $DATA size (for the $FILE_NAME).
 * Returns target_ref, or 0 if the target record can't fit another name (the
 * caller then flattens this link into its own record). */
uint64_t ntfs_add_hardlink(ntfs_writer_t *w, uint64_t target_ref, uint64_t parent_ref,
                           const uint16_t *name, int name_chars,
                           const uint16_t *short_name, int short_name_chars,
                           uint32_t attributes, uint64_t c,uint64_t a,uint64_t m,
                           uint64_t size){
    uint32_t recno = parent_recno(target_ref);
    if (recno >= w->cap_rec || !w->rec[recno]) return 0;
    mftrec_t *r = w->rec[recno];
    if (r->is_dir) return 0;                       /* directories are never hard-linked */
    mftrec_t *parent = w->rec[parent_recno(parent_ref)];
    if (!parent) return 0;

    uint32_t disk_attrs = to_disk_attrs(attributes, 0);
    uint64_t nclu = size ? (size + w->cluster_size - 1)/w->cluster_size : 0;
    uint64_t alloc_size = nclu * w->cluster_size;
    /* Same namespace rule as add_common (per the NTFS format): POSIX when no WIM
     * DOS alias, else WIN32(1)+DOS(2), or a single WIN32_AND_DOS(3) if collapsible. */
    int has_short = short_name && short_name_chars > 0;
    int collapsed = has_short && names_collapsible(name, name_chars, short_name, short_name_chars);
    uint8_t long_ns = !has_short ? 0 : (collapsed ? 3 : 1);

    uint8_t fn[0x42 + 512];
    uint32_t fn_len = build_file_name(fn, parent_ref, c,a,m, alloc_size, size, disk_attrs,
                                      name, name_chars, long_ns);
    int has_dos = has_short && !collapsed;
    uint8_t sn[0x42 + 512]; uint32_t sn_len = 0;
    if (has_dos)
        sn_len = build_file_name(sn, parent_ref, c,a,m, alloc_size, size, disk_attrs,
                                 short_name, short_name_chars, 2);

    /* require room for BOTH names up front, so we never half-add a link */
    uint32_t need = align8u(0x18 + fn_len) + (has_dos ? align8u(0x18 + sn_len) : 0);
    /* Once this inode has started overflowing (or this pair won't fit), the new
     * name(s) go to the overflow list -> spilled into $ATTRIBUTE_LIST + extension
     * records at close. This is the NTFS approach for big hard-link groups
     * (WinSxS component store), instead of flattening into duplicate inodes which
     * breaks CBS servicing. */
    if (r->n_ov || r->used + need + 8 > NTFS_MFT_RECSZ){
        store_ov(r, fn, fn_len);
        dir_push(parent, target_ref, fn, (uint16_t)fn_len);
        if (has_dos){ store_ov(r, sn, sn_len); dir_push(parent, target_ref, sn, (uint16_t)sn_len); }
        r->link_count++;
        if (has_dos) r->link_count++;              /* DOS name is a separate hard link */
        return target_ref;
    }

    if (attr_insert_filename(r, fn, fn_len)) return 0;
    dir_push(parent, target_ref, fn, (uint16_t)fn_len);
    if (has_dos){
        if (attr_insert_filename(r, sn, sn_len)) return 0;
        dir_push(parent, target_ref, sn, (uint16_t)sn_len);
    }
    r->link_count++;                               /* the long name (one more hard link) */
    if (has_dos) r->link_count++;                  /* plus the DOS name */
    return target_ref;
}

/* ------------------------------------------------------- directory $I30 index */
/* A node entry while building. Filename index ($I30): `file_ref` + `key`. VIEW
 * index ($SDH/$SII): `key` + inline `data`/`data_len` (data!=NULL selects this).
 * `sub_vcn` is the down-pointer for internal nodes. The B-tree machinery
 * (pack_level/emit_indx/serialize_body) is shared between both index kinds. */
typedef struct { uint64_t file_ref; const uint8_t *key; uint16_t key_len; uint64_t sub_vcn;
                 const uint8_t *data; uint16_t data_len; } bentry_t;
/* A built node awaiting emission once the index's base LCN is known. */
typedef struct { uint64_t vcn; bentry_t *ent; int n; int internal; uint64_t end_vcn; } nodespec_t;

/* Entry size. Filename: [hdr0x10][key][subvcn]. VIEW: [hdr0x10][key][data][subvcn]
 * with data IMMEDIATELY after the key at 0x10+key_len (NOT 8-aligned — Windows/
 * $SII data sits at 0x14, not 0x18; aligning it makes ntfs.sys read the
 * pad zeros as the $SDS pointer -> every security_id resolves to descriptor #0). */
static uint32_t entry_size_dv(uint16_t key_len, uint16_t data_len, int internal){
    if (data_len){ uint32_t doff=0x10+key_len; return align8u(doff + data_len + (internal?8:0)); }
    return align8u(0x10 + key_len + (internal?8:0));
}
static uint32_t entry_size(uint16_t key_len, int internal){ return entry_size_dv(key_len, 0, internal); }
static uint32_t end_size(int internal){ return align8u(0x10 + (internal?8:0)); }

/* Serialize [entries + END] into a node body. Returns bytes. */
static uint32_t serialize_body(uint8_t *out, const bentry_t *ent, int n, int internal, uint64_t end_vcn){
    uint32_t p=0;
    for (int i=0;i<n;i++){
        uint32_t elen = entry_size_dv(ent[i].key_len, ent[i].data_len, internal);
        uint8_t *e = out+p; memset(e,0,elen);
        if (ent[i].data){                       /* VIEW index entry ($SDH/$SII) */
            uint16_t doff=(uint16_t)(0x10+ent[i].key_len);   /* data right after key (0x14 for SII) */
            w16(e+0x00, doff); w16(e+0x02, ent[i].data_len);
            w16(e+0x08, (uint16_t)elen); w16(e+0x0A, ent[i].key_len);
            w16(e+0x0C, internal?0x01:0x00);
            memcpy(e+0x10, ent[i].key, ent[i].key_len);
            memcpy(e+doff, ent[i].data, ent[i].data_len);
        } else {                                /* filename index entry ($I30) */
            w64(e+0x00, ent[i].file_ref);
            w16(e+0x08, (uint16_t)elen);
            w16(e+0x0A, ent[i].key_len);
            w16(e+0x0C, internal?0x01:0x00);
            memcpy(e+0x10, ent[i].key, ent[i].key_len);
        }
        if (internal) w64(e+elen-8, ent[i].sub_vcn);
        p+=elen;
    }
    uint32_t elen = end_size(internal);
    uint8_t *e = out+p; memset(e,0,elen);
    w16(e+0x08, (uint16_t)elen);
    w16(e+0x0C, internal?0x03:0x02);
    if (internal) w64(e+elen-8, end_vcn);
    p+=elen;
    return p;
}

/* Emit one INDX block (4096B, fixup-applied) at lcn_base + vcn*4096. */
static void emit_indx(ntfs_writer_t *w, uint64_t lcn_base, const nodespec_t *ns){
    uint8_t blk[NTFS_INDX_SIZE]; memset(blk,0,sizeof blk);
    memcpy(blk,"INDX",4);
    w16(blk+4, 0x28); w16(blk+6, 9);              /* usa_offset, usa_count (8 sectors) */
    w64(blk+0x10, ns->vcn);
    uint8_t *ih = blk+0x18;
    uint32_t entries_off = 0x28;                   /* entries at blk+0x40, clear of USA */
    uint32_t blen = serialize_body(ih+entries_off, ns->ent, ns->n, ns->internal, ns->end_vcn);
    w32(ih+0x00, entries_off);
    w32(ih+0x04, entries_off + blen);
    w32(ih+0x08, NTFS_INDX_SIZE - 0x18);
    ih[0x0C] = ns->internal ? 0x01 : 0x00;
    apply_fixup(blk, NTFS_INDX_SIZE, 512, (uint16_t)(ns->vcn+1));
    blockio_write(w->io, cluster_byte(w,lcn_base) + ns->vcn*NTFS_INDX_SIZE, blk, NTFS_INDX_SIZE);
}

#define INDX_BODY_CAP (NTFS_INDX_SIZE - 0x40)      /* bytes available for [entries+END] */

/* Pack a level of entries into INDX nodes, promoting a separator entry between
 * adjacent nodes up to the parent. `internal`: are these internal nodes?
 * `rightmost_child`: VCN the last node's END points to (ignored if leaf).
 * Appends nodes to *specs; returns the promoted separators in *out / *nout and
 * the rightmost node's VCN in *rightmost_out. *next_vcn is the VCN allocator. */
static void pack_level(const bentry_t *in, int nin, int internal, uint64_t rightmost_child,
                       nodespec_t **specs, int *nspec, int *cap_spec,
                       bentry_t *out, int *nout, uint64_t *rightmost_out, uint64_t *next_vcn){
    int i=0; *nout=0;
    for (;;){
        bentry_t *cur = malloc((nin+1)*sizeof(bentry_t)); int n=0; uint32_t used=0;
        while (i<nin){
            uint32_t es = entry_size_dv(in[i].key_len, in[i].data_len, internal);
            if (n>0 && used + es + end_size(internal) > INDX_BODY_CAP) break;
            cur[n++] = in[i]; used += es; i++;
        }
        uint64_t vcn = (*next_vcn)++;
        if (*nspec==*cap_spec){ *cap_spec = *cap_spec?*cap_spec*2:16; *specs=realloc(*specs,*cap_spec*sizeof(nodespec_t)); }
        nodespec_t *ns = &(*specs)[(*nspec)++];
        ns->vcn=vcn; ns->ent=cur; ns->n=n; ns->internal=internal;
        if (i<nin && i==nin-1 && n>=2){
            /* in[i] is the LAST entry and didn't fit: promoting it the normal way
             * would leave an empty rightmost node (chkdsk: "Correcting error in
             * index"). Instead pop THIS node's last entry as the separator and
             * give in[i] its own non-empty final node. */
            bentry_t sep = cur[--n];                       /* pop -> becomes the separator */
            ns->n = n;
            ns->end_vcn = internal ? sep.sub_vcn : 0;      /* sep's left child becomes this node's END */
            sep.sub_vcn = vcn;
            out[(*nout)++] = sep;
            bentry_t *last = malloc(sizeof(bentry_t)); last[0] = in[i];
            uint64_t vcn2 = (*next_vcn)++;
            if (*nspec==*cap_spec){ *cap_spec=*cap_spec?*cap_spec*2:16; *specs=realloc(*specs,*cap_spec*sizeof(nodespec_t)); }
            nodespec_t *ns2 = &(*specs)[(*nspec)++];
            ns2->vcn=vcn2; ns2->ent=last; ns2->n=1; ns2->internal=internal;
            ns2->end_vcn = internal ? rightmost_child : 0; /* in[i] keeps its own sub_vcn; END = overall rightmost */
            *rightmost_out = vcn2;
            i = nin;
            break;
        } else if (i<nin){
            /* promote in[i]: its left subtree is this node; this node's END child
             * is in[i]'s original down-pointer (the child to the right of `cur`). */
            ns->end_vcn = internal ? in[i].sub_vcn : 0;
            bentry_t prom = in[i]; prom.sub_vcn = vcn;
            out[(*nout)++] = prom;
            i++;
        } else {
            ns->end_vcn = internal ? rightmost_child : 0;
            *rightmost_out = vcn;
            break;
        }
    }
}

/* Build a directory's $I30 index. Small dirs -> resident $INDEX_ROOT only; large
 * dirs -> $INDEX_ROOT (separators) + $INDEX_ALLOCATION (INDX B-tree) + $BITMAP. */
static int build_directory_index(ntfs_writer_t *w, mftrec_t *d){
    static const uint16_t I30[4]={'$','I','3','0'};
    if (d->nentries) qsort(d->entries, d->nentries, sizeof(idx_entry_t), idx_cmp);

    /* root body budget = free record space minus the attribute/index headers.
     * For the B-tree case we must also reserve room for the $INDEX_ALLOCATION and
     * $BITMAP attributes that get appended after $INDEX_ROOT, or the record
     * overflows for huge directories (long WinSxS names + thousands of entries). */
    uint32_t fixed = 0x20 /*attr+name*/ + 0x10 /*idxroot hdr*/ + 0x10 /*idx hdr*/ + 8 /*rec end*/ + 8;
    uint32_t free_now = (NTFS_MFT_RECSZ > d->used + fixed) ? NTFS_MFT_RECSZ - d->used - fixed : 0;
    uint32_t small_budget = free_now;
    uint32_t TREE_RESERVE = 0x60 /*$INDEX_ALLOCATION*/ + 0x60 /*$BITMAP*/;
    uint32_t root_budget = free_now > TREE_RESERVE ? free_now - TREE_RESERVE : 0;

    /* does everything fit resident (leaf-style, no children)? */
    uint32_t small = 0;
    for (uint32_t i=0;i<d->nentries;i++) small += entry_size(d->entries[i].key_len, 0);
    small += end_size(0);

    uint8_t *val; uint32_t val_len; int large; uint64_t idx_lcn=0, idx_blocks=0;

    if (small <= small_budget){
        bentry_t *e = malloc((d->nentries+1)*sizeof(bentry_t));
        for (uint32_t i=0;i<d->nentries;i++){ e[i].file_ref=d->entries[i].file_ref; e[i].key=d->entries[i].key; e[i].key_len=d->entries[i].key_len; e[i].sub_vcn=0; e[i].data=NULL; e[i].data_len=0; }
        uint8_t *body = malloc(small+64);
        uint32_t blen = serialize_body(body, e, d->nentries, 0, 0);
        val_len = 0x20 + blen; val = calloc(1,val_len);
        memcpy(val+0x20, body, blen);
        large = 0;
        free(body); free(e);
    } else {
        /* B-tree: leaves -> internal levels until the top fits in the root. */
        bentry_t *leaf_in = malloc(d->nentries*sizeof(bentry_t));
        for (uint32_t i=0;i<d->nentries;i++){ leaf_in[i].file_ref=d->entries[i].file_ref; leaf_in[i].key=d->entries[i].key; leaf_in[i].key_len=d->entries[i].key_len; leaf_in[i].sub_vcn=0; leaf_in[i].data=NULL; leaf_in[i].data_len=0; }
        nodespec_t *specs=NULL; int nspec=0, cap_spec=0; uint64_t next_vcn=0;
        bentry_t *seps = malloc((d->nentries+1)*sizeof(bentry_t)); int nsep=0; uint64_t rightmost=0;
        pack_level(leaf_in, (int)d->nentries, 0, 0, &specs,&nspec,&cap_spec, seps,&nsep,&rightmost,&next_vcn);
        free(leaf_in);

        /* promote internal levels until the separators fit in the root */
        for (;;){
            uint32_t need=0; for (int i=0;i<nsep;i++) need += entry_size(seps[i].key_len,1); need += end_size(1);
            if (need <= root_budget || nsep==0) break;   /* nsep==0: root = END->child only */
            bentry_t *up = malloc((nsep+1)*sizeof(bentry_t)); int nup=0; uint64_t rout=0;
            pack_level(seps, nsep, 1, rightmost, &specs,&nspec,&cap_spec, up,&nup,&rout,&next_vcn);
            free(seps); seps=up; nsep=nup; rightmost=rout;
        }

        /* allocate the INDX region and emit every node */
        idx_blocks = next_vcn;
        idx_lcn = alloc_clusters(w, idx_blocks);
        for (int i=0;i<nspec;i++){ emit_indx(w, idx_lcn, &specs[i]); free(specs[i].ent); }
        free(specs);

        /* root = internal node holding the top separators + END->rightmost */
        uint8_t *body = malloc(NTFS_MFT_RECSZ);
        uint32_t blen = serialize_body(body, seps, nsep, 1, rightmost);
        val_len = 0x20 + blen; val = calloc(1,val_len);
        memcpy(val+0x20, body, blen);
        large = 1;
        free(body); free(seps);
    }

    /* common $INDEX_ROOT header */
    w32(val+0x00, NTFS_AT_FILE_NAME);
    w32(val+0x04, 0x01);                 /* COLLATION_FILE_NAME */
    w32(val+0x08, NTFS_INDX_SIZE);
    val[0x0C] = 1;
    uint8_t *ih = val+0x10;
    w32(ih+0x00, 0x10);
    w32(ih+0x04, val_len - 0x10);        /* index_length (rel to ih) */
    w32(ih+0x08, val_len - 0x10);        /* allocated_size */
    ih[0x0C] = large ? 0x01 : 0x00;
    if (attr_resident(d, NTFS_AT_INDEX_ROOT, I30, 4, val, val_len, 0)){ free(val); return -1; }
    free(val);

    if (large){
        if (attr_nonres(d, NTFS_AT_INDEX_ALLOCATION, I30, 4, idx_lcn, idx_blocks, w->cluster_size,
                        idx_blocks*NTFS_INDX_SIZE, idx_blocks*NTFS_INDX_SIZE, 0)) return -1;
        uint32_t bm_bytes = (uint32_t)((idx_blocks+7)/8);
        /* Keep the index $BITMAP resident whenever it fits the record (what
         * Windows does); only spill to non-resident if attr_resident can't
         * fit it. This avoids forcing non-resident for the giant WinSxS dirs
         * (>512 INDX blocks), which chkdsk would otherwise "Correct". */
        uint8_t *rbm = calloc(1, align8u(bm_bytes));
        for (uint64_t b=0;b<idx_blocks;b++) rbm[b/8]|=1<<(b&7);
        if (attr_resident(d, NTFS_AT_BITMAP, I30, 4, rbm, align8u(bm_bytes), 0) == 0){
            free(rbm);                                  /* fit resident */
        } else {                                        /* too big -> non-resident $BITMAP */
            free(rbm);
            uint64_t bm_clusters = (bm_bytes + w->cluster_size-1)/w->cluster_size;
            uint64_t bm_lcn = alloc_clusters(w, bm_clusters);
            uint64_t bm_alloc = bm_clusters*w->cluster_size;
            uint8_t *bm = calloc(1, bm_alloc);
            for (uint64_t b=0;b<idx_blocks;b++) bm[b/8]|=1<<(b&7);
            blockio_write(w->io, cluster_byte(w,bm_lcn), bm, bm_alloc);
            free(bm);
            if (attr_nonres(d, NTFS_AT_BITMAP, I30, 4, bm_lcn, bm_clusters, w->cluster_size,
                            bm_bytes, bm_bytes, 0)) return -1;
        }
    }
    return 0;
}

/* A prepared $Secure view index ($SDH or $SII). The INDX B-tree blocks are
 * already written to disk; the caller emits the record attributes ($INDEX_ROOT,
 * then $INDEX_ALLOCATION, then $BITMAP) interleaved across $SDH/$SII so the
 * record stays sorted by (type, name). */
typedef struct {
    uint8_t *root; uint32_t root_len;     /* $INDEX_ROOT value (caller frees) */
    int      large;
    uint64_t idx_lcn, idx_blocks;         /* $INDEX_ALLOCATION run (if large) */
    uint8_t *bm; uint32_t bm_len;         /* resident $BITMAP (if large; caller frees) */
} secidx_t;

/* COLLATION_NTOFS_SECURITY_HASH ($SDH): key = {hash:u32, security_id:u32}, ordered
 * by hash then id (little-endian uint32s). */
static int secidx_sdh_cmp(const void *a, const void *b){
    const bentry_t *x=a,*y=b; const uint8_t *kx=x->key,*ky=y->key;
    uint32_t hx=kx[0]|(kx[1]<<8)|((uint32_t)kx[2]<<16)|((uint32_t)kx[3]<<24);
    uint32_t hy=ky[0]|(ky[1]<<8)|((uint32_t)ky[2]<<16)|((uint32_t)ky[3]<<24);
    if (hx!=hy) return hx<hy?-1:1;
    uint32_t ix=kx[4]|(kx[5]<<8)|((uint32_t)kx[6]<<16)|((uint32_t)kx[7]<<24);
    uint32_t iy=ky[4]|(ky[5]<<8)|((uint32_t)ky[6]<<16)|((uint32_t)ky[7]<<24);
    return ix<iy?-1:(ix>iy?1:0);
}

/* Build the B-tree for one $Secure view index. `ent` (sorted by `collation`)
 * carry key + inline data (the 20-byte $SDS header). Emits INDX blocks to disk
 * and fills `out`. `root_budget` is the entry-byte budget for the resident root;
 * keep it small so two roots + their alloc/bitmap attrs share the 1024B record. */
static int prepare_secure_index(ntfs_writer_t *w, uint32_t collation,
                                bentry_t *ent, uint32_t n, uint32_t root_budget, secidx_t *out){
    memset(out,0,sizeof *out);
    uint32_t small=0;
    for (uint32_t i=0;i<n;i++) small += entry_size_dv(ent[i].key_len, ent[i].data_len, 0);
    small += end_size(0);

    uint8_t *val; uint32_t val_len; int large; uint64_t idx_lcn=0, idx_blocks=0;
    if (small <= root_budget){
        uint8_t *body = malloc(small+64);
        uint32_t blen = serialize_body(body, ent, n, 0, 0);
        val_len = 0x20 + blen; val = calloc(1,val_len);
        memcpy(val+0x20, body, blen); large=0; free(body);
    } else {
        nodespec_t *specs=NULL; int nspec=0, cap_spec=0; uint64_t next_vcn=0;
        bentry_t *seps = malloc((n+1)*sizeof(bentry_t)); int nsep=0; uint64_t rightmost=0;
        pack_level(ent,(int)n,0,0,&specs,&nspec,&cap_spec,seps,&nsep,&rightmost,&next_vcn);
        for(;;){
            uint32_t need=0; for(int i=0;i<nsep;i++) need+=entry_size_dv(seps[i].key_len,seps[i].data_len,1); need+=end_size(1);
            if (need <= root_budget || nsep==0) break;
            bentry_t *up=malloc((nsep+1)*sizeof(bentry_t)); int nup=0; uint64_t rout=0;
            pack_level(seps,nsep,1,rightmost,&specs,&nspec,&cap_spec,up,&nup,&rout,&next_vcn);
            free(seps); seps=up; nsep=nup; rightmost=rout;
        }
        idx_blocks=next_vcn; idx_lcn=alloc_clusters(w,idx_blocks);
        for(int i=0;i<nspec;i++){ emit_indx(w,idx_lcn,&specs[i]); free(specs[i].ent); }
        free(specs);
        uint8_t *body=malloc((size_t)NTFS_INDX_SIZE);
        uint32_t blen=serialize_body(body,seps,nsep,1,rightmost);
        val_len=0x20+blen; val=calloc(1,val_len); memcpy(val+0x20,body,blen);
        large=1; free(body); free(seps);
    }
    /* $INDEX_ROOT header: VIEW index -> indexed attr type = 0 (AT_UNUSED). */
    w32(val+0x00,0); w32(val+0x04,collation); w32(val+0x08,NTFS_INDX_SIZE); val[0x0C]=1;
    uint8_t *ih=val+0x10; w32(ih+0x00,0x10); w32(ih+0x04,val_len-0x10); w32(ih+0x08,val_len-0x10); ih[0x0C]=large?0x01:0x00;
    out->root=val; out->root_len=val_len; out->large=large; out->idx_lcn=idx_lcn; out->idx_blocks=idx_blocks;
    if (large){
        uint32_t bm_bytes=(uint32_t)((idx_blocks+7)/8);
        out->bm_len=align8u(bm_bytes); out->bm=calloc(1,out->bm_len);
        for (uint64_t b=0;b<idx_blocks;b++) out->bm[b/8]|=1<<(b&7);
    }
    return 0;
}

/* Append an EMPTY view-index $INDEX_ROOT (END entry only) to a view-index
 * metafile, building the empty $INDEX_ROOT with
 * indexed_attr_type == AT_UNUSED (so r->type = 0). `body`/`body_len` may carry
 * pre-built index entries (e.g. $Quota's $Q defaults); pass NULL/0 for empty.
 * Layout matches the existing $SDH/$SII builder above. */
static int attr_view_index_root(mftrec_t *r, const uint16_t *name, int name_chars,
                                uint32_t collation, const uint8_t *body, uint32_t body_len){
    uint32_t vl = 0x10 + 0x10 + body_len + 0x10;     /* hdr + idx-hdr + entries + END */
    uint8_t *val = calloc(1, vl);
    /* INDEX_ROOT header: type=0 (AT_UNUSED, not $FILE_NAME),
     * collation_rule, index_block_size=NTFS_INDX_SIZE, clusters_per_index_block=1. */
    w32(val+0x00, 0);                       /* AT_UNUSED */
    w32(val+0x04, collation);
    w32(val+0x08, NTFS_INDX_SIZE);
    val[0x0C] = 1;                          /* NTFS_INDX_SIZE(4096)/cluster_size(4096) */
    uint8_t *ih = val+0x10;
    uint32_t idx_len = 0x10 + body_len + 0x10;       /* INDEX_HEADER + entries + END */
    w32(ih+0x00, 0x10);                     /* entries_offset = sizeof(INDEX_HEADER) */
    w32(ih+0x04, idx_len);                  /* index_length */
    w32(ih+0x08, idx_len);                  /* allocated_size */
    ih[0x0C] = 0;                           /* SMALL_INDEX */
    if (body_len) memcpy(ih+0x10, body, body_len);
    /* terminating END entry: all-zero key, flags=INDEX_ENTRY_END */
    uint8_t *e = ih + 0x10 + body_len;
    w16(e+0x08, 0x10);                      /* length = sizeof(INDEX_ENTRY_HEADER) */
    w16(e+0x0C, 0x02);                      /* INDEX_ENTRY_END */
    int rc = attr_resident(r, NTFS_AT_INDEX_ROOT, name, name_chars, val, vl, 0);
    free(val);
    return rc;
}

/* ----------------------------------------------------- $ATTRIBUTE_LIST (links) */
/* One 32-byte ATTR_LIST_ENTRY for an unnamed attribute. */
static void al_entry(uint8_t *e, uint32_t type, uint64_t ref, uint16_t inst){
    memset(e,0,0x20);
    w32(e+0x00, type);
    w16(e+0x04, 0x20);     /* record_length (8-aligned) */
    e[0x06]=0;             /* name_length */
    e[0x07]=0x1A;          /* name_offset */
    w64(e+0x08, 0);        /* lowest_vcn */
    w64(e+0x10, ref);      /* base/extension MFT reference holding the attribute */
    w16(e+0x18, inst);     /* the attribute's instance id within that record */
}

/* Convert a heavily hard-linked base record (one with stashed overflow names)
 * into $ATTRIBUTE_LIST form: rebuild the base as STD_INFO + non-resident
 * $ATTRIBUTE_LIST + one $FILE_NAME + $DATA, spill the rest of the names into
 * fresh extension records (base_record_ref -> base), and write the attribute list
 * (STD_INFO@base, every $FILE_NAME@its record, $DATA@base) to its own cluster.
 * Handles big WinSxS hard-link groups. */
static int convert_to_attrlist(ntfs_writer_t *w, uint32_t base_recno){
    mftrec_t *base = w->rec[base_recno];
    uint64_t base_ref = (uint64_t)base_recno | ((uint64_t)base->seq << 48);

    /* 1) pull STD_INFO raw, DATA raw, and every $FILE_NAME body out of the base */
    uint8_t std_raw[512], data_raw[1024]; uint32_t std_len=0, data_len=0;
    uint32_t cap = base->n_ov + 32;
    uint8_t **fn = malloc(cap*sizeof(*fn)); uint32_t *fnl = malloc(cap*sizeof(*fnl)); uint32_t nfn=0;
    uint32_t p=0x38;
    while (p < base->used){
        uint32_t t=r32(base->buf+p); if (t==0xFFFFFFFFu) break;
        uint32_t l=r32(base->buf+p+4); if (!l) break;
        if (t==0x10 && l<=sizeof std_raw){ memcpy(std_raw,base->buf+p,l); std_len=l; }
        else if (t==0x80 && l<=sizeof data_raw){ memcpy(data_raw,base->buf+p,l); data_len=l; }
        else if (t==0x30){
            uint32_t vo=r16(base->buf+p+0x14), vl=r32(base->buf+p+0x10);
            if (nfn==cap){ cap*=2; fn=realloc(fn,cap*sizeof(*fn)); fnl=realloc(fnl,cap*sizeof(*fnl)); }
            fn[nfn]=malloc(vl); memcpy(fn[nfn],base->buf+p+vo,vl); fnl[nfn]=vl; nfn++;
        }
        p+=l;
    }
    for (uint32_t i=0;i<base->n_ov;i++){
        if (nfn==cap){ cap*=2; fn=realloc(fn,cap*sizeof(*fn)); fnl=realloc(fnl,cap*sizeof(*fnl)); }
        fn[nfn]=base->ov[i]; fnl[nfn]=base->ov_len[i]; nfn++;   /* ownership moves to fn[] */
    }

    /* 2) plan the attribute list: 1 STD + nfn FILE_NAMEs + 1 DATA, 32 bytes each */
    uint32_t n_ent = (std_len?1:0) + nfn + (data_len?1:0);
    uint32_t al_bytes = n_ent*0x20;
    uint64_t al_clu = (al_bytes + w->cluster_size-1)/w->cluster_size;
    uint64_t al_lcn = alloc_clusters(w, al_clu);
    struct ale { uint64_t ref; uint16_t inst; } *fe = malloc((nfn?nfn:1)*sizeof(*fe)); uint32_t nfe=0;
    uint16_t std_inst=0, data_inst=0;

    /* 3) rebuild the base: STD_INFO, $ATTRIBUTE_LIST (non-res), FN#0, DATA */
    base->used=0x38; base->attr_id=0;
    if (std_len){ std_inst=(uint16_t)base->attr_id;
        memcpy(base->buf+base->used,std_raw,std_len);
        w16(base->buf+base->used+0x0E,base->attr_id); base->attr_id++; base->used+=std_len; }
    if (attr_nonres(base,0x20,NULL,0, al_lcn, al_clu, w->cluster_size, al_bytes, al_bytes, 0)) return -1;
    uint32_t fi=0;
    /* Keep FN#0 in the base only if FN#0 AND $DATA both still fit. A resident
     * $DATA can be ~700B and a long WinSxS name ~576B; together they overflow the
     * 1024B record -> in that case push ALL names to extensions (base = STD +
     * $ATTRIBUTE_LIST + $DATA only). Without this guard the unbounded $DATA memcpy
     * below smashes base->buf -> heap corruption -> SIGABRT. */
    if (nfn && base->used + align8u(0x18+fnl[0]) + data_len + 8 <= NTFS_MFT_RECSZ){
        uint16_t inst=(uint16_t)base->attr_id;
        if (attr_resident(base,0x30,NULL,0, fn[0], fnl[0], 0)==0){
            fe[nfe].ref=base_ref; fe[nfe].inst=inst; nfe++; fi=1;
        }
    }
    if (data_len){
        if (base->used + data_len + 8 > NTFS_MFT_RECSZ) return -1;   /* must not overflow the record */
        data_inst=(uint16_t)base->attr_id;
        memcpy(base->buf+base->used,data_raw,data_len);
        w16(base->buf+base->used+0x0E,base->attr_id); base->attr_id++; base->used+=data_len; }

    /* 4) spill the remaining names into extension records */
    mftrec_t *ext=NULL; uint64_t ext_ref=0;
    for (; fi<nfn; fi++){
        if (!ext){
            uint32_t er=w->nrec; ext=rec_new(w, er, 0);
            ext->base_ref=base_ref; ext->link_count=0;
            ext_ref=(uint64_t)er | ((uint64_t)ext->seq<<48);
        }
        uint16_t inst=(uint16_t)ext->attr_id;
        if (attr_resident(ext,0x30,NULL,0, fn[fi], fnl[fi], 0)!=0){ ext=NULL; fi--; continue; }
        fe[nfe].ref=ext_ref; fe[nfe].inst=inst; nfe++;
    }

    /* 5) assemble + write the attribute-list data (STD, FILE_NAMEs, DATA) */
    uint8_t *al = calloc(1, al_clu*w->cluster_size); uint32_t eo=0;
    if (std_len){ al_entry(al+eo,0x10,base_ref,std_inst); eo+=0x20; }
    for (uint32_t i=0;i<nfe;i++){ al_entry(al+eo,0x30,fe[i].ref,fe[i].inst); eo+=0x20; }
    if (data_len){ al_entry(al+eo,0x80,base_ref,data_inst); eo+=0x20; }
    blockio_write(w->io, cluster_byte(w,al_lcn), al, al_clu*w->cluster_size);
    free(al);

    for (uint32_t i=0;i<nfn;i++) free(fn[i]);   /* frees base copies + adopted ov[] bodies */
    free(fn); free(fnl); free(fe);
    free(base->ov); free(base->ov_len); base->ov=NULL; base->ov_len=NULL; base->n_ov=0; base->cap_ov=0;
    return 0;
}

/* --------------------------------------------------------------------- close */
static void put_record(ntfs_writer_t *w, uint32_t recno){
    mftrec_t *r = w->rec[recno];
    rec_finalize(r);
    apply_fixup(r->buf, NTFS_MFT_RECSZ, w->sector_size, (uint16_t)(recno+1));
    blockio_write(w->io, cluster_byte(w,w->mft_lcn) + (uint64_t)recno*NTFS_MFT_RECSZ, r->buf, NTFS_MFT_RECSZ);
}

int ntfs_writer_close(ntfs_writer_t *w){
    static const uint16_t N_MFT[]={'$','M','F','T'}, N_MIR[]={'$','M','F','T','M','i','r','r'};
    static const uint16_t N_LOG[]={'$','L','o','g','F','i','l','e'}, N_VOL[]={'$','V','o','l','u','m','e'};
    static const uint16_t N_ATD[]={'$','A','t','t','r','D','e','f'}, N_DOT[]={'.'};
    static const uint16_t N_BMP[]={'$','B','i','t','m','a','p'}, N_BOOT[]={'$','B','o','o','t'};
    static const uint16_t N_BAD[]={'$','B','a','d','C','l','u','s'}, N_SEC[]={'$','S','e','c','u','r','e'};
    static const uint16_t N_UPC[]={'$','U','p','C','a','s','e'}, N_EXT[]={'$','E','x','t','e','n','d'};
    static const uint16_t NM_BAD[]={'$','B','a','d'}, NM_INFO[]={'$','I','n','f','o'};
    static const uint16_t NM_SDS[]={'$','S','D','S'}, NM_SDH[]={'$','S','D','H'}, NM_SII[]={'$','S','I','I'};
    static const uint16_t N_QUOTA[]={'$','Q','u','o','t','a'}, N_OBJID[]={'$','O','b','j','I','d'};
    static const uint16_t N_REPARSE[]={'$','R','e','p','a','r','s','e'};
    static const uint16_t NM_Q[]={'$','Q'}, NM_O[]={'$','O'}, NM_R[]={'$','R'};
    uint64_t root_ref = ntfs_root_ref(w);
    uint64_t T = 0x01da84c3f3b79090ull;  /* a fixed FILETIME for metafiles */

    /* 1) root (rec5) + $Extend (rec11) get their $STD_INFO/$FILE_NAME first (must
     *    precede $INDEX_ROOT for ascending attribute order), then build EVERY
     *    directory's $I30 index — this allocates INDX clusters for large dirs and
     *    so must run before metafile cluster allocation below. */
    {
        mftrec_t *r=w->rec[NTFS_REC_ROOT];
        uint8_t si[72]; build_std_info(si,T,T,T,w->root_attrs,w->root_sec_id); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+32]; uint32_t l=build_file_name(fn,root_ref,T,T,T,0,0,w->root_attrs,N_DOT,1,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        /* The root directory indexes itself: its "." $FILE_NAME is an entry in its
         * own $I30 (the root's "." self-link). Without
         * this the root index lacks the self-link the kernel expects. */
        dir_push(r, root_ref, fn, (uint16_t)l);
    }
    {
        mftrec_t *r=w->rec[NTFS_REC_EXTEND];
        uint8_t si[72]; build_std_info(si,T,T,T,0x10000006,0x101); attr_resident(r,0x10,NULL,0,si,72,0); /* $Extend sec_id 0x101 */
        uint8_t fn[0x42+32]; uint32_t l=build_file_name(fn,root_ref,T,T,T,0,0,0x10000006,N_EXT,7,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
    }
    /* Insert the named system metafiles (recs 0-4,6-11) into the ROOT directory's
     * $I30 index. Each metafile carries a
     * $FILE_NAME claiming parent=root; without the matching root index entries the
     * volume is inconsistent and, critically, the kernel cannot open \$Extend (and
     * the other metafiles) BY NAME at mount -> bugcheck NTFS_FILE_SYSTEM (0x24).
     * Sizes are left 0 here (the kernel reads real sizes from the target record;
     * the index entry's cached sizes are not a collation key). */
    {
        /* $AttrDef name is "$AttrDef" = 8 chars; the count must be the full name
         * length. Matched here and at the rec-4 builder below. */
        struct { uint32_t rec; const uint16_t *nm; int nc; } sysf[] = {
            {NTFS_REC_MFT,N_MFT,4},   {NTFS_REC_MFTMIRR,N_MIR,8}, {NTFS_REC_LOGFILE,N_LOG,8},
            {NTFS_REC_VOLUME,N_VOL,7},{NTFS_REC_ATTRDEF,N_ATD,8}, {NTFS_REC_BITMAP,N_BMP,7},
            {NTFS_REC_BOOT,N_BOOT,5}, {NTFS_REC_BADCLUS,N_BAD,8}, {NTFS_REC_SECURE,N_SEC,7},
            {NTFS_REC_UPCASE,N_UPC,7},{NTFS_REC_EXTEND,N_EXT,7},
        };
        mftrec_t *root = w->rec[NTFS_REC_ROOT];
        for (unsigned k=0;k<sizeof sysf/sizeof sysf[0];k++){
            mftrec_t *sr = w->rec[sysf[k].rec];
            /* $Secure is a VIEW_INDEX metafile: its root-index $FILE_NAME must
             * equal the record's own (0x20000006), since one
             * FILE_NAME_ATTR serves for both. */
            uint32_t da;
            if (sysf[k].rec == NTFS_REC_SECURE) da = 0x20000006u;
            else da = sr->is_dir ? 0x10000006u : 0x06u;
            uint8_t fn[0x42+32];
            uint32_t l = build_file_name(fn, root_ref, T,T,T, 0,0, da, sysf[k].nm, sysf[k].nc, 3);
            uint64_t ref = (uint64_t)sysf[k].rec | ((uint64_t)sr->seq << 48);
            dir_push(root, ref, fn, (uint16_t)l);
        }
    }

    /* $Extend children: $Quota(24)/$ObjId(25)/$Reparse(26) — VIEW_INDEX metafiles
     * Each gets MFT flags 0x0d (IN_USE|IS_4|IS_VIEW_INDEX), a
     * $STD_INFO and $FILE_NAME (parent=$Extend, ns=3 WIN32_AND_DOS, dos-attrs
     * 0x20000026 = HIDDEN|SYSTEM|ARCHIVE|VIEW_INDEX_PRESENT, sec_id 0x101), and
     * empty view-index $INDEX_ROOT(s). Built here (not in the rec-fill section) so
     * the $FILE_NAME keys can be dir_push'd into $Extend BEFORE its $I30 is built by
     * the loop below. create_hardlink_res inserts the index entry into rec 11 only;
     * the file's own $FILE_NAME attr is added to the record. */
    {
        uint64_t extend_ref = (uint64_t)NTFS_REC_EXTEND
                            | ((uint64_t)w->rec[NTFS_REC_EXTEND]->seq << 48);
        mftrec_t *extend = w->rec[NTFS_REC_EXTEND];
        struct { uint32_t rec; const uint16_t *nm; int nc; } extf[] = {
            {24,N_QUOTA,6},{25,N_OBJID,6},{26,N_REPARSE,8},
        };
        for (unsigned k=0;k<sizeof extf/sizeof extf[0];k++){
            mftrec_t *r = w->rec[extf[k].rec];
            r->flags |= 0x04 | 0x08;            /* MFT_RECORD_IS_4 | IS_VIEW_INDEX */
            /* $STD_INFO (dos-attrs 0x20000026; build_std_info strips only 0x10000000,
             * which 0x20000026 lacks). sec_id 0x101. */
            uint8_t si[72]; build_std_info(si,T,T,T,0x20000026,0x101);
            attr_resident(r,0x10,NULL,0,si,72,0);
            /* $FILE_NAME: parent=$Extend, ns=3 WIN32_AND_DOS, alloc=real=0. */
            uint8_t fn[0x42+32];
            uint32_t l = build_file_name(fn, extend_ref, T,T,T, 0,0, 0x20000026,
                                         extf[k].nm, extf[k].nc, 3);
            attr_resident(r,0x30,NULL,0,fn,l,0);
            /* index entry goes into $Extend's $I30 only (create_hardlink_res). */
            uint64_t ref = (uint64_t)extf[k].rec | ((uint64_t)r->seq << 48);
            dir_push(extend, ref, fn, (uint16_t)l);
        }
        /* $Quota (24): $Q (COLLATION_NTOFS_ULONG=0x10) + $O (COLLATION_NTOFS_SID=0x11)
         * $Q carries the 2 default owner entries
         * ($O is left empty per spec). */
        {
            mftrec_t *r = w->rec[24];
            /* Build the $Q body: two INDEX_ENTRYs (owner_id 1 then 0x100), ascending. */
            uint8_t qbody[0x48+0x58]; memset(qbody,0,sizeof qbody);
            /* q1: owner_id=1, 0x48 bytes. */
            {
                uint8_t *e = qbody;
                w16(e+0x00, 0x14);              /* data_offset */
                w16(e+0x02, 0x30);              /* data_length */
                w16(e+0x08, 0x48);              /* length */
                w16(e+0x0A, 0x04);              /* key_length */
                w32(e+0x10, 0x01);             /* key.owner_id = 1 */
                uint8_t *d = e+0x14;           /* QUOTA_CONTROL_ENTRY */
                w32(d+0x00, 0x02);             /* version */
                w32(d+0x04, 0x01);             /* QUOTA_FLAG_DEFAULT_LIMITS */
                w64(d+0x08, 0);                /* bytes_used */
                w64(d+0x10, T);                /* change_time */
                w64(d+0x18, (uint64_t)-1);     /* threshold = -1 */
                w64(d+0x20, (uint64_t)-1);     /* limit = -1 */
                w64(d+0x28, 0);                /* exceeded_time */
            }
            /* q2: owner_id=0x100 (QUOTA_FIRST_USER_ID), 0x58 bytes w/ SID
             * S-1-5-32-544. */
            {
                uint8_t *e = qbody+0x48;
                w16(e+0x00, 0x14);              /* data_offset */
                w16(e+0x02, 0x40);              /* data_length */
                w16(e+0x08, 0x58);              /* length */
                w16(e+0x0A, 0x04);              /* key_length */
                w32(e+0x10, 0x100);            /* key.owner_id = QUOTA_FIRST_USER_ID */
                uint8_t *d = e+0x14;
                w32(d+0x00, 0x02);             /* version */
                w32(d+0x04, 0x01);             /* QUOTA_FLAG_DEFAULT_LIMITS */
                w64(d+0x08, 0);                /* bytes_used */
                w64(d+0x10, T);                /* change_time */
                w64(d+0x18, (uint64_t)-1);     /* threshold = -1 */
                w64(d+0x20, (uint64_t)-1);     /* limit = -1 */
                w64(d+0x28, 0);                /* exceeded_time */
                /* sid = S-1-5-32-544 (BUILTIN\Administrators), at data+0x30 */
                uint8_t *s = d+0x30;
                s[0]=1;                        /* revision */
                s[1]=2;                        /* sub_authority_count */
                s[7]=0x05;                     /* identifier_authority = 5 */
                w32(s+0x08, 0x20);            /* SECURITY_BUILTIN_DOMAIN_RID = 32 */
                w32(s+0x0C, 0x220);           /* DOMAIN_ALIAS_RID_ADMINS = 544 */
            }
            /* "$O" < "$Q" by name, so emit $O first then $Q (same type 0x90;
             * chkdsk flags "$Quota" record attributes unsorted otherwise). */
            if (attr_view_index_root(r,NM_O,2,0x11,NULL,0)) return -1;
            if (attr_view_index_root(r,NM_Q,2,0x10,qbody,0x48+0x58)) return -1;
        }
        /* $ObjId (25): $O (COLLATION_NTOFS_ULONGS=0x13), empty. */
        if (attr_view_index_root(w->rec[25],NM_O,2,0x13,NULL,0)) return -1;
        /* $Reparse (26): $R index (COLLATION_NTOFS_ULONGS=0x13) of every reparse
         * point: REPARSE_INDEX_KEY{reparse_tag:u32, file_id:MFT_REF} (12B key, no
         * data). Without these entries chkdsk re-"Inserts" them into $R. */
        if (w->n_rpl == 0){
            if (attr_view_index_root(w->rec[26],NM_R,2,0x13,NULL,0)) return -1;
        } else {
            /* NTOFS_ULONGS order: by tag, then file_id low u32, then high u32 */
            for (uint32_t a=0;a<w->n_rpl;a++) for (uint32_t b=a+1;b<w->n_rpl;b++){
                uint32_t ta=w->rpl[a].tag, tb=w->rpl[b].tag;
                uint32_t la=(uint32_t)w->rpl[a].ref, lb=(uint32_t)w->rpl[b].ref;
                uint32_t ha=(uint32_t)(w->rpl[a].ref>>32), hb=(uint32_t)(w->rpl[b].ref>>32);
                int gt = (ta!=tb)?(ta>tb):((la!=lb)?(la>lb):(ha>hb));
                if (gt){ typeof(w->rpl[a]) t=w->rpl[a]; w->rpl[a]=w->rpl[b]; w->rpl[b]=t; }
            }
            uint32_t blen = w->n_rpl * 0x20;
            uint8_t *body = calloc(1, blen);
            for (uint32_t k=0;k<w->n_rpl;k++){
                uint8_t *e = body + k*0x20;
                w16(e+0x00, 0x1C);                 /* data_offset = 0x10 + key_len(12) */
                w16(e+0x02, 0);                    /* data_length = 0 */
                w16(e+0x08, 0x20);                 /* entry length */
                w16(e+0x0A, 0x0C);                 /* key_length = 12 */
                w16(e+0x0C, 0);                    /* flags: leaf, not last */
                w32(e+0x10, w->rpl[k].tag);        /* key: reparse_tag */
                w64(e+0x14, w->rpl[k].ref);        /* key: file_id (MFT ref) */
            }
            int rc = attr_view_index_root(w->rec[26],NM_R,2,0x13,body,blen);
            free(body);
            if (rc) return -1;
        }
    }

    for (uint32_t i=0; i<w->nrec; i++){
        mftrec_t *d = w->rec[i];
        if (d && d->is_dir){
            if (build_directory_index(w, d) != 0){
                fprintf(stderr, "ntfs: index build failed for rec=%u (nentries=%u used=%u)\n",
                        i, d->nentries, d->used);
                return -1;
            }
            /* Directory reparse point (junction/symlink): $REPARSE_POINT(0xC0)
             * comes AFTER $INDEX_ROOT(0x90)/$INDEX_ALLOCATION/$BITMAP in ascending
             * attribute order. The $REPARSE_POINT attribute
             * carries the real reparse data. */
            if (d->reparse){
                if (attr_resident(d, NTFS_AT_REPARSE_POINT, NULL,0, d->reparse, d->reparse_len, 0)){
                    fprintf(stderr, "ntfs: reparse attr overflow rec=%u\n", i);
                    return -1;
                }
            }
        }
    }

    /* 1.5) spill heavily hard-linked inodes (stashed overflow names) into
     *      $ATTRIBUTE_LIST + extension records. Snapshot the count first: the
     *      conversion appends extension records (recno >= n0) which must NOT be
     *      re-scanned. Must run before MFT sizing so extensions are counted. */
    {
        uint32_t n0 = w->nrec, converted=0;
        for (uint32_t i=0;i<n0;i++){
            if (w->rec[i] && w->rec[i]->n_ov){
                if (convert_to_attrlist(w, i) != 0){
                    fprintf(stderr,"ntfs: attribute-list build failed for rec=%u\n", i);
                    return -5;
                }
                converted++;
            }
        }
        if (converted) fprintf(stderr,"ntfs: %u inodes spilled to $ATTRIBUTE_LIST (%u total records)\n", converted, w->nrec);
    }

    /* 2) sizes that drive cluster allocation */
    uint32_t mft_count = w->nrec;
    w->mft_clusters = ( (uint64_t)mft_count*NTFS_MFT_RECSZ + w->cluster_size-1)/w->cluster_size;
    uint32_t attrdef_len; uint8_t *attrdef = build_attrdef(&attrdef_len);
    w->attrdef_clusters = (attrdef_len + w->cluster_size-1)/w->cluster_size;
    uint64_t vol_bytes = (w->part_sectors-1)*512ull;
    uint64_t logfile_bytes = vol_bytes/200; if (logfile_bytes<1u<<20) logfile_bytes=1u<<20; if (logfile_bytes>64u<<20) logfile_bytes=64u<<20;
    logfile_bytes = align_up(logfile_bytes, w->cluster_size);
    w->logfile_clusters = logfile_bytes/w->cluster_size;
    /* $SDS: one mirrored block per descriptor set; data = 0x40000 + tail */
    uint32_t sds_tail=0;
    for (uint32_t i=0;i<w->nsec;i++){ w->sec[i].sds_offset = sds_tail; sds_tail = (uint32_t)align_up(sds_tail + 20 + w->sec[i].sd_len, 16); }
    /* $SDS data_size = the LOGICAL end (mirror of the last descriptor), NOT the
     * cluster-aligned allocation. Windows/chkdsk walk $SDS up to data_size; trailing
     * zero padding inside data_size reads as a 0-length entry and crashes sdchk.cxx
     * (and corrupts Windows' next-descriptor offset -> over-allocation). Match the
     * native installer, whose data_size is the exact last-entry end (not aligned). */
    uint64_t sds_data_size = w->nsec
        ? (uint64_t)0x40000 + w->sec[w->nsec-1].sds_offset + 20 + w->sec[w->nsec-1].sd_len
        : (uint64_t)0x40000;
    uint64_t sds_bytes = align_up((uint64_t)0x40000 + sds_tail, w->cluster_size);
    w->sds_clusters = sds_bytes/w->cluster_size;

    /* ---- place the low fixed-position metafiles (NTFS-faithful) -----------
     * MFT-record $BITMAP @ LCN 2 (right after $Boot), then $MFT just past it (and
     * at least 16 KiB in). Both live below mft_zone_end, in front of the free MFT
     * zone. $MFTMirr was fixed at the volume midpoint in open(). */
    uint64_t boot_clusters = (8192 + w->cluster_size-1)/w->cluster_size;  /* $Boot = [0,2) */
    uint64_t mftbmp_bytes = align_up(((uint64_t)mft_count+7)/8, 8);
    int      mftbmp_resident = (mftbmp_bytes <= 0x40);
    uint64_t mftbmp_lcn = 0, mftbmp_clusters = 0;
    if (!mftbmp_resident){
        mftbmp_clusters = (mftbmp_bytes + w->cluster_size-1)/w->cluster_size;
        mftbmp_lcn = boot_clusters;                  /* LCN 2 ($Boot is [0,2)) */
        bmp_mark(w, mftbmp_lcn, mftbmp_clusters);
    }
    w->mft_lcn = boot_clusters + mftbmp_clusters;
    if (w->mft_lcn * w->cluster_size < 16*1024)
        w->mft_lcn = (16*1024 + w->cluster_size-1)/w->cluster_size;
    bmp_mark(w, w->mft_lcn, w->mft_clusters);
    if (w->mft_lcn + w->mft_clusters > w->mft_zone_end) return -3;  /* MFT overruns its zone */
    if (w->mftmirr_lcn + w->reserve_len > w->total_clusters) return -3;

    /* ---- the rest bump from the data zone (after all file data + INDX blocks) -
     * remaining metafiles, $LogFile, and finally the cluster $Bitmap. */
    w->upcase_lcn  = alloc_clusters(w, (0x20000 + w->cluster_size-1)/w->cluster_size);
    w->attrdef_lcn = alloc_clusters(w, w->attrdef_clusters);
    w->logfile_lcn = alloc_clusters(w, w->logfile_clusters);
    w->sds_lcn     = alloc_clusters(w, w->sds_clusters);
    uint64_t bitmap_bytes = w->clbmp_bytes;
    w->bitmap_clusters = bitmap_bytes/w->cluster_size;
    w->bitmap_lcn = alloc_clusters(w, w->bitmap_clusters);
    if (w->next_lcn > w->total_clusters) return -2;   /* volume too small */

    /* 3) fill the 16 metafile records */
    /* rec 0 $MFT */
    {
        mftrec_t *r=w->rec[NTFS_REC_MFT];
        uint8_t si[72]; build_std_info(si,T,T,T, 0x06, 0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+32]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->mft_clusters*w->cluster_size,(uint64_t)mft_count*NTFS_MFT_RECSZ,0x06,N_MFT,4,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_nonres(r,0x80,NULL,0,w->mft_lcn,w->mft_clusters,w->cluster_size,(uint64_t)mft_count*NTFS_MFT_RECSZ,(uint64_t)mft_count*NTFS_MFT_RECSZ,0);
        /* $BITMAP of MFT records (mft_count bits): resident if tiny, else non-res.
         * NTFS marks every record in-use EXCEPT the reserved
         * range 16-23 ("i<16 || i>23"); records 16-23 are formatted-but-free. */
        if (mftbmp_resident){
            uint8_t mb[0x40]; memset(mb,0,sizeof mb);
            for (uint32_t k=0;k<mft_count;k++) if (k<16 || k>23) mb[k/8]|=1<<(k&7);
            attr_resident(r,0xB0,NULL,0,mb,align8u((uint32_t)mftbmp_bytes),0);
        } else {
            uint64_t alloc = mftbmp_clusters*w->cluster_size;
            uint8_t *mb = calloc(1, alloc);
            for (uint32_t k=0;k<mft_count;k++) if (k<16 || k>23) mb[k/8]|=1<<(k&7);
            blockio_write(w->io, cluster_byte(w,mftbmp_lcn), mb, alloc);
            free(mb);
            attr_nonres(r,0xB0,NULL,0,mftbmp_lcn,mftbmp_clusters,w->cluster_size,mftbmp_bytes,mftbmp_bytes,0);
        }
    }
    /* rec 1 $MFTMirr */
    {
        mftrec_t *r=w->rec[NTFS_REC_MFTMIRR];
        uint8_t si[72]; build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->cluster_size,4*NTFS_MFT_RECSZ,0x06,N_MIR,8,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_nonres(r,0x80,NULL,0,w->mftmirr_lcn,1,w->cluster_size,4*NTFS_MFT_RECSZ,4*NTFS_MFT_RECSZ,0);
    }
    /* rec 2 $LogFile */
    {
        mftrec_t *r=w->rec[NTFS_REC_LOGFILE];
        uint8_t si[72]; build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->logfile_clusters*w->cluster_size,logfile_bytes,0x06,N_LOG,8,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_nonres(r,0x80,NULL,0,w->logfile_lcn,w->logfile_clusters,w->cluster_size,logfile_bytes,logfile_bytes,0);
    }
    /* rec 3 $Volume */
    {
        mftrec_t *r=w->rec[NTFS_REC_VOLUME];
        /* Use the shared $Secure security_id 0x100 (like $MFT etc.), NOT a standalone
         * $SECURITY_DESCRIPTOR + security_id 0: Windows migrates standalone-SD
         * metafiles into $Secure at boot, leaving dangling ids that chkdsk reports
         * as "missing/invalid security descriptor". */
        uint8_t si[72]; uint32_t sl=build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,sl,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,0,0,0x06,N_VOL,7,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        if (w->label_chars){ uint16_t lab[32]; for(int i=0;i<w->label_chars;i++) lab[i]=(uint8_t)w->label[i]; attr_resident(r,0x60,NULL,0,lab,2*w->label_chars,0); }
        uint8_t vi[12]; memset(vi,0,12); vi[8]=3; vi[9]=1; attr_resident(r,0x70,NULL,0,vi,12,0);
        /* $Volume carries an empty unnamed $DATA (NULL/0 $DATA attr on rec 3).
         * Appended last (0x80 > 0x70). */
        attr_resident(r,0x80,NULL,0,NULL,0,0);
    }
    /* rec 4 $AttrDef */
    {
        mftrec_t *r=w->rec[NTFS_REC_ATTRDEF];
        uint8_t si[72]; uint32_t sl=build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,sl,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->attrdef_clusters*w->cluster_size,attrdef_len,0x06,N_ATD,8,3); /* "$AttrDef" = 8 chars */
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_nonres(r,0x80,NULL,0,w->attrdef_lcn,w->attrdef_clusters,w->cluster_size,attrdef_len,attrdef_len,0);
    }
    /* rec 5 root dir: $STD_INFO/$FILE_NAME/$INDEX_ROOT already built in step 1 */
    /* rec 6 $Bitmap */
    {
        mftrec_t *r=w->rec[NTFS_REC_BITMAP];
        uint8_t si[72]; build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->bitmap_clusters*w->cluster_size,(w->total_clusters+7)/8,0x06,N_BMP,7,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_nonres(r,0x80,NULL,0,w->bitmap_lcn,w->bitmap_clusters,w->cluster_size,(w->total_clusters+7)/8,(w->total_clusters+7)/8,0);
    }
    /* rec 7 $Boot */
    {
        mftrec_t *r=w->rec[NTFS_REC_BOOT];
        uint8_t si[72]; uint32_t sl=build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,sl,0);
        uint8_t fn[0x42+32]; uint32_t l=build_file_name(fn,root_ref,T,T,T,2*w->cluster_size,8192,0x06,N_BOOT,5,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        uint64_t bootclu=(8192+w->cluster_size-1)/w->cluster_size;
        attr_nonres(r,0x80,NULL,0,0,bootclu,w->cluster_size,8192,8192,0);
    }
    /* rec 8 $BadClus (named sparse $DATA '$Bad' over whole volume) */
    {
        mftrec_t *r=w->rec[NTFS_REC_BADCLUS];
        uint8_t si[72]; build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,0,0,0x06,N_BAD,8,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        attr_resident(r,0x80,NULL,0,NULL,0,0);                 /* unnamed empty $DATA */
        /* named '$Bad': the non-resident $BadClus:$Bad named stream.
         * It is NOT a sparse attribute (flags=0x0000) -- it is the special $BadClus
         * case where allocated_size = data_size = whole-volume size, but the runlist
         * is ONE HOLE run (offSize=0) since there are no bad clusters. Standard
         * 0x40-byte non-res header (no compressed_size), name at 0x40, mp at 0x48.
         * Header bytes: 80000000 50000000 01044000 00000100 ... alloc=data=volsize. */
        uint8_t a[0x60]; memset(a,0,sizeof a);
        int lb = min_bytes_s((int64_t)w->total_clusters);
        uint32_t name_off = 0x40;
        uint32_t mp_off = align8u(name_off + 2*4);   /* "$Bad" = 4 chars (8 bytes) -> 0x48 */
        uint32_t total  = align8u(mp_off + 1 + lb + 1);
        w32(a+0x00,0x80); w32(a+0x04,total); a[0x08]=1; a[0x09]=4; w16(a+0x0A,(uint16_t)name_off);
        w16(a+0x0C,0x0000); w16(a+0x0E,r->attr_id++);          /* flags = 0 (not sparse) */
        w64(a+0x10,0);                                          /* start_vcn */
        w64(a+0x18, w->total_clusters?w->total_clusters-1:0);  /* last_vcn */
        w16(a+0x20,(uint16_t)mp_off); w16(a+0x22,0);           /* mp_off ; compression_unit=0 */
        w64(a+0x28, (uint64_t)w->total_clusters*w->cluster_size); /* allocated_size = volume */
        w64(a+0x30, (uint64_t)w->total_clusters*w->cluster_size); /* data_size = volume */
        w64(a+0x38, 0);                                         /* initialized_size = 0 */
        memcpy(a+name_off,NM_BAD,8);
        uint8_t *mp = a+mp_off;                                 /* one hole run (offSize=0) */
        mp[0] = (uint8_t)lb;                                    /* lenSize | (offSize=0<<4) */
        for (int i=0;i<lb;i++) mp[1+i] = (uint8_t)(w->total_clusters >> (8*i));
        mp[1+lb] = 0;                                           /* terminator */
        memcpy(r->buf+r->used,a,total); r->used+=total;
    }
    /* rec 9 $Secure (B-tree $SDH/$SII -> holds the WIM's hundreds of descriptors) */
    {
        mftrec_t *r=w->rec[NTFS_REC_SECURE];
        /* $Secure is a VIEW_INDEX metafile: set MFT_RECORD_IS_VIEW_INDEX (0x08)
         * flags 0x09, and its $STD_INFO/$FILE_NAME dos-attrs are
         * HIDDEN|SYSTEM|VIEW_INDEX_PRESENT = 0x20000006. Its own
         * security_id is 0x101 (the metafile descriptor), not 0x100. */
        r->flags |= 0x08;
        uint8_t si[72]; build_std_info(si,T,T,T,0x20000006,0x101); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,w->sds_clusters*w->cluster_size,sds_data_size,0x20000006,N_SEC,7,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        /* $DATA:$SDS non-resident -- data_size is the LOGICAL end (not cluster-aligned) */
        attr_nonres(r,0x80,NM_SDS,4,w->sds_lcn,w->sds_clusters,w->cluster_size,sds_data_size,sds_data_size,0);

        uint32_t n=w->nsec;
        uint8_t  *data20 = malloc((size_t)n*20 + 1);   /* $SII data: hash,id,sds_off,len (20B) */
        uint8_t  *data24 = malloc((size_t)n*24 + 1);   /* $SDH data: same + reserved_II (24B) */
        uint8_t  *siikey = malloc((size_t)n*4 + 1);
        uint8_t  *sdhkey = malloc((size_t)n*8 + 1);
        bentry_t *sii = malloc((size_t)n*sizeof(bentry_t) + 1);
        bentry_t *sdh = malloc((size_t)n*sizeof(bentry_t) + 1);
        for (uint32_t i=0;i<n;i++){
            uint8_t *d=data20+i*20;
            w32(d+0,w->sec[i].hash); w32(d+4,(uint32_t)w->sec[i].id); w64(d+8,w->sec[i].sds_offset); w32(d+16,20+w->sec[i].sd_len);
            /* $SDH data is the $SII datum (20B) + a 4-byte reserved_II "II" trailer
             * (0x00490049), making $SDH index entries 24B of data.
             * Windows writes this on every $SDH entry; omitting it makes our $SDH
             * entries the wrong size, breaking the dedup-by-hash lookup Windows uses
             * when ADDING new security descriptors (e.g. during specialize). */
            uint8_t *d2=data24+i*24;
            memcpy(d2,d,20); w32(d2+20,0x00490049);
            w32(siikey+i*4,(uint32_t)w->sec[i].id);
            w32(sdhkey+i*8,w->sec[i].hash); w32(sdhkey+i*8+4,(uint32_t)w->sec[i].id);
            sii[i]=(bentry_t){.key=siikey+i*4,.key_len=4,.data=d,.data_len=20};
            sdh[i]=(bentry_t){.key=sdhkey+i*8,.key_len=8,.data=d2,.data_len=24};
        }
        /* $SII keyed by security_id: w->sec is already id-ascending. $SDH keyed by
         * COLLATION_NTOFS_SECURITY_HASH: sort by (hash, id). */
        qsort(sdh, n, sizeof(bentry_t), secidx_sdh_cmp);

        /* Budget the resident roots small so both ($SDH,$SII) roots + their
         * $INDEX_ALLOCATION + $BITMAP attrs share the 1024B record. */
        uint32_t avail = (NTFS_MFT_RECSZ > r->used) ? NTFS_MFT_RECSZ - r->used : 0;
        uint32_t per_root = (avail > 0x200) ? 0x40 : 0x20;
        secidx_t SDH, SII;
        prepare_secure_index(w, 0x12 /*SECURITY_HASH*/, sdh, n, per_root, &SDH);
        prepare_secure_index(w, 0x10 /*ULONG*/,         sii, n, per_root, &SII);

        /* Emit record attrs in (type, name) order: 0x90 $SDH,$SII ; 0xA0 ; 0xB0. */
        int rc = attr_resident(r,0x90,NM_SDH,4,SDH.root,SDH.root_len,0);
        rc |= attr_resident(r,0x90,NM_SII,4,SII.root,SII.root_len,0);
        if (SDH.large) rc |= attr_nonres(r,0xA0,NM_SDH,4,SDH.idx_lcn,SDH.idx_blocks,w->cluster_size,SDH.idx_blocks*NTFS_INDX_SIZE,SDH.idx_blocks*NTFS_INDX_SIZE,0);
        if (SII.large) rc |= attr_nonres(r,0xA0,NM_SII,4,SII.idx_lcn,SII.idx_blocks,w->cluster_size,SII.idx_blocks*NTFS_INDX_SIZE,SII.idx_blocks*NTFS_INDX_SIZE,0);
        if (SDH.large) rc |= attr_resident(r,0xB0,NM_SDH,4,SDH.bm,SDH.bm_len,0);
        if (SII.large) rc |= attr_resident(r,0xB0,NM_SII,4,SII.bm,SII.bm_len,0);
        free(SDH.root); free(SII.root); free(SDH.bm); free(SII.bm);
        free(data20); free(data24); free(siikey); free(sdhkey); free(sii); free(sdh);
        if (rc){ return -4; }   /* $Secure record overflowed 1024B (lower per_root) */
    }
    /* rec 10 $UpCase */
    {
        mftrec_t *r=w->rec[NTFS_REC_UPCASE];
        uint8_t si[72]; build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,72,0);
        uint8_t fn[0x42+16]; uint32_t l=build_file_name(fn,root_ref,T,T,T,0x20000,0x20000,0x06,N_UPC,7,3);
        attr_resident(r,0x30,NULL,0,fn,l,0);
        uint64_t upc_clu=(0x20000+w->cluster_size-1)/w->cluster_size;
        attr_nonres(r,0x80,NULL,0,w->upcase_lcn,upc_clu,w->cluster_size,0x20000,0x20000,0);
        /* $UpCase:$Info named $DATA stream (Win8+): 32-byte
         * UPCASEINFO {len=0x20, filler=0, crc64(table), osmajor/minor/build/pack=0}.
         * Ordered after the unnamed $DATA (same type 0x80, "" < "$Info"). */
        uint8_t info[32]; memset(info,0,32);
        w32(info+0,0x20);                                            /* len */
        w64(info+8, crc64_ecma((const uint8_t*)g_upcase, 0x20000));  /* crc of upcase table */
        attr_resident(r,0x80,NM_INFO,5,info,32,0);
    }
    /* rec 11 $Extend: $STD_INFO/$FILE_NAME/$INDEX_ROOT already built in step 1 */
    /* rec 12-15 reserved: $STD_INFO(shared security_id 0x100) + empty $DATA, marked
     * in-use, no $FILE_NAME. (Shared $Secure id, not a standalone $SECURITY_DESCRIPTOR
     * + id 0, so Windows has nothing to migrate into $Secure at boot.) */
    for (uint32_t i=12;i<16;i++){
        mftrec_t *r=w->rec[i];
        r->link_count = 0;                          /* reserved: no $FILE_NAME -> 0 hard links (matches ref) */
        uint8_t si[72]; uint32_t sl=build_std_info(si,T,T,T,0x06,0x100); attr_resident(r,0x10,NULL,0,si,sl,0);
        attr_resident(r,0x80,NULL,0,NULL,0,0);
    }
    /* rec 16-23 reserved: NTFS leaves these as formatted-but-FREE FILE records
     * (file numbering starts at 24, ignoring 16-23): no attributes
     * and the IN_USE flag CLEARED. They are also marked free in the MFT $BITMAP
     * above. writer_open created them via rec_new (IN_USE set); undo that here. */
    for (uint32_t i=16;i<24;i++){
        mftrec_t *r=w->rec[i];
        if (!r) continue;
        r->flags = 0x0000;          /* clear IN_USE (and DIRECTORY) */
        r->link_count = 0;
        r->used = 0x38;             /* drop any attributes -> first_attr_offset only */
        r->attr_id = 0;
    }

    /* 4) write the MFT (records 0..mft_count-1, fixup-applied) */
    for (uint32_t i=0;i<mft_count;i++) if (w->rec[i]) put_record(w, i);

    /* 5) $MFTMirr data = copy of records 0-3 (already finalized+fixup'd on disk) */
    {
        uint8_t mir[4*NTFS_MFT_RECSZ];
        for (int i=0;i<4;i++) memcpy(mir+i*NTFS_MFT_RECSZ, w->rec[i]->buf, NTFS_MFT_RECSZ);
        blockio_write(w->io, cluster_byte(w,w->mftmirr_lcn), mir, sizeof mir);
    }

    /* 6) $UpCase data */
    {
        uint8_t *u = malloc(0x20000);
        for (int c=0;c<65536;c++){ u[2*c]=(uint8_t)g_upcase[c]; u[2*c+1]=(uint8_t)(g_upcase[c]>>8); }
        blockio_write(w->io, cluster_byte(w,w->upcase_lcn), u, 0x20000);
        free(u);
    }
    /* 7) $AttrDef data */
    blockio_write(w->io, cluster_byte(w,w->attrdef_lcn), attrdef, attrdef_len);
    free(attrdef);

    /* 8) $LogFile = 0xFF */
    {
        uint8_t *f = malloc(1u<<20); memset(f,0xFF,1u<<20);
        uint64_t rem = logfile_bytes, base = cluster_byte(w,w->logfile_lcn);
        while (rem){ uint64_t k = rem<(1u<<20)?rem:(1u<<20); blockio_write(w->io, base, f, k); base+=k; rem-=k; }
        free(f);
    }

    /* 9) $Secure $SDS (entry + 0x40000 mirror) */
    {
        uint8_t *sds = calloc(1, sds_bytes);
        uint32_t off=0;
        for (uint32_t i=0;i<w->nsec;i++){
            uint8_t *e=sds+off;
            w32(e+0, w->sec[i].hash); w32(e+4,(uint32_t)w->sec[i].id);
            w64(e+8, w->sec[i].sds_offset); w32(e+16, 20+w->sec[i].sd_len);
            memcpy(e+20, w->sec[i].sd, w->sec[i].sd_len);
            /* mirror copy */
            memcpy(sds+0x40000+off, e, 20+w->sec[i].sd_len);
            off = (uint32_t)align_up(off + 20 + w->sec[i].sd_len, 16);
        }
        blockio_write(w->io, cluster_byte(w,w->sds_lcn), sds, sds_bytes);
        free(sds);
    }

    /* 10) cluster $Bitmap: the in-memory allocation bitmap verbatim (every cluster
     *     handed out by bmp_mark/alloc_clusters). Clusters that do not exist
     *     [total_clusters, 8*clbmp_bytes) are marked allocated so the kernel never
     *     hands them out (leaving them 0 is corruption). */
    {
        for (uint64_t c=w->total_clusters; c<w->clbmp_bytes*8; c++) w->clbmp[c>>3]|=1u<<(c&7);
        blockio_write(w->io, cluster_byte(w,w->bitmap_lcn), w->clbmp, w->clbmp_bytes);
        free(w->clbmp); w->clbmp = NULL;
    }

    /* 11) $Boot (BPB) at LBA0 of partition + backup at last sector */
    {
        uint8_t boot[512]; memset(boot,0,512);
        boot[0]=0xEB; boot[1]=0x52; boot[2]=0x90;
        memcpy(boot+3,"NTFS    ",8);
        w16(boot+0x0B, w->sector_size);
        boot[0x0D]=(uint8_t)w->spc;
        boot[0x15]=0xF8;
        w16(boot+0x18,0x3F); w16(boot+0x1A,0xFF);
        w32(boot+0x1C, (uint32_t)(w->part_off/w->sector_size)); /* hidden sectors */
        w32(boot+0x24, 0x00800080);
        w64(boot+0x28, w->part_sectors-1);
        w64(boot+0x30, w->mft_lcn);
        w64(boot+0x38, w->mftmirr_lcn);
        boot[0x40]=0xF6;                       /* clusters_per_mft_record = -10 => 1024 */
        boot[0x44]=0x01;                        /* clusters_per_index_record = 1 */
        w64(boot+0x48, w->serial);
        w16(boot+0x1FE, 0xAA55);
        blockio_write(w->io, w->part_off, boot, 512);
        /* the $Boot $DATA spans 8192 bytes at LCN0; sector0 is this BPB, rest zero (already) */
        blockio_write(w->io, w->part_off + (w->part_sectors-1)*512ull, boot, 512); /* backup */
    }

    blockio_flush(w->io);
    return 0;
}
