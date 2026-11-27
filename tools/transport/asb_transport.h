/*
 * asb_transport.h — AppSandbox guest transport abstraction.
 *
 * One API, two backends, chosen at runtime:
 *   - PC  (default): AF_HYPERV / HCS hvsocket.
 *   - Mac:           ivshmem shared memory (our tools/ivshmem driver maps BAR2 to a user VA).
 *
 * The 128 MiB BAR is statically partitioned: every service gets its OWN region (see the directory
 * below). Each guest sub-program (its own process) runs its own thread that reads/writes ONLY its
 * region — no cross-service locks. The byte protocols above the transport are identical on both.
 *
 * No third-party code.
 */
#ifndef ASB_TRANSPORT_H
#define ASB_TRANSPORT_H

#include <stdint.h>
#include "asb_atomics.h"   /* force _Interlocked* INLINE on MSVC (/MT outlined-atomics crash fix); no-op off MSVC */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Channels (the N in the PC service GUID a5b0cafe-000N-4000-8000-000000000001) ---- */
#define ASB_CH_AGENT            1
#define ASB_CH_DISPLAY          2      /* VDD frame region (not a stream)            */
#define ASB_CH_INPUT            3
#define ASB_CH_AUDIO            4
#define ASB_CH_CLIPBOARD        5
#define ASB_CH_CLIPBOARD_READER 6
#define ASB_CH_SSH              7
#define ASB_CH_GAMEPAD          8
#define ASB_CH_9P               50001

/* ================================================================================
 * Shared-memory wire format (identical on guest and host; little-endian; fixed layout).
 * ================================================================================ */

#define ASB_SHM_DIR_MAGIC   0x31444D4853425341ULL /* "ASBSHMD1" (little-endian) */
#define ASB_SHM_VERSION     1

#define ASB_REGION_STREAM   0x0001u   /* connection slots + SPSC rings */
#define ASB_REGION_FRAME    0x0002u   /* VDD double/triple-buffered frames */

/* Slot/ring layout constants (both guest and host lay the region out identically):
 * a slot is [ASB_SLOT_HDR bytes][AsbRing g2h hdr (sizeof AsbRing)][g2h.cap data][AsbRing h2g hdr][h2g.cap data]. */
#define ASB_SLOT_HDR   64u

/* Byte offset of AsbSlot.owner_id within the 64-byte slot header (state@0, _pad@4, owner_id@8). Guest
 * (asb_transport.c) and Mac host (asb_ivshmem_transport.m) compute the owner_id pointer from the slot
 * base via this constant. ivshmem (Mac<->Win) ONLY — the PC AF_HYPERV path never maps slot memory. */
#define ASB_SLOT_OWNER_OFFSET 8u

/* CAS-free single-writer handshake words (ivshmem; host-connector channels ch1-7). Each is written by
 * EXACTLY ONE side, so no BAR word ever has two writers — a hardware atomic RMW (casal/ldxr-stxr) is an
 * illegal instruction on the ivshmem BAR under QEMU+HVF. host_token/host_gen = HOST-sole; guest_ack =
 * GUEST-sole; owner_id = GUEST-sole; state = HOST-sole on ch1-7 (GUEST-sole on the 9P region, which
 * inverts connector/acceptor roles and is left at its validated baseline). */
#define ASB_SLOT_HOST_TOKEN_OFFSET 16u   /* host-sole: per-arm handshake nonce; 0 = released */
#define ASB_SLOT_GUEST_ACK_OFFSET  24u   /* guest-sole (ch1-7): echo of the host_token the guest accepted */
#define ASB_SLOT_HOST_GEN_OFFSET   32u   /* host-sole: arming host's process generation (mach_absolute_time), residue id */
#define ASB_SLOT_GUEST_HB_OFFSET   40u   /* guest-sole: liveness beacon. A guest background ticker bumps it
                                          * ~4Hz while the owning PROCESS is alive (data-INDEPENDENT, so an
                                          * idle channel still beats); the host pump tears the slot down if it
                                          * goes stale. This is the ivshmem analog of the OS-delivered peer
                                          * death that hvsocket (Win<->Win) / vsock (Mac<->Mac) give for free.
                                          * Host keys on the value CHANGING (clock-domain-safe) and only after
                                          * it has seen >=1 change (so a legacy guest that never beats is never
                                          * false-killed). ivshmem (Mac<->Win) ONLY. */
#define ACCEPT_EST_WAIT_MS 2000          /* guest accept: max ms to wait for the host to publish ESTABLISHED */

/* Connection slot states. */
#define ASB_SLOT_FREE        0u
#define ASB_SLOT_CONNECTING  1u   /* connector claimed it, awaiting accept */
#define ASB_SLOT_ESTABLISHED 2u
#define ASB_SLOT_CLOSING     3u
#define ASB_SLOT_CLOSED      4u

/* One lock-free single-producer/single-consumer byte ring. cap is a power of 2.
 * Producer: write data[tail & (cap-1)]…, barrier, tail += n.  Consumer: read from head, barrier,
 * head += n.  used = tail-head; free = cap-(tail-head). */
typedef struct AsbRing {
    volatile uint64_t head;     /* consumer index (monotonic) */
    volatile uint64_t tail;     /* producer index (monotonic) */
    uint32_t          cap;      /* data capacity, power of 2 */
    uint32_t          _pad;
    /* uint8_t data[cap] follows immediately */
} AsbRing;

/* One bidirectional connection slot: guest->host and host->guest rings. */
typedef struct AsbSlot {
    volatile uint32_t state;    /* ASB_SLOT_* */
    uint32_t          _pad;
    volatile uint64_t owner_id; /* (guest_pid<<32)|accept_seq, at ASB_SLOT_OWNER_OFFSET=8. The guest
                                 * acceptor stamps it just before publishing ESTABLISHED (and in
                                 * asb_stream_reset); the Mac host pump captures it once and EXITS when
                                 * it changes (a NEW acceptance took the slot). A respawned guest (fresh
                                 * pid) stamps a new owner_id, so its dead predecessor's residue is reclaimed
                                 * host-side when the pump observes the changed identity. Liveness by
                                 * IDENTITY, never a clock -> no idle false-positive. Mac/ivshmem ONLY;
                                 * the PC AF_HYPERV path never maps slot memory. */
    volatile uint64_t host_token; /* @16 HOST-sole (ch1-7): per-arm handshake nonce; 0 = released */
    volatile uint64_t guest_ack;  /* @24 GUEST-sole (ch1-7): echo of accepted host_token = "accept this arm" */
    volatile uint64_t host_gen;   /* @32 HOST-sole: arming host generation (mach_absolute_time @ init) */
    volatile uint64_t guest_hb;   /* @40 GUEST-sole: liveness beacon, bumped ~4Hz by a guest ticker thread */
    /* AsbRing g2h (+ its data), then AsbRing h2g (+ its data) follow; layout via slot_stride. */
} AsbSlot;

/* Per-region directory entry. */
typedef struct AsbShmRegionDesc {
    uint32_t channel_id;        /* ASB_CH_* */
    uint32_t flags;             /* ASB_REGION_STREAM | ASB_REGION_FRAME */
    uint64_t offset;            /* from BAR base */
    uint64_t size;
    uint32_t n_slots;           /* stream: 1, or 8 for ssh/9p */
    uint32_t slot_stride;       /* stream: bytes per AsbSlot (incl. both rings + data) */
} AsbShmRegionDesc;

/* The directory at BAR offset 0 (one 4 KiB page). Written by the host at VM create. */
typedef struct AsbShmDirectory {
    uint64_t magic;             /* ASB_SHM_DIR_MAGIC */
    uint32_t version;           /* ASB_SHM_VERSION */
    uint32_t bar_size;
    volatile uint64_t host_epoch;   /* host liveness; 0 = host gone */
    volatile uint64_t guest_epoch;  /* guest agent liveness */
    uint32_t n_regions;
    uint32_t _pad;
    AsbShmRegionDesc regions[16];   /* up to 16 services */
} AsbShmDirectory;

/* VDD frame region header (ASB_REGION_FRAME). Buffers follow the header. */
typedef struct AsbFrameRegion {
    uint32_t magic;             /* 'ASFR' */
    uint32_t n_buffers;         /* 2 (double) or 3 (triple) */
    uint32_t width, height, stride, format;  /* BGRA */
    volatile uint32_t produced_seq;  /* VDD bumps after publishing active_buffer */
    volatile uint32_t consumed_seq;  /* host bumps after reading */
    volatile uint32_t active_buffer; /* index the VDD just published */
    uint32_t dirty_rect_count;
    uint64_t buffers_offset;    /* from region base to buffer[0] */
    uint64_t buffer_stride;     /* bytes per buffer (width*height*4, page-aligned) */
    uint64_t cursor_offset;     /* ASCR cursor header + image, or 0 */
} AsbFrameRegion;

/* Hardware-cursor area (at AsbFrameRegion.cursor_offset). The VDD writes the guest's HW cursor
 * (position + shape) here so the host can draw it as an overlay — the captured frame does NOT
 * contain the hardware cursor. The image (cursor_type 1=MASKED_COLOR, 2=ALPHA/BGRA) follows the
 * header. pos_seq bumps on any move/visibility change; shape_seq bumps when the image changes. */
#define ASB_CURSOR_MAGIC  0x52435341u   /* 'ASCR' */
#define ASB_CURSOR_MAX_W  256u
#define ASB_CURSOR_MAX_H  256u
typedef struct AsbCursor {
    uint32_t magic;                 /* ASB_CURSOR_MAGIC */
    volatile uint32_t pos_seq;      /* host polls; bumped on position/visibility change */
    volatile uint32_t shape_seq;    /* bumped when the image (below) changes */
    int32_t  x, y;                  /* cursor position in guest framebuffer pixels (top-left origin) */
    uint32_t visible;
    uint32_t width, height, pitch;
    uint32_t xhot, yhot;
    uint32_t cursor_type;           /* 1=MASKED_COLOR, 2=ALPHA */
    uint32_t image_size;            /* bytes of image following this header */
    /* image[image_size] follows immediately */
} AsbCursor;
#define ASB_CURSOR_AREA  ((uint64_t)sizeof(AsbCursor) + (uint64_t)ASB_CURSOR_MAX_W*ASB_CURSOR_MAX_H*4*2)

/* ================================================================================
 * Opaque handles + API (drop-in for the socket sites).
 * ================================================================================ */
typedef struct AsbListener AsbListener;
typedef struct AsbConn     AsbConn;
typedef struct AsbFrame    AsbFrame;

/* Init the transport (selects backend). PC: WSAStartup. Mac: open ivshmem driver, map BAR, find the
 * directory. Returns 0 on success. */
int  asb_transport_init(void);

/* True if the Mac (ivshmem) backend is active; false = PC (AF_HYPERV). */
int  asb_transport_is_ivshmem(void);

/* Stream channels (replaces socket+bind+listen / select+accept / connect / recv / send / close). */
AsbListener *asb_listen(int channel);
AsbConn     *asb_accept(AsbListener *l, int timeout_ms);   /* NULL on timeout */
AsbConn     *asb_connect(int channel);                     /* 9P guest->host connect-out */
int          asb_recv(AsbConn *c, void *buf, int len);     /* blocking; <=0 = closed */
int          asb_send(AsbConn *c, const void *buf, int len);
/* Wait up to timeout_ms for inbound data (replaces select() in the agent heartbeat loop):
 * >0 = data ready, 0 = timeout, <0 = closed. timeout_ms<0 = block. */
int          asb_poll(AsbConn *c, int timeout_ms);
/* PC: setsockopt SO_RCVTIMEO/SO_SNDTIMEO. Mac (ivshmem): no-op (poll-driven). */
void         asb_set_timeout(AsbConn *c, int recv_ms, int send_ms);
void         asb_close(AsbConn *c);
void         asb_close_listener(AsbListener *l);

/* Free a connection wrapper WITHOUT signaling the slot/peer. Use only when a new connection has
 * already taken over the SAME ivshmem slot (host relaunch on a single-slot channel), so closing the
 * stale wrapper would wrongly mark the shared slot CLOSING and kill the new one. On PC this would
 * leak the socket — callers must close PC sockets normally and reserve this for the ivshmem case. */
void         asb_abandon(AsbConn *c);
/* Acceptor-side: empty this connection's outbound (g2h) ring and (re)mark the slot ESTABLISHED, so a
 * fresh stream (e.g. the VDD's first full frame after a host reattach) starts at ring offset 0,
 * aligned with the just-attached consumer. No-op on PC. */
void         asb_stream_reset(AsbConn *c);

/* Newline-framed helpers (preserve the existing recv_line/send_line semantics). */
int          asb_recv_line(AsbConn *c, char *buf, int buf_size);
int          asb_send_line(AsbConn *c, const char *msg);

/* VDD frame channel (ch2). On PC this rides the ch2 hvsocket; on Mac it writes the frame region. */
AsbFrame    *asb_frame_open(int width, int height, int n_buffers);
void        *asb_frame_back_buffer(AsbFrame *f);           /* buffer to write the next frame into */
void         asb_frame_publish(AsbFrame *f);               /* mark it produced */
void         asb_frame_close(AsbFrame *f);
void        *asb_frame_cursor(AsbFrame *f);                /* AsbCursor* in the region, or NULL */

/* Raw region access (ivshmem only): base pointer of the first region with this channel_id,
 * or NULL. For a STREAM region laid out as a bare AsbRing (header + data), asb_ring_drain
 * consumes up to len bytes into buf using the standard SPSC ring semantics. Used by simple
 * one-directional channels (e.g. host->guest input) that don't need the slot handshake. */
void        *asb_transport_region_base(int channel_id);
int          asb_ring_drain(void *region_base, void *buf, int len);

/* The underlying PC socket for a connection (so PC-only code paths — e.g. the SSH relay's dual-fd
 * select — stay byte-identical), or (unsigned long long)(~0) on ivshmem. Returned as u64 so this
 * header stays usable on the Mac host where SOCKET is undefined. */
unsigned long long asb_conn_socket_u64(AsbConn *c);

#ifdef __cplusplus
}
#endif

#endif /* ASB_TRANSPORT_H */
