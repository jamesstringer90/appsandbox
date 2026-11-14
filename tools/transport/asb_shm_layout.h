/* asb_shm_layout.h — authoritative ivshmem region layout + directory publisher.
 *
 * The ivshmem directory (region table) is owned by the VM LAUNCHER and written into the
 * memory-backend-file BEFORE the guest boots, so every guest service's asb_transport_init()
 * (agent, VDD, input, audio, clipboard — each a separate process) sees ASB_SHM_DIR_MAGIC at
 * start and selects the ivshmem backend. Consumers (the macOS viewer / the host transport)
 * read this layout and attach to slots; they do not write the directory.
 *
 * Shared by:
 *   - asb_shm_publish.c        (the dev-harness pre-boot publisher CLI)
 *   - tools/transport/test/asb_viewer.m  (consumer; publishes only as a standalone fallback)
 *   - the production QemuVm launcher in src/backend_mac (future)
 *
 * The slot/ring math here MUST match asb_transport.c's slot_ptrs(): a slot is
 * [ASB_SLOT_HDR][AsbRing g2h + g2h.cap data][AsbRing h2g + h2g.cap data].
 */
#ifndef ASB_SHM_LAYOUT_H
#define ASB_SHM_LAYOUT_H

#include <stdint.h>
#include <string.h>
#include "asb_transport.h"
#include "asb_automation.h"

#define ASB_RING_HDR   ((uint32_t)sizeof(AsbRing))
#define ASB_SHM_BAR_SIZE (128ull * 1024 * 1024)

/* ---- Region offsets/sizes/ring-caps (from BAR base). The directory occupies the first
 * 4 KiB page; regions start at 0x10000. Sizes leave headroom over each region's slot stride. */
#define ASB_FRAME_REGION_OFF    0x10000ull
#define ASB_FRAME_REGION_SIZE   (48ull * 1024 * 1024)
#define ASB_DISPLAY_RING_CAP    0x1000000u            /* 16 MiB g2h (a full 1080p frame + headers) */
#define ASB_DISPLAY_H2G_CAP     4096u                 /* h2g unused for the display stream */

#define ASB_INPUT_REGION_OFF    (ASB_FRAME_REGION_OFF + ASB_FRAME_REGION_SIZE)
#define ASB_INPUT_REGION_SIZE   0x10000ull
#define ASB_INPUT_RING_CAP      4096u

#define ASB_AUDIO_REGION_OFF    (ASB_INPUT_REGION_OFF + ASB_INPUT_REGION_SIZE)
#define ASB_AUDIO_REGION_SIZE   0x100000ull           /* 1 MiB */
#define ASB_AUDIO_RING_CAP      0x40000u              /* 256 KiB per ring */

#define ASB_CLIPW_REGION_OFF    (ASB_AUDIO_REGION_OFF + ASB_AUDIO_REGION_SIZE)   /* ch5 */
#define ASB_CLIPW_REGION_SIZE   0x400000ull           /* 4 MiB */
#define ASB_CLIPR_REGION_OFF    (ASB_CLIPW_REGION_OFF + ASB_CLIPW_REGION_SIZE)   /* ch6 */
#define ASB_CLIPR_REGION_SIZE   0x400000ull
#define ASB_CLIP_RING_CAP       0x100000u             /* 1 MiB per ring */

#define ASB_AGENT_REGION_OFF    (ASB_CLIPR_REGION_OFF + ASB_CLIPR_REGION_SIZE)   /* ch1 */
#define ASB_AGENT_REGION_SIZE   0x21000ull            /* 132 KiB -- must be >= ASB_AGENT_SLOT_STRIDE so the slot fits the region */
#define ASB_AGENT_RING_CAP      0x10000u              /* 64 KiB per ring */

#define ASB_SSH_REGION_OFF      (ASB_AGENT_REGION_OFF + ASB_AGENT_REGION_SIZE)   /* ch7 */
#define ASB_SSH_REGION_SIZE     0x200000ull           /* 2 MiB */
#define ASB_SSH_RING_CAP        0x20000u              /* 128 KiB per ring */
#define ASB_SSH_N_SLOTS         4u

#define ASB_P9_REGION_OFF       (ASB_SSH_REGION_OFF + ASB_SSH_REGION_SIZE)       /* ch9P */
#define ASB_P9_REGION_SIZE      0x400000ull           /* 4 MiB */
#define ASB_P9_RING_CAP         0x20000u              /* 128 KiB per ring (>= 9P msize 64 KiB) */
#define ASB_P9_N_SLOTS          2u

#define ASB_SCREENSHOT_REGION_OFF (ASB_P9_REGION_OFF + ASB_P9_REGION_SIZE)
#define ASB_SCREENSHOT_REGION_SIZE 0x21000ull
#define ASB_SCREENSHOT_RING_CAP 0x10000u
#define ASB_SCREENSHOT_SLOT_STRIDE (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_SCREENSHOT_RING_CAP))

/* Stream slot strides: [hdr][g2h hdr+cap][h2g hdr+cap]. Display is asymmetric (big g2h, tiny h2g). */
#define ASB_DISPLAY_SLOT_STRIDE (ASB_SLOT_HDR + (ASB_RING_HDR + ASB_DISPLAY_RING_CAP) + (ASB_RING_HDR + ASB_DISPLAY_H2G_CAP))
#define ASB_INPUT_SLOT_STRIDE   (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_INPUT_RING_CAP))
#define ASB_AUDIO_SLOT_STRIDE   (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_AUDIO_RING_CAP))
#define ASB_CLIP_SLOT_STRIDE    (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_CLIP_RING_CAP))
#define ASB_AGENT_SLOT_STRIDE   (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_AGENT_RING_CAP))
#define ASB_SSH_SLOT_STRIDE     (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_SSH_RING_CAP))
#define ASB_P9_SLOT_STRIDE      (ASB_SLOT_HDR + 2u * (ASB_RING_HDR + ASB_P9_RING_CAP))

/* Each region must hold all its slots: n_slots * slot_stride <= region_size. These compile-time
 * checks fail the build (negative array dimension) if a region is too small to contain its slot(s),
 * so a region/ring-cap mismatch cannot silently overlap the next region. File scope (no
 * unused-typedef warning); compiled only on the host/publisher side -- the guest reads geometry
 * from the published directory. */
typedef char asb_fits_display[(ASB_DISPLAY_SLOT_STRIDE <= ASB_FRAME_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_input  [(ASB_INPUT_SLOT_STRIDE   <= ASB_INPUT_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_audio  [(ASB_AUDIO_SLOT_STRIDE   <= ASB_AUDIO_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_clipw  [(ASB_CLIP_SLOT_STRIDE    <= ASB_CLIPW_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_clipr  [(ASB_CLIP_SLOT_STRIDE    <= ASB_CLIPR_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_agent  [(ASB_AGENT_SLOT_STRIDE   <= ASB_AGENT_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_ssh    [((uint64_t)ASB_SSH_N_SLOTS * ASB_SSH_SLOT_STRIDE <= ASB_SSH_REGION_SIZE) ? 1 : -1];
typedef char asb_fits_p9     [((uint64_t)ASB_P9_N_SLOTS  * ASB_P9_SLOT_STRIDE  <= ASB_P9_REGION_SIZE)  ? 1 : -1];

typedef char asb_fits_screenshot[(ASB_SCREENSHOT_SLOT_STRIDE <= ASB_SCREENSHOT_REGION_SIZE) ? 1 : -1];

#define ASB_SHM_LEGACY_N_REGIONS 8u
#define ASB_SHM_N_REGIONS        9u

typedef struct AsbShmRegionLayout {
    AsbShmRegionDesc region;
    uint32_t g2h_cap;
    uint32_t h2g_cap;
} AsbShmRegionLayout;

static const AsbShmRegionLayout asb_shm_regions[ASB_SHM_N_REGIONS] = {
    { { ASB_CH_DISPLAY,          ASB_REGION_STREAM, ASB_FRAME_REGION_OFF,      ASB_FRAME_REGION_SIZE,      1,               ASB_DISPLAY_SLOT_STRIDE    }, ASB_DISPLAY_RING_CAP,    ASB_DISPLAY_H2G_CAP },
    { { ASB_CH_INPUT,            ASB_REGION_STREAM, ASB_INPUT_REGION_OFF,      ASB_INPUT_REGION_SIZE,      1,               ASB_INPUT_SLOT_STRIDE      }, ASB_INPUT_RING_CAP,      ASB_INPUT_RING_CAP },
    { { ASB_CH_AUDIO,            ASB_REGION_STREAM, ASB_AUDIO_REGION_OFF,      ASB_AUDIO_REGION_SIZE,      1,               ASB_AUDIO_SLOT_STRIDE      }, ASB_AUDIO_RING_CAP,      ASB_AUDIO_RING_CAP },
    { { ASB_CH_CLIPBOARD,        ASB_REGION_STREAM, ASB_CLIPW_REGION_OFF,      ASB_CLIPW_REGION_SIZE,      1,               ASB_CLIP_SLOT_STRIDE       }, ASB_CLIP_RING_CAP,       ASB_CLIP_RING_CAP },
    { { ASB_CH_CLIPBOARD_READER, ASB_REGION_STREAM, ASB_CLIPR_REGION_OFF,      ASB_CLIPR_REGION_SIZE,      1,               ASB_CLIP_SLOT_STRIDE       }, ASB_CLIP_RING_CAP,       ASB_CLIP_RING_CAP },
    { { ASB_CH_AGENT,            ASB_REGION_STREAM, ASB_AGENT_REGION_OFF,      ASB_AGENT_REGION_SIZE,      1,               ASB_AGENT_SLOT_STRIDE      }, ASB_AGENT_RING_CAP,      ASB_AGENT_RING_CAP },
    { { ASB_CH_SSH,              ASB_REGION_STREAM, ASB_SSH_REGION_OFF,        ASB_SSH_REGION_SIZE,        ASB_SSH_N_SLOTS, ASB_SSH_SLOT_STRIDE        }, ASB_SSH_RING_CAP,        ASB_SSH_RING_CAP },
    { { ASB_CH_9P,               ASB_REGION_STREAM, ASB_P9_REGION_OFF,         ASB_P9_REGION_SIZE,         ASB_P9_N_SLOTS,  ASB_P9_SLOT_STRIDE         }, ASB_P9_RING_CAP,         ASB_P9_RING_CAP },
    { { ASB_CH_SCREENSHOT,       ASB_REGION_STREAM, ASB_SCREENSHOT_REGION_OFF, ASB_SCREENSHOT_REGION_SIZE, 1,               ASB_SCREENSHOT_SLOT_STRIDE }, ASB_SCREENSHOT_RING_CAP, ASB_SCREENSHOT_RING_CAP },
};

static inline int asb_shm_region_matches(const AsbShmRegionDesc *actual,
                                         const AsbShmRegionDesc *expected)
{
    return actual->channel_id == expected->channel_id &&
           actual->flags == expected->flags &&
           actual->offset == expected->offset &&
           actual->size == expected->size &&
           actual->n_slots == expected->n_slots &&
           actual->slot_stride == expected->slot_stride;
}

static inline uint32_t asb_shm_validate_directory(const AsbShmDirectory *dir,
                                                  uint64_t mapping_size)
{
    if (!dir || mapping_size != ASB_SHM_BAR_SIZE ||
        dir->magic != ASB_SHM_DIR_MAGIC || dir->version != ASB_SHM_VERSION ||
        dir->bar_size != ASB_SHM_BAR_SIZE ||
        (dir->n_regions != ASB_SHM_LEGACY_N_REGIONS && dir->n_regions != ASB_SHM_N_REGIONS))
        return 0;

    for (uint32_t i = 0; i < dir->n_regions; i++) {
        const AsbShmRegionLayout *layout = &asb_shm_regions[i];
        const AsbShmRegionDesc *region = &layout->region;
        uint64_t required = (uint64_t)ASB_SLOT_HDR + ASB_RING_HDR + layout->g2h_cap +
                            ASB_RING_HDR + layout->h2g_cap;
        if (!asb_shm_region_matches(&dir->regions[i], region) ||
            region->offset > mapping_size || region->size > mapping_size - region->offset ||
            region->slot_stride < required || region->slot_stride == 0 ||
            region->n_slots == 0 || region->n_slots > region->size / region->slot_stride ||
            layout->g2h_cap == 0 || (layout->g2h_cap & (layout->g2h_cap - 1)) != 0 ||
            layout->h2g_cap == 0 || (layout->h2g_cap & (layout->h2g_cap - 1)) != 0)
            return 0;
    }
    return dir->n_regions;
}

static inline const AsbShmRegionLayout *asb_shm_region_for_channel(uint32_t channel,
                                                                   uint32_t n_regions)
{
    if (n_regions != ASB_SHM_LEGACY_N_REGIONS && n_regions != ASB_SHM_N_REGIONS) return NULL;
    for (uint32_t i = 0; i < n_regions; i++)
        if (asb_shm_regions[i].region.channel_id == channel) return &asb_shm_regions[i];
    return NULL;
}

/* Initialize one region's slots in place: zero each slot, set both rings' caps, mark FREE.
 * For an asymmetric region pass distinct g2h/h2g caps (display); otherwise they're equal. */
static inline void asb_shm_init_slots(uint8_t *bar, uint64_t off, uint32_t n_slots,
                                      uint32_t stride, uint32_t g2h_cap, uint32_t h2g_cap)
{
    for (uint32_t i = 0; i < n_slots; i++) {
        uint8_t *slot = bar + off + (uint64_t)i * stride;
        memset(slot, 0, stride);
        AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
        AsbRing *h2g = (AsbRing *)((uint8_t *)g2h + ASB_RING_HDR + g2h_cap);
        g2h->cap = g2h_cap;
        h2g->cap = h2g_cap;
        *(volatile uint32_t *)slot = ASB_SLOT_FREE;
    }
}

static inline void asb_shm_set_region(AsbShmDirectory *dir, uint32_t idx,
                                      const AsbShmRegionLayout *layout)
{
    dir->regions[idx] = layout->region;
}

/* Publish the directory + initialize every slot (state FREE, ring caps set). Sets the magic LAST,
 * after a barrier, so a guest reading the page never sees a half-written table. The launcher calls
 * this once, before QEMU starts. Idempotent: re-publishing rewrites identical values. */
static inline void asb_shm_publish(uint8_t *bar, uint64_t bar_size)
{
    AsbShmDirectory *dir = (AsbShmDirectory *)bar;
    memset(bar, 0, 4096);
    dir->version   = ASB_SHM_VERSION;
    dir->bar_size  = (uint32_t)bar_size;
    dir->n_regions = ASB_SHM_N_REGIONS;

    for (uint32_t i = 0; i < ASB_SHM_N_REGIONS; i++) {
        const AsbShmRegionLayout *layout = &asb_shm_regions[i];
        asb_shm_set_region(dir, i, layout);
        asb_shm_init_slots(bar, layout->region.offset, layout->region.n_slots,
                           layout->region.slot_stride, layout->g2h_cap, layout->h2g_cap);
    }

    __sync_synchronize();
    dir->magic = ASB_SHM_DIR_MAGIC;
    __sync_synchronize();
}

#endif /* ASB_SHM_LAYOUT_H */
