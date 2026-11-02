/* ext4.c - minimal ext4 image writer. See ext4.h for design notes.
 *
 * On-disk structures from the ext4 disk-format docs:
 *   https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html
 *
 * Convention: every field on disk is little-endian. Windows is LE so we
 * write naturally; if this ever runs on BE we'd need bswap wrappers.
 */
#include "ext4.h"
#include "log.h"

#include <windows.h>
#include <winioctl.h>
#include <objbase.h>     /* CoCreateGuid */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE             4096u
#define INODE_SIZE             256u
#define BLOCKS_PER_GROUP       32768u
#define INODES_PER_GROUP       1024u

#define EXT4_SUPER_MAGIC       0xEF53u

/* Reserved inodes (must match kernel's expectations). */
#define EXT4_ROOT_INO          2u
#define EXT4_JOURNAL_INO       8u
#define EXT4_JOURNAL_BLOCKS    32768u
#define EXT4_LOST_FOUND_INO    11u
#define EXT4_FIRST_INO         11u   /* s_first_ino: first non-reserved */

/* Feature flags - kept very small. */
#define EXT4_FEATURE_COMPAT     0x0Cu
#define EXT4_FEATURE_INCOMPAT   (0x02u | 0x40u)   /* FILETYPE | EXTENTS */
#define EXT4_FEATURE_RO_COMPAT  (0x01u | 0x02u | 0x08u | 0x20u | 0x40u)
                                 /* SPARSE_SUPER | LARGE_FILE | HUGE_FILE
                                  * | DIR_NLINK | EXTRA_ISIZE */

/* File-type bits in dir entry (s_feature_incompat & FILETYPE). */
#define EXT4_FT_UNKNOWN  0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR      2
#define EXT4_FT_CHRDEV   3
#define EXT4_FT_BLKDEV   4
#define EXT4_FT_FIFO     5
#define EXT4_FT_SOCK     6
#define EXT4_FT_SYMLINK  7

/* Inode mode type bits (S_IF*). */
#define S_IFMT   0xF000u
#define S_IFSOCK 0xC000u
#define S_IFLNK  0xA000u
#define S_IFREG  0x8000u
#define S_IFBLK  0x6000u
#define S_IFDIR  0x4000u
#define S_IFCHR  0x2000u
#define S_IFIFO  0x1000u

/* Inode flags. */
#define EXT4_EXTENTS_FL 0x00080000u

/* Extent constants. */
#define EXT4_EXT_MAGIC  0xF30Au

/* ---- On-disk structs ---- */

#pragma pack(push, 1)
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_update_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint8_t  s_encryption_level;
    uint8_t  s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t  s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint32_t s_last_error_line;
    uint64_t s_last_error_block;
    uint8_t  s_last_error_func[32];
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_clusters;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint8_t  s_wtime_hi;
    uint8_t  s_mtime_hi;
    uint8_t  s_mkfs_time_hi;
    uint8_t  s_lastcheck_hi;
    uint8_t  s_first_error_time_hi;
    uint8_t  s_last_error_time_hi;
    uint8_t  s_first_error_errcode;
    uint8_t  s_last_error_errcode;
    uint16_t s_encoding;
    uint16_t s_encoding_flags;
    uint32_t s_orphan_file_inum;
    uint32_t s_reserved[94];
    uint32_t s_checksum;
} ext4_superblock_t;
/* sizeof(ext4_superblock_t) must equal 1024 bytes. Compile-time check below. */

typedef struct {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
} ext4_group_desc_t;
/* sizeof = 32. */

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint8_t  i_block[60];
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_hi;
    uint32_t i_obso_faddr;
    uint16_t i_osd2_blocks_hi;
    uint16_t i_osd2_file_acl_hi;
    uint16_t i_osd2_uid_hi;
    uint16_t i_osd2_gid_hi;
    uint16_t i_osd2_checksum_lo;
    uint16_t i_osd2_reserved;
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} ext4_inode_t;
/* sizeof = 0xa0 = 160. Inode-size 256 leaves 96 trailing bytes (xattrs). */

typedef struct {
    uint8_t  e_name_len;
    uint8_t  e_name_index;
    uint16_t e_value_offs;
    uint32_t e_value_inum;
    uint32_t e_value_size;
    uint32_t e_hash;
} ext4_xattr_entry_t;

typedef struct {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;
/* sizeof = 12. */

typedef struct {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;
/* sizeof = 12. */

typedef struct {
    uint32_t ei_block;       /* first logical block this index covers */
    uint32_t ei_leaf_lo;     /* physical block holding the child node */
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;
/* sizeof = 12. */

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    /* char name[]; */
} ext4_dir_entry_t;
#pragma pack(pop)

/* ---- In-memory state ---- */

typedef struct dir_entry_rec {
    char    name[256];          /* null-terminated */
    uint8_t name_len;
    uint8_t file_type;
    uint32_t inode;
    struct dir_entry_rec *next;
} dir_entry_rec_t;

typedef struct dir_node {
    uint32_t       inode;
    struct dir_node *parent;
    dir_entry_rec_t *entries;
    dir_entry_rec_t *entries_tail;
    /* children dir_nodes - for lookup by name; linked sibling chain */
    struct dir_node *first_child;
    struct dir_node *next_sibling;
    char            name[256];  /* leaf name; "" for root */
} dir_node_t;

typedef struct {
    uint32_t  inode;
    /* On-disk representation buffered until close. We write the inode body
     * directly to its slot when we know enough; for dirs the size + extent
     * are known only after we've serialized entries. So buffer everything. */
    ext4_inode_t body;
    /* If this inode owns data blocks, they're a single contiguous extent. */
    uint32_t  data_first_block;
    uint32_t  data_block_count;
    /* For dirs only - serialized entry blob (filled at close). */
    uint8_t  *dir_blob;
    uint32_t  dir_blob_len;
    void     *capability;
    uint32_t  capability_size;
} inode_rec_t;

struct ext4_writer {
    HANDLE      phys;
    uint64_t    part_lba_start;
    uint64_t    part_lba_count;
    uint64_t    part_bytes;

    /* Layout. */
    uint32_t    total_blocks;
    uint32_t    blocks_per_group;
    uint32_t    inodes_per_group;
    uint32_t    num_groups;
    uint32_t    inode_table_blocks_per_group;
    uint32_t    total_inodes;
    /* Per-group metadata layout (relative to group start in blocks):
     *   non-backup: bbitmap=0, ibitmap=1, itable=2..2+T-1, data=2+T..
     *   backup    : sb=0, gdt=1, bbitmap=2, ibitmap=3, itable=4..4+T-1, data=4+T..
     * (We use the same layout in group 0 as backup groups since the SB+GDT
     * live there.) */
    uint32_t    gdt_blocks;
    uint8_t    *sb_buf;        /* canonical SB (1024 bytes, allocated 4096) */
    uint8_t    *gdt_buf;       /* GDT, gdt_blocks * 4096 */

    /* Block allocator: per-group "first free data block" cursor. */
    uint32_t   *next_free_block;   /* [num_groups], in group-relative blocks */
    uint32_t   *blocks_used_in_group; /* [num_groups], count of data blocks allocated */

    /* Inode allocator: bump pointer through global inode space. */
    uint32_t    next_free_inode;

    /* Inode records (1..total_inodes). Index 0 unused. Allocated lazily. */
    inode_rec_t **inodes;          /* [total_inodes + 1] */
    uint32_t      n_used_inodes;

    /* Directory tree. */
    dir_node_t *root;

    /* UUID + label. */
    uint8_t  uuid[16];
    char     label[17];

    /* Write coalescer. Synchronous WriteFile to \\.\PhysicalDriveN is
     * absurdly slow for many small writes (~30 MB/s no matter what the
     * SSD can do) because each syscall pays full I/O-manager + vhdmp +
     * NTFS round-trip latency. Coalescing contiguous adjacent writes
     * into 16 MiB chunks lets the path see a few large requests instead
     * of hundreds of thousands of small ones - 10-20x faster in practice. */
    uint8_t  *coal_buf;
    size_t    coal_buf_cap;
    uint64_t  coal_off;        /* absolute partition offset of buf[0] */
    size_t    coal_len;        /* bytes currently buffered */
    int       coal_active;     /* 0 if no active batch */

    /* I/O stats - cumulative across the writer's lifetime. */
    uint64_t  stat_bytes_in;       /* bytes passed to part_write */
    uint64_t  stat_bytes_flushed;  /* bytes that hit WriteFile */
    uint64_t  stat_flush_ticks;    /* QPC ticks spent in WriteFile */
    uint64_t  stat_flush_calls;    /* number of WriteFile syscalls */
};

/* ---- Helpers ---- */

#define MIN_OF(a,b) ((a) < (b) ? (a) : (b))

/* Mark a backup-superblock group per sparse_super: group 0, 1, and groups
 * with index == 3^n, 5^n, 7^n. */
static int is_backup_group(uint32_t g)
{
    if (g == 0 || g == 1) return 1;
    for (uint64_t v = 3; v <= g; v *= 3) if (v == g) return 1;
    for (uint64_t v = 5; v <= g; v *= 5) if (v == g) return 1;
    for (uint64_t v = 7; v <= g; v *= 7) if (v == g) return 1;
    return 0;
}

/* Compute the first data block within a group (group-relative). */
static uint32_t group_first_data_block(const ext4_writer_t *w, uint32_t g)
{
    uint32_t off = is_backup_group(g) ? (1 + w->gdt_blocks) : 0;
    off += 2 /* bitmaps */ + w->inode_table_blocks_per_group;
    return off;
}

/* Direct (unbuffered) write straight to the underlying disk handle. */
static int part_write_direct(ext4_writer_t *w, uint64_t off,
                              const void *buf, size_t len)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)(w->part_lba_start * 512ULL + off);
    if (!SetFilePointerEx(w->phys, li, NULL, FILE_BEGIN)) {
        LOG_WIN32("SetFilePointerEx(part_write_direct)", GetLastError());
        return -1;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    DWORD wr = 0;
    BOOL ok = WriteFile(w->phys, buf, (DWORD)len, &wr, NULL);
    QueryPerformanceCounter(&t1);
    w->stat_flush_ticks += (uint64_t)(t1.QuadPart - t0.QuadPart);
    w->stat_flush_calls++;
    w->stat_bytes_flushed += len;
    if (!ok || wr != (DWORD)len) {
        LOG_WIN32("WriteFile(part_write_direct)", GetLastError());
        LOG_E("  off=%lld len=%zu wr=%lu", (long long)li.QuadPart, len, wr);
        return -1;
    }
    return 0;
}

/* Flush whatever is currently in the coalescing buffer to disk. */
static int part_flush(ext4_writer_t *w)
{
    if (!w->coal_active || w->coal_len == 0) return 0;
    int rc = part_write_direct(w, w->coal_off, w->coal_buf, w->coal_len);
    w->coal_active = 0;
    w->coal_len = 0;
    return rc;
}

/* Coalesced write: route every write through the aligned coalesce buffer.
 * Contiguous adjacent writes append to the active batch (no flush, no
 * syscall). On non-contiguous or full, flush + restart. Writes larger
 * than the buffer get chunked through it.
 *
 * Everything must go through this path because the disk handle uses
 * FILE_FLAG_NO_BUFFERING and requires sector-aligned BUFFERS, not just
 * offsets/lengths - we can't direct-write a malloc'd file-data pointer. */
static int part_write(ext4_writer_t *w, uint64_t off, const void *buf, size_t len)
{
    w->stat_bytes_in += len;
    const uint8_t *src = (const uint8_t *)buf;
    while (len > 0) {
        /* If there's an active batch and we're contiguous, try to append. */
        if (w->coal_active && off == w->coal_off + (uint64_t)w->coal_len) {
            size_t room = w->coal_buf_cap - w->coal_len;
            size_t chunk = (len < room) ? len : room;
            memcpy(w->coal_buf + w->coal_len, src, chunk);
            w->coal_len += chunk;
            src += chunk;
            off += chunk;
            len -= chunk;
            if (w->coal_len == w->coal_buf_cap) {
                if (part_flush(w) != 0) return -1;
            }
            continue;
        }
        /* Not contiguous (or no active batch). Flush + start new. */
        if (part_flush(w) != 0) return -1;
        w->coal_off = off;
        w->coal_active = 1;
    }
    return 0;
}

/* Write at a logical block number. */
static int part_write_block(ext4_writer_t *w, uint32_t block_no, const void *buf, size_t blocks)
{
    return part_write(w, (uint64_t)block_no * BLOCK_SIZE, buf, blocks * BLOCK_SIZE);
}

/* Zero a block range. Caller passes a 4 KB zero buffer. */
static int part_zero_blocks(ext4_writer_t *w, uint32_t block_no,
                             uint32_t n_blocks, const uint8_t *zero_block)
{
    for (uint32_t i = 0; i < n_blocks; i++) {
        if (part_write_block(w, block_no + i, zero_block, 1) != 0) return -1;
    }
    return 0;
}

/* ---- Path navigation ---- */

/* Find or fail-to-find a path's parent dir + leaf name.
 * On entry path is absolute; on success *out_parent is the dir_node
 * containing the parent, *out_leaf is the leaf component (heap-copied). */
static int split_path(ext4_writer_t *w, const char *path,
                      dir_node_t **out_parent, char *out_leaf, size_t leaf_cap)
{
    if (path[0] != '/') return -1;
    if (path[1] == 0) return -1;  /* "/" itself */

    /* Walk components. */
    dir_node_t *cur = w->root;
    const char *p = path + 1;
    char comp[256];

    while (1) {
        const char *slash = strchr(p, '/');
        size_t complen;
        if (slash) {
            complen = (size_t)(slash - p);
        } else {
            complen = strlen(p);
        }
        if (complen == 0 || complen >= sizeof(comp)) return -1;
        memcpy(comp, p, complen);
        comp[complen] = 0;

        if (!slash) {
            /* leaf */
            if (complen >= leaf_cap) return -1;
            memcpy(out_leaf, comp, complen + 1);
            *out_parent = cur;
            return 0;
        }

        /* descend into existing child dir */
        dir_node_t *child = cur->first_child;
        while (child && strcmp(child->name, comp) != 0) child = child->next_sibling;
        if (!child) {
            LOG_E("split_path: parent dir %.*s does not exist for %s",
                  (int)complen, comp, path);
            return -1;
        }
        cur = child;
        p = slash + 1;
    }
}

/* ---- Inode allocation ---- */

static inode_rec_t *alloc_inode_rec(ext4_writer_t *w, uint32_t inode)
{
    if (inode >= w->total_inodes + 1) {
        LOG_E("alloc_inode_rec: out of inode space (req=%u max=%u)",
              inode, w->total_inodes);
        return NULL;
    }
    inode_rec_t *r = (inode_rec_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->inode = inode;
    w->inodes[inode] = r;
    w->n_used_inodes++;
    return r;
}

static uint32_t alloc_next_inode(ext4_writer_t *w)
{
    uint32_t n = w->next_free_inode++;
    if (n > w->total_inodes) { LOG_E("inode space exhausted"); return 0; }
    return n;
}

/* ---- Block allocation ----
 *
 * We allocate contiguous runs of data blocks per file. Strategy:
 *   1. Try to satisfy from the current "favourite" group's free space
 *   2. If not enough, move to next group with free space
 *   3. Return the absolute block number of the first block in the run
 *
 * Returns 0 on success. *out_first set to absolute block number. */
static int alloc_data_blocks(ext4_writer_t *w, uint32_t want,
                              uint32_t *out_first)
{
    if (want == 0) { *out_first = 0; return 0; }
    for (uint32_t g = 0; g < w->num_groups; g++) {
        uint32_t group_blocks = (g == w->num_groups - 1) ?
            (w->total_blocks - g * w->blocks_per_group) : w->blocks_per_group;
        uint32_t group_data_end = group_blocks;   /* group-relative */
        uint32_t free_now = group_data_end - w->next_free_block[g];
        if (free_now >= want) {
            uint32_t first_rel = w->next_free_block[g];
            w->next_free_block[g] += want;
            w->blocks_used_in_group[g] += want;
            *out_first = g * w->blocks_per_group + first_rel;
            return 0;
        }
    }
    LOG_D("alloc_data_blocks: cannot satisfy %u blocks (caller will retry smaller)", want);
    return -1;
}

/* ---- Inode body construction ---- */

/* Fill in the standard fields. Caller fills type-specific fields after. */
static void inode_init(ext4_inode_t *inode, uint16_t mode_full,
                       uint32_t uid, uint32_t gid, uint32_t mtime)
{
    memset(inode, 0, sizeof(*inode));
    inode->i_mode = mode_full;
    inode->i_uid  = (uint16_t)(uid & 0xffff);
    inode->i_osd2_uid_hi = (uint16_t)(uid >> 16);
    inode->i_gid  = (uint16_t)(gid & 0xffff);
    inode->i_osd2_gid_hi = (uint16_t)(gid >> 16);
    inode->i_atime = inode->i_mtime = inode->i_ctime = mtime;
    inode->i_links_count = 1;
    inode->i_extra_isize = (uint16_t)(INODE_SIZE - 128);  /* 256 - 128 = 128 */
}

/* Set an extent tree in i_block for a single contiguous run of blocks. */
static void inode_set_single_extent(ext4_inode_t *inode,
                                     uint32_t first_block, uint32_t n_blocks)
{
    ext4_extent_header_t h = { 0 };
    h.eh_magic   = EXT4_EXT_MAGIC;
    h.eh_entries = (n_blocks > 0) ? 1 : 0;
    h.eh_max     = 4;       /* fits in 60 bytes: header(12) + 4*ext(12) = 60 */
    h.eh_depth   = 0;
    h.eh_generation = 0;
    memcpy(inode->i_block, &h, sizeof(h));

    if (n_blocks > 0) {
        ext4_extent_t e = { 0 };
        e.ee_block    = 0;
        e.ee_len      = (uint16_t)n_blocks;
        e.ee_start_lo = first_block;
        e.ee_start_hi = 0;
        memcpy(inode->i_block + sizeof(h), &e, sizeof(e));

        inode->i_flags |= EXT4_EXTENTS_FL;
        /* i_blocks counts in 512-byte units (logical "blocks count"). */
        inode->i_blocks_lo = n_blocks * (BLOCK_SIZE / 512u);
    } else {
        /* Empty extents allowed; just header. */
        inode->i_flags |= EXT4_EXTENTS_FL;
    }
}

/* Forward decls for the writer functions used by inode_set_extents below. */
typedef struct ext4_writer ext4_writer_t_fwd;
static int alloc_data_blocks(struct ext4_writer *w, uint32_t want, uint32_t *out_first);
static int part_write_block(struct ext4_writer *w, uint32_t block_no,
                             const void *buf, size_t blocks);

/* Extents that fit in the inode's 60-byte i_block field: header (12) +
 * up to 4 leaf entries (12 each). For more extents, build a depth-1 tree:
 * one index entry in the inode pointing at a leaf block holding all the
 * actual leaves (up to ~340 in a 4 KiB block). */
#define INODE_DIRECT_EXTENTS  4u
#define LEAFS_PER_BLOCK       ((BLOCK_SIZE - sizeof(ext4_extent_header_t)) / sizeof(ext4_extent_t))

typedef struct { uint32_t first; uint16_t len; uint32_t logical; } ext_run_t;

static void inode_set_extents_direct(ext4_inode_t *inode, const ext_run_t *exts,
                                      int n, uint64_t total_size_512_units)
{
    ext4_extent_header_t h = { 0 };
    h.eh_magic   = EXT4_EXT_MAGIC;
    h.eh_entries = (uint16_t)n;
    h.eh_max     = INODE_DIRECT_EXTENTS;
    h.eh_depth   = 0;
    memcpy(inode->i_block, &h, sizeof(h));
    for (int i = 0; i < n; i++) {
        ext4_extent_t e = { 0 };
        e.ee_block    = exts[i].logical;
        e.ee_len      = exts[i].len;
        e.ee_start_lo = exts[i].first;
        memcpy(inode->i_block + sizeof(h) + i * sizeof(e), &e, sizeof(e));
    }
    inode->i_flags |= EXT4_EXTENTS_FL;
    inode->i_blocks_lo = (uint32_t)total_size_512_units;
}

/* Build a depth-1 extent tree: allocate one "leaf block" on disk, write
 * all N extents into it, set the inode to a single index entry pointing
 * at that leaf block. Adds 1 block to i_blocks_lo for the leaf itself. */
static int inode_set_extents_indirect(struct ext4_writer *w, ext4_inode_t *inode,
                                       const ext_run_t *exts, int n,
                                       uint64_t total_size_512_units)
{
    if ((size_t)n > LEAFS_PER_BLOCK) {
        LOG_E("inode_set_extents_indirect: n=%d exceeds %zu (would need depth=2)",
              n, LEAFS_PER_BLOCK);
        return -1;
    }
    uint32_t leaf_block = 0;
    if (alloc_data_blocks(w, 1, &leaf_block) != 0) return -1;

    /* Build the leaf block: header + N extents, padded with zeros. */
    uint8_t leafbuf[BLOCK_SIZE] = { 0 };
    ext4_extent_header_t lh = { 0 };
    lh.eh_magic   = EXT4_EXT_MAGIC;
    lh.eh_entries = (uint16_t)n;
    lh.eh_max     = (uint16_t)LEAFS_PER_BLOCK;
    lh.eh_depth   = 0;
    memcpy(leafbuf, &lh, sizeof(lh));
    for (int i = 0; i < n; i++) {
        ext4_extent_t e = { 0 };
        e.ee_block    = exts[i].logical;
        e.ee_len      = exts[i].len;
        e.ee_start_lo = exts[i].first;
        memcpy(leafbuf + sizeof(lh) + i * sizeof(e), &e, sizeof(e));
    }
    if (part_write_block(w, leaf_block, leafbuf, 1) != 0) return -1;

    /* Inode header: 1 index entry, depth=1. */
    ext4_extent_header_t ih = { 0 };
    ih.eh_magic   = EXT4_EXT_MAGIC;
    ih.eh_entries = 1;
    ih.eh_max     = INODE_DIRECT_EXTENTS;
    ih.eh_depth   = 1;
    memcpy(inode->i_block, &ih, sizeof(ih));

    ext4_extent_idx_t idx = { 0 };
    idx.ei_block   = 0;
    idx.ei_leaf_lo = leaf_block;
    memcpy(inode->i_block + sizeof(ih), &idx, sizeof(idx));

    inode->i_flags |= EXT4_EXTENTS_FL;
    /* +1 for the leaf block itself (in 512-byte units). */
    inode->i_blocks_lo = (uint32_t)(total_size_512_units + (BLOCK_SIZE / 512u));
    return 0;
}

/* ---- Layout planner + open ---- */

ext4_writer_t *ext4_writer_open(HANDLE phys,
                                uint64_t partition_lba_start,
                                uint64_t partition_lba_count,
                                uint32_t est_inodes)
{
    ext4_writer_t *w = (ext4_writer_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->phys = phys;
    w->part_lba_start = partition_lba_start;
    w->part_lba_count = partition_lba_count;
    w->part_bytes = partition_lba_count * 512ULL;

    w->total_blocks      = (uint32_t)(w->part_bytes / BLOCK_SIZE);
    w->blocks_per_group  = BLOCKS_PER_GROUP;
    w->inodes_per_group  = INODES_PER_GROUP;
    w->num_groups = (w->total_blocks + w->blocks_per_group - 1) / w->blocks_per_group;

    /* Inodes per group: scale UP if est_inodes exceeds current default,
     * never DOWN. Always a multiple of 8 (kernel requirement). */
    if (est_inodes > w->num_groups * w->inodes_per_group) {
        uint32_t per = (est_inodes + w->num_groups - 1) / w->num_groups;
        per = (per + 7) & ~7u;
        /* A single 4 KiB inode bitmap block addresses at most BLOCK_SIZE*8
         * inodes; never let inodes_per_group exceed that or write_inode_bitmap
         * overruns its fixed bm[BLOCK_SIZE] stack buffer. */
        if (per > BLOCK_SIZE * 8) per = BLOCK_SIZE * 8;
        w->inodes_per_group = per;
    }
    w->total_inodes = w->inodes_per_group * w->num_groups;

    w->inode_table_blocks_per_group =
        (w->inodes_per_group * INODE_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;

    w->gdt_blocks = (w->num_groups * (uint32_t)sizeof(ext4_group_desc_t)
                     + BLOCK_SIZE - 1) / BLOCK_SIZE;

    LOG_I("ext4 layout:");
    LOG_I("  partition bytes      : %llu (%.2f GiB)",
          (unsigned long long)w->part_bytes,
          (double)w->part_bytes / (1024.0 * 1024.0 * 1024.0));
    LOG_I("  block size           : %u", BLOCK_SIZE);
    LOG_I("  total blocks         : %u", w->total_blocks);
    LOG_I("  blocks per group     : %u", w->blocks_per_group);
    LOG_I("  num groups           : %u", w->num_groups);
    LOG_I("  inode size           : %u", INODE_SIZE);
    LOG_I("  inodes per group     : %u", w->inodes_per_group);
    LOG_I("  total inodes         : %u", w->total_inodes);
    LOG_I("  inode table blocks/g : %u", w->inode_table_blocks_per_group);
    LOG_I("  GDT blocks           : %u", w->gdt_blocks);

    w->next_free_block = (uint32_t *)calloc(w->num_groups, sizeof(uint32_t));
    w->blocks_used_in_group = (uint32_t *)calloc(w->num_groups, sizeof(uint32_t));
    w->inodes = (inode_rec_t **)calloc((size_t)w->total_inodes + 1, sizeof(inode_rec_t *));
    if (!w->next_free_block || !w->blocks_used_in_group || !w->inodes) {
        free(w->next_free_block); free(w->blocks_used_in_group); free(w->inodes);
        free(w); return NULL;
    }
    for (uint32_t g = 0; g < w->num_groups; g++) {
        w->next_free_block[g] = group_first_data_block(w, g);
    }

    /* Generate a fresh UUID and zero the label. */
    GUID g;
    if (CoCreateGuid(&g) != S_OK) {
        memset(w->uuid, 0, 16);
    } else {
        /* GUID layout matches ext4's UUID layout (16 bytes raw). */
        memcpy(w->uuid, &g, 16);
    }
    memset(w->label, 0, sizeof(w->label));

    /* Allocate SB + GDT scratch buffers via VirtualAlloc (page-aligned).
     * The disk handle uses FILE_FLAG_NO_BUFFERING which requires
     * sector-aligned buffer pointers - malloc has no such guarantee. */
    w->sb_buf  = (uint8_t *)VirtualAlloc(NULL, BLOCK_SIZE,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    w->gdt_buf = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)w->gdt_blocks * BLOCK_SIZE,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    w->coal_buf_cap = 16ULL * 1024ULL * 1024ULL;   /* 16 MiB */
    w->coal_buf = (uint8_t *)VirtualAlloc(NULL, w->coal_buf_cap,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!w->sb_buf || !w->gdt_buf || !w->coal_buf) {
        if (w->sb_buf)   VirtualFree(w->sb_buf,   0, MEM_RELEASE);
        if (w->gdt_buf)  VirtualFree(w->gdt_buf,  0, MEM_RELEASE);
        if (w->coal_buf) VirtualFree(w->coal_buf, 0, MEM_RELEASE);
        free(w->next_free_block); free(w->blocks_used_in_group);
        free(w->inodes); free(w); return NULL;
    }
    memset(w->sb_buf, 0, BLOCK_SIZE);
    memset(w->gdt_buf, 0, (size_t)w->gdt_blocks * BLOCK_SIZE);

    /* Inode allocator: 1..10 reserved, lost+found = 11, app inodes start 12. */
    w->next_free_inode = EXT4_FIRST_INO + 1;

    /* Build root dir node + reserve root inode + lost+found. */
    w->root = (dir_node_t *)calloc(1, sizeof(dir_node_t));
    w->root->inode = EXT4_ROOT_INO;
    inode_rec_t *root_rec = alloc_inode_rec(w, EXT4_ROOT_INO);
    inode_init(&root_rec->body, S_IFDIR | 0755, 0, 0, (uint32_t)time(NULL));
    root_rec->body.i_links_count = 2;  /* . + .. from itself */

    /* Create lost+found dir for fsck friendliness. */
    dir_node_t *lf = (dir_node_t *)calloc(1, sizeof(dir_node_t));
    lf->inode = EXT4_LOST_FOUND_INO;
    strcpy(lf->name, "lost+found");
    lf->parent = w->root;
    lf->next_sibling = w->root->first_child;
    w->root->first_child = lf;
    inode_rec_t *lf_rec = alloc_inode_rec(w, EXT4_LOST_FOUND_INO);
    inode_init(&lf_rec->body, S_IFDIR | 0700, 0, 0, (uint32_t)time(NULL));
    lf_rec->body.i_links_count = 2;
    /* Add to root's entries list. */
    dir_entry_rec_t *de = (dir_entry_rec_t *)calloc(1, sizeof(*de));
    strcpy(de->name, "lost+found");
    de->name_len = (uint8_t)strlen(de->name);
    de->file_type = EXT4_FT_DIR;
    de->inode = EXT4_LOST_FOUND_INO;
    w->root->entries = de;
    w->root->entries_tail = de;
    root_rec->body.i_links_count++; /* link from lost+found's .. */

    return w;
}

void ext4_writer_set_label(ext4_writer_t *w, const char *label)
{
    if (!label) { memset(w->label, 0, sizeof(w->label)); return; }
    strncpy(w->label, label, sizeof(w->label) - 1);
    w->label[sizeof(w->label) - 1] = 0;
}

void ext4_writer_get_io_stats(const ext4_writer_t *w,
                              uint64_t *out_bytes_in,
                              uint64_t *out_bytes_flushed,
                              uint64_t *out_flush_ticks,
                              uint64_t *out_flush_calls)
{
    if (out_bytes_in)      *out_bytes_in      = w->stat_bytes_in;
    if (out_bytes_flushed) *out_bytes_flushed = w->stat_bytes_flushed;
    if (out_flush_ticks)   *out_flush_ticks   = w->stat_flush_ticks;
    if (out_flush_calls)   *out_flush_calls   = w->stat_flush_calls;
}

void ext4_writer_get_uuid_text(const ext4_writer_t *w, char *out, size_t cap)
{
    if (cap < 37) { if (cap) out[0] = 0; return; }
    const uint8_t *u = w->uuid;
    /* Big-endian text form. ext4 stores raw bytes; canonical text is
     * big-endian per RFC 4122. */
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        u[0],u[1],u[2],u[3], u[4],u[5], u[6],u[7],
        u[8],u[9], u[10],u[11],u[12],u[13],u[14],u[15]);
}

/* ---- add_dir / add_file / add_symlink / add_special ---- */

static int add_dir_entry(dir_node_t *parent, const char *leaf, size_t leaf_len,
                         uint32_t child_inode, uint8_t file_type)
{
    if (leaf_len >= 256) return -1;
    dir_entry_rec_t *de = (dir_entry_rec_t *)calloc(1, sizeof(*de));
    if (!de) return -1;
    memcpy(de->name, leaf, leaf_len);
    de->name_len  = (uint8_t)leaf_len;
    de->file_type = file_type;
    de->inode     = child_inode;
    if (!parent->entries) parent->entries = de;
    else parent->entries_tail->next = de;
    parent->entries_tail = de;
    return 0;
}

int ext4_writer_add_dir(ext4_writer_t *w, const char *path,
                        uint16_t perms, uint32_t uid, uint32_t gid,
                        uint32_t mtime)
{
    dir_node_t *parent;
    char leaf[256];
    if (split_path(w, path, &parent, leaf, sizeof(leaf)) != 0) return -1;

    /* Idempotent: if a child directory with this name already exists, no-op.
     * Callers like the boot-file staging want to "ensure /boot exists" even
     * if it was already created during squashfs ingest. */
    for (dir_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (strcmp(c->name, leaf) == 0) {
            return 0;
        }
    }

    uint32_t ino = alloc_next_inode(w);
    if (!ino) return -1;
    inode_rec_t *r = alloc_inode_rec(w, ino);
    if (!r) return -1;
    inode_init(&r->body, S_IFDIR | (perms & 07777), uid, gid, mtime);
    r->body.i_links_count = 2;   /* . + .. from self */

    dir_node_t *child = (dir_node_t *)calloc(1, sizeof(*child));
    if (!child) return -1;
    strncpy(child->name, leaf, sizeof(child->name) - 1);
    child->inode = ino;
    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;

    if (add_dir_entry(parent, leaf, strlen(leaf), ino, EXT4_FT_DIR) != 0) return -1;

    /* parent gains a link from this child's ".." */
    w->inodes[parent->inode]->body.i_links_count++;
    return 0;
}

static int write_file_extents(ext4_writer_t *w, inode_rec_t *r,
                              const void *data, uint64_t size)
{
    if (size == 0) {
        inode_set_single_extent(&r->body, 0, 0);
        r->body.i_size_lo = 0;
        r->body.i_size_hi = 0;
        return 0;
    }
    uint64_t blocks_needed = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* Allocate enough extents. Up to INODE_DIRECT_EXTENTS (4) fit directly
     * in the inode; more spill into a depth-1 extent tree (one leaf block
     * holding up to LEAFS_PER_BLOCK entries). For files larger than what
     * one leaf block can address (~340 extents * 128 MB = ~42 GB), we'd
     * need depth-2 - not supported in this POC. */
    uint32_t first = 0;
    size_t runs_cap = LEAFS_PER_BLOCK;
    ext_run_t *runs = (ext_run_t *)calloc(runs_cap, sizeof(ext_run_t));
    if (!runs) return -1;
    int n_runs = 0;
    uint64_t remaining = blocks_needed;
    uint32_t logical = 0;
    while (remaining > 0) {
        if ((size_t)n_runs >= runs_cap) {
            LOG_E("ext4_writer: file at inode %u needs > %zu extents (depth-2 not supported)",
                  r->inode, runs_cap);
            free(runs); return -1;
        }
        uint32_t got_first = 0;
        uint32_t try_n = (remaining > 32768u) ? 32768u : (uint32_t)remaining;
        int ok = -1;
        while (try_n > 0) {
            if (alloc_data_blocks(w, try_n, &got_first) == 0) { ok = 0; break; }
            try_n /= 2;
        }
        if (ok != 0 || try_n == 0) {
            LOG_E("ext4_writer: out of space allocating %llu blocks (inode %u)",
                  (unsigned long long)blocks_needed, r->inode);
            free(runs); return -1;
        }
        runs[n_runs].first   = got_first;
        runs[n_runs].len     = (uint16_t)try_n;
        runs[n_runs].logical = logical;
        n_runs++;
        first = (n_runs == 1) ? got_first : first;
        logical += try_n;
        remaining -= try_n;
    }

    /* Write data immediately, chunked along the runs. */
    const uint8_t *p = (const uint8_t *)data;
    uint64_t left = size;
    for (int i = 0; i < n_runs; i++) {
        uint64_t run_bytes_avail = (uint64_t)runs[i].len * BLOCK_SIZE;
        uint64_t this_write = run_bytes_avail < left ? run_bytes_avail : left;

        size_t full_blocks = (size_t)(this_write / BLOCK_SIZE);
        size_t tail        = (size_t)(this_write % BLOCK_SIZE);

        if (full_blocks > 0) {
            if (part_write_block(w, runs[i].first, p, full_blocks) != 0) {
                free(runs); return -1;
            }
        }
        if (tail > 0) {
            uint8_t pad[BLOCK_SIZE] = { 0 };
            memcpy(pad, p + full_blocks * BLOCK_SIZE, tail);
            uint32_t tail_block = runs[i].first + (uint32_t)full_blocks;
            if (part_write_block(w, tail_block, pad, 1) != 0) {
                free(runs); return -1;
            }
        }
        p += this_write;
        left -= this_write;
    }

    uint64_t blocks_512_units = blocks_needed * (BLOCK_SIZE / 512u);
    if (n_runs == 1) {
        inode_set_single_extent(&r->body, runs[0].first, runs[0].len);
        r->data_first_block = runs[0].first;
        r->data_block_count = runs[0].len;
    } else if ((uint32_t)n_runs <= INODE_DIRECT_EXTENTS) {
        inode_set_extents_direct(&r->body, runs, n_runs, blocks_512_units);
    } else {
        if (inode_set_extents_indirect(w, &r->body, runs, n_runs, blocks_512_units) != 0) {
            free(runs); return -1;
        }
    }
    r->body.i_size_lo = (uint32_t)(size & 0xffffffffu);
    r->body.i_size_hi = (uint32_t)(size >> 32);
    free(runs);
    return 0;
}

static void journal_put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int write_journal(ext4_writer_t *w)
{
    size_t size = (size_t)EXT4_JOURNAL_BLOCKS * BLOCK_SIZE;
    uint8_t *data = (uint8_t *)calloc(1, size);
    inode_rec_t *r;
    int result;

    if (!data) return -1;
    r = alloc_inode_rec(w, EXT4_JOURNAL_INO);
    if (!r) { free(data); return -1; }
    inode_init(&r->body, S_IFREG | 0600, 0, 0, (uint32_t)time(NULL));
    journal_put_be32(data + 0x00, 0xC03B3998u);
    journal_put_be32(data + 0x04, 4);
    journal_put_be32(data + 0x0C, BLOCK_SIZE);
    journal_put_be32(data + 0x10, EXT4_JOURNAL_BLOCKS);
    journal_put_be32(data + 0x14, 1);
    journal_put_be32(data + 0x18, 1);
    journal_put_be32(data + 0x28, 1);
    memcpy(data + 0x30, w->uuid, sizeof(w->uuid));
    journal_put_be32(data + 0x40, 1);
    result = write_file_extents(w, r, data, size);
    free(data);
    return result;
}

int ext4_writer_add_file(ext4_writer_t *w, const char *path,
                         uint16_t perms, uint32_t uid, uint32_t gid,
                         uint32_t mtime, const void *data, uint64_t size)
{
    dir_node_t *parent;
    char leaf[256];
    if (split_path(w, path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    uint32_t ino = alloc_next_inode(w);
    if (!ino) return -1;
    inode_rec_t *r = alloc_inode_rec(w, ino);
    if (!r) return -1;
    inode_init(&r->body, S_IFREG | (perms & 07777), uid, gid, mtime);
    if (write_file_extents(w, r, data, size) != 0) return -1;
    if (add_dir_entry(parent, leaf, strlen(leaf), ino, EXT4_FT_REG_FILE) != 0) return -1;
    return 0;
}

int ext4_writer_set_capability(ext4_writer_t *w, const char *path,
                               const void *data, size_t size)
{
    if (!data || (size != 12 && size != 20 && size != 24)) return -1;
    dir_node_t *parent;
    char leaf[256];
    if (split_path(w, path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    for (dir_entry_rec_t *e = parent->entries; e; e = e->next) {
        if (strcmp(e->name, leaf) != 0) continue;
        if (e->file_type != EXT4_FT_REG_FILE) return -1;
        inode_rec_t *r = w->inodes[e->inode];
        void *copy = malloc(size);
        if (!copy) return -1;
        memcpy(copy, data, size);
        free(r->capability);
        r->capability = copy;
        r->capability_size = (uint32_t)size;
        r->body.i_extra_isize = (uint16_t)(sizeof(ext4_inode_t) - 128);
        return 0;
    }
    return -1;
}

int ext4_writer_add_symlink(ext4_writer_t *w, const char *path,
                            const char *target, size_t target_len,
                            uint32_t uid, uint32_t gid, uint32_t mtime)
{
    dir_node_t *parent;
    char leaf[256];
    if (split_path(w, path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    uint32_t ino = alloc_next_inode(w);
    if (!ino) return -1;
    inode_rec_t *r = alloc_inode_rec(w, ino);
    if (!r) return -1;
    inode_init(&r->body, S_IFLNK | 0777, uid, gid, mtime);

    if (target_len < 60) {
        /* Fast symlink: target inline in i_block. The cutoff is < 60 (not
         * <= 60) because e2fsck requires strnlen(target, 60) != 60 - i.e.
         * the inline region must contain at least one trailing NUL byte.
         * The kernel doesn't care, but we want a clean fsck. */
        memcpy(r->body.i_block, target, target_len);
        r->body.i_size_lo = (uint32_t)target_len;
        r->body.i_blocks_lo = 0;
        /* Important: do NOT set EXTENTS flag for inline symlinks. */
    } else {
        /* Slow symlink: target in a data block. */
        uint8_t buf[BLOCK_SIZE] = { 0 };
        if (target_len > BLOCK_SIZE) {
            LOG_E("symlink target too long: %zu bytes", target_len);
            return -1;
        }
        memcpy(buf, target, target_len);
        uint32_t first;
        if (alloc_data_blocks(w, 1, &first) != 0) return -1;
        if (part_write_block(w, first, buf, 1) != 0) return -1;
        inode_set_single_extent(&r->body, first, 1);
        r->body.i_size_lo = (uint32_t)target_len;
    }
    if (add_dir_entry(parent, leaf, strlen(leaf), ino, EXT4_FT_SYMLINK) != 0) return -1;
    return 0;
}

int ext4_writer_add_special(ext4_writer_t *w, const char *path,
                            uint16_t mode_with_type,
                            uint32_t uid, uint32_t gid, uint32_t mtime,
                            uint32_t rdev)
{
    dir_node_t *parent;
    char leaf[256];
    if (split_path(w, path, &parent, leaf, sizeof(leaf)) != 0) return -1;
    uint32_t ino = alloc_next_inode(w);
    if (!ino) return -1;
    inode_rec_t *r = alloc_inode_rec(w, ino);
    if (!r) return -1;
    inode_init(&r->body, mode_with_type, uid, gid, mtime);

    /* For block/char devices, rdev goes into i_block[0] (legacy major/minor
     * format) or i_block[1] (new). We use legacy. */
    uint16_t typ = mode_with_type & S_IFMT;
    uint8_t ft = EXT4_FT_UNKNOWN;
    if (typ == S_IFBLK)        ft = EXT4_FT_BLKDEV;
    else if (typ == S_IFCHR)   ft = EXT4_FT_CHRDEV;
    else if (typ == S_IFIFO)   ft = EXT4_FT_FIFO;
    else if (typ == S_IFSOCK)  ft = EXT4_FT_SOCK;

    if (typ == S_IFBLK || typ == S_IFCHR) {
        memcpy(r->body.i_block, &rdev, sizeof(uint32_t));
    }
    if (add_dir_entry(parent, leaf, strlen(leaf), ino, ft) != 0) return -1;
    return 0;
}

/* ---- Close: write everything ---- */

/* State for ext4 dir-entry serialization. */
typedef struct {
    uint8_t          *buf;
    size_t            cap;
    size_t            pos;
    size_t            cur_block_start;
    ext4_dir_entry_t *last_in_block;
} ser_ctx_t;

static int ser_emit(ser_ctx_t *s, uint32_t inode_no, uint8_t file_type,
                    const char *name, size_t name_len)
{
    size_t rec = 8 + ((name_len + 3) & ~(size_t)3);
    if (rec > BLOCK_SIZE) return -1;
    size_t block_off = s->pos - s->cur_block_start;

    /* If this entry would cross the next block boundary, advance to the
     * boundary and extend the last entry's rec_len to fill the gap. */
    if (block_off + rec > BLOCK_SIZE) {
        if (s->last_in_block) {
            size_t fill_to = s->cur_block_start + BLOCK_SIZE;
            s->last_in_block->rec_len =
                (uint16_t)(fill_to - ((uint8_t *)s->last_in_block - s->buf));
        }
        s->cur_block_start += BLOCK_SIZE;
        s->pos = s->cur_block_start;
    }

    /* Grow buffer if needed. */
    if (s->pos + rec > s->cap) {
        size_t newcap = s->cap == 0 ? BLOCK_SIZE : s->cap * 2;
        while (newcap < s->pos + rec) newcap *= 2;
        uint8_t *nb = (uint8_t *)realloc(s->buf, newcap);
        if (!nb) return -1;
        memset(nb + s->cap, 0, newcap - s->cap);
        s->buf = nb;
        s->cap = newcap;
    }

    ext4_dir_entry_t *de = (ext4_dir_entry_t *)(s->buf + s->pos);
    de->inode     = inode_no;
    de->rec_len   = (uint16_t)rec;
    de->name_len  = (uint8_t)name_len;
    de->file_type = file_type;
    memcpy(s->buf + s->pos + 8, name, name_len);
    s->last_in_block = de;
    s->pos += rec;
    return 0;
}

static int serialize_dir(ext4_writer_t *w, dir_node_t *d, uint32_t parent_ino,
                          uint8_t **out_buf, uint32_t *out_len)
{
    (void)w;
    ser_ctx_t s = { 0 };
    if (ser_emit(&s, d->inode, EXT4_FT_DIR, ".",  1) != 0) { free(s.buf); return -1; }
    if (ser_emit(&s, parent_ino, EXT4_FT_DIR, "..", 2) != 0) { free(s.buf); return -1; }
    for (dir_entry_rec_t *e = d->entries; e; e = e->next) {
        if (ser_emit(&s, e->inode, e->file_type, e->name, e->name_len) != 0) {
            free(s.buf); return -1;
        }
    }
    /* Extend final entry in last block to reach block boundary, and pad
     * total length up to a multiple of BLOCK_SIZE. */
    if (s.last_in_block) {
        size_t fill_to = s.cur_block_start + BLOCK_SIZE;
        s.last_in_block->rec_len =
            (uint16_t)(fill_to - ((uint8_t *)s.last_in_block - s.buf));
    }
    size_t padded = (s.pos + BLOCK_SIZE - 1) & ~(size_t)(BLOCK_SIZE - 1);
    if (padded == 0) padded = BLOCK_SIZE;
    if (padded > s.cap) {
        uint8_t *nb = (uint8_t *)realloc(s.buf, padded);
        if (!nb) { free(s.buf); return -1; }
        s.buf = nb;
    }
    memset(s.buf + s.pos, 0, padded - s.pos);

    *out_buf = s.buf;
    *out_len = (uint32_t)padded;
    return 0;
}

static int finalize_dirs(ext4_writer_t *w);
static int finalize_dirs_recur(ext4_writer_t *w, dir_node_t *d)
{
    /* First serialize my entries, allocate blocks, write inode body. */
    uint8_t *blob = NULL;
    uint32_t blob_len = 0;
    uint32_t parent_ino = d->parent ? d->parent->inode : d->inode;  /* root: .. = root */
    if (serialize_dir(w, d, parent_ino, &blob, &blob_len) != 0) return -1;

    uint32_t n_blocks = blob_len / BLOCK_SIZE;
    uint32_t first = 0;
    if (alloc_data_blocks(w, n_blocks, &first) != 0) { free(blob); return -1; }
    if (part_write_block(w, first, blob, n_blocks) != 0) { free(blob); return -1; }

    inode_rec_t *r = w->inodes[d->inode];
    r->body.i_size_lo = blob_len;
    inode_set_single_extent(&r->body, first, n_blocks);

    free(blob);

    /* Recurse into children. */
    for (dir_node_t *c = d->first_child; c; c = c->next_sibling) {
        if (finalize_dirs_recur(w, c) != 0) return -1;
    }
    return 0;
}
static int finalize_dirs(ext4_writer_t *w)
{
    return finalize_dirs_recur(w, w->root);
}

/* Write all inode tables. For each group, build a contiguous buffer of
 * inodes_per_group * INODE_SIZE bytes in memory, fill in each used inode's
 * slot, and write the whole table in one shot. */
static int write_all_inodes(ext4_writer_t *w)
{
    LOG_I("writing %u inode bodies across %u groups", w->n_used_inodes, w->num_groups);
    /* The table is written by whole 4 KiB blocks (inode_table_blocks_per_group),
     * so the buffer must be block-aligned: inodes_per_group*INODE_SIZE alone is
     * smaller whenever inodes_per_group is not a multiple of 16, which would make
     * part_write_block read past tbl. The per-group memset zeroes the padding. */
    size_t table_bytes = (size_t)w->inode_table_blocks_per_group * BLOCK_SIZE;
    uint8_t *tbl = (uint8_t *)calloc(1, table_bytes);
    if (!tbl) return -1;

    for (uint32_t g = 0; g < w->num_groups; g++) {
        memset(tbl, 0, table_bytes);
        uint32_t any = 0;
        for (uint32_t idx = 0; idx < w->inodes_per_group; idx++) {
            uint32_t inode_num = g * w->inodes_per_group + idx + 1;
            if (inode_num > w->total_inodes) break;
            if (!w->inodes[inode_num]) continue;
            memcpy(tbl + idx * INODE_SIZE,
                   &w->inodes[inode_num]->body,
                   sizeof(ext4_inode_t));
            inode_rec_t *r = w->inodes[inode_num];
            if (r->capability_size) {
                uint8_t *inode = tbl + idx * INODE_SIZE;
                uint32_t magic = 0xEA020000u;
                memcpy(inode + sizeof(ext4_inode_t), &magic, sizeof(magic));
                ext4_xattr_entry_t *entry = (ext4_xattr_entry_t *)(
                    inode + sizeof(ext4_inode_t) + sizeof(magic));
                entry->e_name_len = 10;
                entry->e_name_index = 6;
                entry->e_value_offs = (uint16_t)(INODE_SIZE - r->capability_size -
                    sizeof(ext4_inode_t) - sizeof(magic));
                entry->e_value_size = r->capability_size;
                memcpy(entry + 1, "capability", entry->e_name_len);
                memcpy(inode + INODE_SIZE - r->capability_size,
                       r->capability, r->capability_size);
            }
            any = 1;
        }
        if (!any) continue;

        uint32_t itable_first_block = g * w->blocks_per_group +
            (is_backup_group(g) ? (1 + w->gdt_blocks) : 0) + 2;
        if (part_write_block(w, itable_first_block, tbl,
                             w->inode_table_blocks_per_group) != 0) {
            LOG_E("inode table write failed for group %u (block %u)",
                  g, itable_first_block);
            free(tbl);
            return -1;
        }
    }
    free(tbl);
    return 0;
}

/* Build & write the block bitmap for one group. */
static int write_block_bitmap(ext4_writer_t *w, uint32_t g)
{
    uint8_t bm[BLOCK_SIZE];
    memset(bm, 0, BLOCK_SIZE);
    /* Mark all blocks 0..first_data_block-1 used (the metadata),
     * plus all data blocks we allocated (first_data..first_data+blocks_used-1). */
    uint32_t group_blocks = (g == w->num_groups - 1) ?
        (w->total_blocks - g * w->blocks_per_group) : w->blocks_per_group;
    uint32_t fdb = group_first_data_block(w, g);
    uint32_t used_up_to = fdb + w->blocks_used_in_group[g];
    /* Bits 0..used_up_to-1 set; bits >= group_blocks set (past end). */
    for (uint32_t b = 0; b < used_up_to; b++)
        bm[b >> 3] |= (uint8_t)(1u << (b & 7));
    for (uint32_t b = group_blocks; b < BLOCKS_PER_GROUP; b++)
        bm[b >> 3] |= (uint8_t)(1u << (b & 7));

    uint32_t bb_block = g * w->blocks_per_group +
        (is_backup_group(g) ? (1 + w->gdt_blocks) : 0);
    return part_write_block(w, bb_block, bm, 1);
}

/* Build & write the inode bitmap for one group. */
static int write_inode_bitmap(ext4_writer_t *w, uint32_t g)
{
    uint8_t bm[BLOCK_SIZE];
    memset(bm, 0, BLOCK_SIZE);
    uint32_t inode_lo = g * w->inodes_per_group + 1;
    uint32_t inode_hi = inode_lo + w->inodes_per_group - 1;
    for (uint32_t i = inode_lo; i <= inode_hi; i++) {
        if (w->inodes[i]) {
            uint32_t b = i - inode_lo;
            bm[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
    }
    /* Reserve inodes 1..10 in group 0. */
    if (g == 0) {
        for (uint32_t i = 1; i <= 10; i++) {
            uint32_t b = i - 1;
            bm[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
    }
    /* Padding: bits at index >= inodes_per_group must be set (those slots
     * don't exist). e2fsck checks this strictly. */
    for (uint32_t b = w->inodes_per_group; b < BLOCK_SIZE * 8; b++) {
        bm[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    uint32_t ib_block = g * w->blocks_per_group +
        (is_backup_group(g) ? (1 + w->gdt_blocks) : 0) + 1;
    return part_write_block(w, ib_block, bm, 1);
}

/* Build the superblock + GDT in our scratch buffers, write to all backup
 * groups + group 0. */
static int write_superblocks_and_gdt(ext4_writer_t *w)
{
    /* Compute free counts. Must match the per-group sums in the GDT. */
    uint64_t free_blocks = 0;
    for (uint32_t g = 0; g < w->num_groups; g++) {
        uint32_t group_blocks = (g == w->num_groups - 1) ?
            (w->total_blocks - g * w->blocks_per_group) : w->blocks_per_group;
        uint32_t used = group_first_data_block(w, g) + w->blocks_used_in_group[g];
        free_blocks += (group_blocks - used);
    }
    /* Reserved inodes 1..10 are always considered used by ext4 conventions;
     * root (inode 2) is in that range so we don't double-count. */
    uint32_t allocated_above_reserved = 0;
    for (uint32_t i = 11; i <= w->total_inodes; i++) {
        if (w->inodes[i]) allocated_above_reserved++;
    }
    uint32_t free_inodes = w->total_inodes - 10 - allocated_above_reserved;

    ext4_superblock_t *sb = (ext4_superblock_t *)(w->sb_buf);
    memset(sb, 0, sizeof(*sb));
    sb->s_inodes_count       = w->total_inodes;
    sb->s_blocks_count_lo    = w->total_blocks;
    sb->s_r_blocks_count_lo  = 0;
    sb->s_free_blocks_count_lo = (uint32_t)free_blocks;
    sb->s_free_inodes_count  = free_inodes;
    sb->s_first_data_block   = 0;       /* with 4K blocks, SB itself lives in block 0 */
    sb->s_log_block_size     = 2;       /* 1024 << 2 = 4096 */
    sb->s_log_cluster_size   = 2;
    sb->s_blocks_per_group   = w->blocks_per_group;
    sb->s_clusters_per_group = w->blocks_per_group;
    sb->s_inodes_per_group   = w->inodes_per_group;
    sb->s_mtime              = 0;
    sb->s_wtime              = (uint32_t)time(NULL);
    sb->s_mnt_count          = 0;
    sb->s_max_mnt_count      = (uint16_t)-1;
    sb->s_magic              = EXT4_SUPER_MAGIC;
    sb->s_state              = 1;       /* clean */
    sb->s_errors             = 1;       /* continue */
    sb->s_minor_rev_level    = 0;
    sb->s_lastcheck          = (uint32_t)time(NULL);
    sb->s_checkinterval      = 0;
    sb->s_creator_os         = 0;       /* linux */
    sb->s_rev_level          = 1;       /* dynamic */
    sb->s_def_resuid         = 0;
    sb->s_def_resgid         = 0;
    sb->s_first_ino          = EXT4_FIRST_INO;
    sb->s_inode_size         = INODE_SIZE;
    sb->s_block_group_nr     = 0;
    sb->s_feature_compat     = EXT4_FEATURE_COMPAT;
    sb->s_feature_incompat   = EXT4_FEATURE_INCOMPAT;
    sb->s_feature_ro_compat  = EXT4_FEATURE_RO_COMPAT;
    sb->s_journal_inum       = EXT4_JOURNAL_INO;
    sb->s_jnl_backup_type    = 1;
    memcpy(sb->s_jnl_blocks, w->inodes[EXT4_JOURNAL_INO]->body.i_block, 60);
    sb->s_jnl_blocks[15] = w->inodes[EXT4_JOURNAL_INO]->body.i_size_hi;
    sb->s_jnl_blocks[16] = w->inodes[EXT4_JOURNAL_INO]->body.i_size_lo;
    memcpy(sb->s_uuid, w->uuid, 16);
    strncpy(sb->s_volume_name, w->label, sizeof(sb->s_volume_name));
    sb->s_desc_size          = 32;  /* not 64bit */
    sb->s_default_mount_opts = 0;
    sb->s_first_meta_bg      = 0;
    sb->s_mkfs_time          = (uint32_t)time(NULL);
    sb->s_min_extra_isize    = 32;
    sb->s_want_extra_isize   = 32;
    sb->s_flags              = 0;
    sb->s_log_groups_per_flex = 0;
    sb->s_checksum_type      = 0;  /* no metadata_csum */

    /* GDT. */
    memset(w->gdt_buf, 0, (size_t)w->gdt_blocks * BLOCK_SIZE);
    ext4_group_desc_t *gd = (ext4_group_desc_t *)w->gdt_buf;
    for (uint32_t g = 0; g < w->num_groups; g++) {
        uint32_t group_blocks = (g == w->num_groups - 1) ?
            (w->total_blocks - g * w->blocks_per_group) : w->blocks_per_group;
        uint32_t base = g * w->blocks_per_group +
            (is_backup_group(g) ? (1 + w->gdt_blocks) : 0);
        gd[g].bg_block_bitmap_lo = base;
        gd[g].bg_inode_bitmap_lo = base + 1;
        gd[g].bg_inode_table_lo  = base + 2;
        uint32_t used = group_first_data_block(w, g) + w->blocks_used_in_group[g];
        gd[g].bg_free_blocks_count_lo = (uint16_t)(group_blocks - used);

        /* Used inodes per group. For group 0, the reserved inodes 1..10 are
         * always considered used (including root = inode 2, which falls
         * within that range). For all groups, count allocated inodes whose
         * number isn't already in the reserved range. */
        uint32_t inode_lo = g * w->inodes_per_group + 1;
        uint32_t used_inodes = 0;
        uint32_t dirs = 0;
        for (uint32_t i = inode_lo; i < inode_lo + w->inodes_per_group; i++) {
            if (i > w->total_inodes) break;
            if (g == 0 && i <= 10) continue;     /* counted below */
            if (!w->inodes[i]) continue;
            used_inodes++;
            if ((w->inodes[i]->body.i_mode & S_IFMT) == S_IFDIR) dirs++;
        }
        if (g == 0) {
            used_inodes += 10;
            /* root inode (2) is a directory, counted in reserved range */
            dirs++;
        }
        gd[g].bg_free_inodes_count_lo = (uint16_t)(w->inodes_per_group - used_inodes);
        gd[g].bg_used_dirs_count_lo   = (uint16_t)dirs;
        /* bg_itable_unused only meaningful with UNINIT_BG/GDT_CSUM features
         * (which we don't enable). Setting it nonzero confuses e2fsck. */
        gd[g].bg_itable_unused_lo = 0;
        gd[g].bg_flags = 0;
    }

    /* Write to group 0 (block 0 of partition holds SB at offset 1024). */
    uint8_t block0[BLOCK_SIZE];
    memset(block0, 0, BLOCK_SIZE);
    memcpy(block0 + 1024, sb, 1024);
    if (part_write_block(w, 0, block0, 1) != 0) return -1;
    /* GDT immediately follows in block 1+. */
    if (part_write_block(w, 1, w->gdt_buf, w->gdt_blocks) != 0) return -1;

    /* Backup SB + GDT in each backup group (1, 3, 5, 7, 9, ...). */
    for (uint32_t g = 1; g < w->num_groups; g++) {
        if (!is_backup_group(g)) continue;
        sb->s_block_group_nr = (uint16_t)g;
        memset(block0, 0, BLOCK_SIZE);
        /* For backup groups the SB is at block 0 of group, no leading 1024B
         * padding. */
        memcpy(block0, sb, 1024);
        uint32_t gb0 = g * w->blocks_per_group;
        if (part_write_block(w, gb0, block0, 1) != 0) return -1;
        if (part_write_block(w, gb0 + 1, w->gdt_buf, w->gdt_blocks) != 0) return -1;
    }
    sb->s_block_group_nr = 0;
    uint32_t backups = 0;
    for (uint32_t g = 1; g < w->num_groups; g++) if (is_backup_group(g)) backups++;
    LOG_I("wrote superblock + GDT to group 0 and %u backup groups", backups);

    return 0;
}

int ext4_writer_close(ext4_writer_t *w)
{
    LOG_I("ext4_writer_close: finalizing %u inodes", w->n_used_inodes);

    if (write_journal(w) != 0) { LOG_E("write_journal failed"); return -1; }

    /* 1. Serialize all directory data + allocate their blocks + set inode extents. */
    if (finalize_dirs(w) != 0) { LOG_E("finalize_dirs failed"); return -1; }

    /* 2. Write all inode bodies. */
    if (write_all_inodes(w) != 0) return -1;

    /* 3. Write block + inode bitmaps for every group. */
    for (uint32_t g = 0; g < w->num_groups; g++) {
        if (write_block_bitmap(w, g) != 0) {
            LOG_E("write_block_bitmap(g=%u) failed", g); return -1;
        }
        if (write_inode_bitmap(w, g) != 0) {
            LOG_E("write_inode_bitmap(g=%u) failed", g); return -1;
        }
    }
    LOG_I("wrote bitmaps for %u groups", w->num_groups);

    /* 4. Write superblock + GDT (including backups). */
    if (write_superblocks_and_gdt(w) != 0) return -1;

    /* 5. Flush any pending coalesced writes before closing. */
    if (part_flush(w) != 0) {
        LOG_E("final part_flush failed");
        return -1;
    }

    /* 6. Cleanup. */
    VirtualFree(w->sb_buf, 0, MEM_RELEASE);
    VirtualFree(w->gdt_buf, 0, MEM_RELEASE);
    VirtualFree(w->coal_buf, 0, MEM_RELEASE);
    free(w->next_free_block); free(w->blocks_used_in_group);
    for (uint32_t i = 1; i <= w->total_inodes; i++) {
        if (w->inodes[i]) {
            free(w->inodes[i]->dir_blob);
            free(w->inodes[i]->capability);
            free(w->inodes[i]);
        }
    }
    free(w->inodes);

    /* Free dir tree. */
    /* (Iterative free skipped for brevity - leak on close, process exits anyway.) */

    LOG_I("ext4_writer_close OK");
    free(w);
    return 0;
}
