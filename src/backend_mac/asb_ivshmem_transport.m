#import "asb_ivshmem_transport.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <mach/mach_time.h>   /* mach_absolute_time() -> per-host-process generation (residue id) */

#include "../../tools/transport/asb_transport.h"   /* AsbShmDirectory, AsbRing, ASB_SLOT_*, ASB_SLOT_HDR */

#define ASB_RING_HDR ((uint32_t)sizeof(AsbRing))
#define MAX_PUMPS    32
#define ASB_RESIDUE_QUIESCE_US 4000   /* host-crash residue reclaim: us to let a stale guest observe CLOSED + stop producing */
/* host->guest liveness: if the h2g ring stays full with undelivered bytes (no drain progress) this long,
 * the guest acceptor is presumed dead (force-killed helper leaves the slot ESTABLISHED with no peer-death
 * signal), so the pump tears the slot down so the connector can re-arm + a respawned guest can re-accept.
 * Generous so a merely-slow guest never trips it; data-driven so an idle channel never trips it. */
#define ASB_H2G_DEAD_MS 1500
/* guest liveness beacon: tear the slot down if the guest's heartbeat word stops advancing this long
 * (guest process force-killed/crashed). Generous vs the ~250ms guest tick so scheduling jitter is fine. */
#define ASB_HB_DEAD_MS 1500

/* Monotonic milliseconds (for the h2g stall timer). mach_absolute_time is ns on Apple Silicon; apply the
 * timebase for correctness on any host. Used only for elapsed deltas. */
static uint64_t asb_mono_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (mach_absolute_time() * tb.numer / tb.denom) / 1000000ULL;
}

/* ---- SPSC ring helpers (host side). Host is consumer of g2h, producer of h2g.
 * Identical math to asb_transport.c's ring_read_host/ring_write_host. ---- */
static int ring_read_host(AsbRing *r, uint8_t *data, void *buf, int len)
{
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail; uint32_t cap = r->cap;
    uint32_t used = (uint32_t)(tail - head);
    uint32_t n = (uint32_t)len < used ? (uint32_t)len : used;
    if (n == 0) return 0;
    __sync_synchronize();   /* ACQUIRE: order the `tail` load before the data read. Without it, weak
                             * memory (ARM64) lets the read be speculated ahead of the tail load and
                             * return bytes the producer hasn't written yet, corrupting large transfers.
                             * See the matching fix + rationale in ring_read() (tools/transport/asb_transport.c). */
    uint32_t first = cap - (uint32_t)(head & (cap - 1));
    if (first >= n) memcpy(buf, data + (head & (cap - 1)), n);
    else { memcpy(buf, data + (head & (cap - 1)), first); memcpy((uint8_t *)buf + first, data, n - first); }
    __sync_synchronize();
    r->head = head + n;
    return (int)n;
}

static int ring_write_host(AsbRing *r, uint8_t *data, const void *buf, int len)
{
    __sync_synchronize();
    uint64_t head = r->head, tail = r->tail; uint32_t cap = r->cap;
    uint32_t freeb = cap - (uint32_t)(tail - head);
    uint32_t n = (uint32_t)len < freeb ? (uint32_t)len : freeb;
    if (n == 0) return 0;
    uint32_t pos = (uint32_t)(tail & (cap - 1)), first = cap - pos;
    if (first >= n) memcpy(data + pos, buf, n);
    else { memcpy(data + pos, buf, first); memcpy(data, (const uint8_t *)buf + first, n - first); }
    __sync_synchronize();
    r->tail = tail + n;
    return (int)n;
}

/* One active bridge: a socketpair end the pump owns + the slot's rings. */
typedef struct {
    volatile uint32_t *state;         /* HOST-sole on ch1-7: the pump writes FREE on exit (under *lock) */
    volatile uint64_t *owner_id;      /* slot identity (off 8); READ-ONLY here */
    uint64_t           my_owner_id;   /* identity captured at connect = THIS pump's acceptance */
    volatile uint64_t *host_token;    /* off 16; HOST-sole; pump reads (retire on change) + clears on release */
    volatile uint64_t *guest_ack;     /* off 24; pump clears to 0 on release (host-sole on ch1-7) */
    uint64_t           my_host_token; /* the arm this pump serves; release is no-op if a newer arm differs */
    volatile uint64_t *guest_hb;      /* off 40; guest-sole liveness beacon (READ-ONLY here; staleness => dead) */
    AsbRing *g2h; uint8_t *g2hData;   /* guest -> host (we read)  */
    AsbRing *h2g; uint8_t *h2gData;   /* host  -> guest (we write) */
    int      internal_fd;             /* our end of the socketpair; caller holds the other */
    pthread_mutex_t *lock;            /* &transport->_lock: serializes the pump's terminal *state=FREE write against a concurrent connectChannel arm */
    volatile int    *closed;          /* &transport->_closed: skip the terminal write while -close tears down */
    volatile int stop;
    volatile int done;                /* set under *lock when the pump self-exits; connectChannel reaps it */
    pthread_t thread;
} AsbPump;

@implementation AsbIvshmemTransport {
    int                 _fd;
    uint8_t            *_bar;
    uint64_t            _barSize;
    AsbShmDirectory    *_dir;
    AsbPump            *_pumps[MAX_PUMPS];
    int                 _pumpCount;
    uint64_t            _host_gen;   /* this host process's generation (mach_absolute_time @ init); identifies
                                      * our arms vs a crashed prior host's residue. Monotonic within a Mac boot;
                                      * the BAR resets on reboot so cross-boot residue cannot exist. */
    uint32_t            _arm_seq;    /* per-arm counter (under _lock); low 32 bits of host_token */
    pthread_mutex_t     _lock;
    /* Set under _lock by -close before it munmaps _bar. connectChannel re-checks it (and _bar) under
     * _lock before every BAR dereference, so a consumer's reconnect loop can never dereference an
     * unmapped BAR while -close is unmapping it. -close is idempotent. */
    volatile int        _closed;
}

- (nullable instancetype)initWithBackingPath:(NSString *)path
{
    if (!(self = [super init])) return nil;
    _fd = -1;
    pthread_mutex_init(&_lock, NULL);

    int fd = open(path.fileSystemRepresentation, O_RDWR);
    if (fd < 0) return nil;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return nil; }
    void *bar = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (bar == MAP_FAILED) { close(fd); return nil; }

    AsbShmDirectory *dir = (AsbShmDirectory *)bar;
    if (dir->magic != ASB_SHM_DIR_MAGIC) {   /* launcher hasn't published yet */
        munmap(bar, (size_t)st.st_size);
        close(fd);
        return nil;
    }
    _fd = fd;
    _bar = (uint8_t *)bar;
    _barSize = (uint64_t)st.st_size;
    _dir = dir;
    _host_gen = (uint64_t)mach_absolute_time();
    if (_host_gen == 0) _host_gen = 1;   /* 0 == "no arm"; never use it as a live generation */
    _arm_seq = 0;
    return self;
}

- (BOOL)mapped { return _bar != NULL; }

- (const AsbShmRegionDesc *)regionForChannel:(int)channel
{
    if (!_dir) return NULL;
    uint32_t n = _dir->n_regions < 16 ? _dir->n_regions : 16;
    for (uint32_t i = 0; i < n; i++)
        if (_dir->regions[i].channel_id == (uint32_t)channel) return &_dir->regions[i];
    return NULL;
}

- (int)connectChannel:(int)channel timeoutMs:(int)timeoutMs
{
    /* CAS-FREE single-writer handshake. The HOST is the SOLE writer of `state` (and host_token/host_gen)
     * on host-connector channels: arm (host_token + host_gen + CONNECTING), wait for the guest to echo
     * our token into guest_ack, then publish ESTABLISHED. The guest never writes `state`, so there is no
     * two-writer clobber and no hardware atomic is needed (atomics fault on the ivshmem BAR / QEMU+HVF). */
    pthread_mutex_lock(&_lock);
    if (_closed || !_bar || !_dir) { pthread_mutex_unlock(&_lock); return -1; }
    if (_pumpCount >= MAX_PUMPS) { pthread_mutex_unlock(&_lock); return -1; }
    /* regionForChannel dereferences _dir (a pointer INTO the BAR); resolve it UNDER _lock with the
     * _closed/_bar guard so -close (which munmaps under _lock on guest shutdown) can't free it mid-deref. */
    const AsbShmRegionDesc *r = [self regionForChannel:channel];
    if (!r) { pthread_mutex_unlock(&_lock); return -1; }

    /* Scan: prefer a FREE/CLOSED slot; else fall back to a DEAD host generation's residue (a non-FREE/
     * CLOSED slot whose host_gen is neither 0 nor ours — a crashed prior host left it; host-crash recov). */
    uint8_t *slot = NULL, *residue = NULL;
    for (uint32_t i = 0; i < r->n_slots; i++) {
        uint8_t *s = _bar + r->offset + (uint64_t)i * r->slot_stride;
        __sync_synchronize();
        uint32_t st = *(volatile uint32_t *)s;
        uint64_t hg = *(volatile uint64_t *)(s + ASB_SLOT_HOST_GEN_OFFSET);
        if (st == ASB_SLOT_FREE || st == ASB_SLOT_CLOSED) { slot = s; break; }
        if (hg != 0 && hg != _host_gen) residue = s;
    }
    if (!slot && residue) {
        /* Reclaim a dead host's residue: write CLOSED so a still-live stale guest producer (its data path
         * sees state!=ESTABLISHED / host_token mismatch) STOPS, let it quiesce, THEN arm — so we never
         * zero the rings under a concurrent producer. Held under _lock (a concurrent connectChannel can't
         * grab the same slot mid-reclaim); -close is blocked only for the ~4ms quiesce, host-crash-only. */
        slot = residue;
        __sync_synchronize();
        *(volatile uint32_t *)slot = ASB_SLOT_CLOSED;
        __sync_synchronize();
        usleep(ASB_RESIDUE_QUIESCE_US);
    }
    if (!slot) { pthread_mutex_unlock(&_lock); return -1; }

    volatile uint32_t *state = (volatile uint32_t *)slot;
    volatile uint64_t *htok  = (volatile uint64_t *)(slot + ASB_SLOT_HOST_TOKEN_OFFSET);
    volatile uint64_t *gack  = (volatile uint64_t *)(slot + ASB_SLOT_GUEST_ACK_OFFSET);
    volatile uint64_t *hgen  = (volatile uint64_t *)(slot + ASB_SLOT_HOST_GEN_OFFSET);
    AsbRing *g2h = (AsbRing *)(slot + ASB_SLOT_HDR);
    uint8_t *g2hData = (uint8_t *)g2h + ASB_RING_HDR;
    uint32_t g2hCap = g2h->cap;                            /* published by the launcher */
    AsbRing *h2g = (AsbRing *)(g2hData + g2hCap);
    uint8_t *h2gData = (uint8_t *)h2g + ASB_RING_HDR;

    uint64_t tok = ((_host_gen & 0xffffffffull) << 32) | (uint64_t)(++_arm_seq);
    g2h->head = g2h->tail = 0;
    h2g->head = h2g->tail = 0;
    __sync_synchronize();
    *gack = 0;       __sync_synchronize();   /* clear any stale echo BEFORE arming (so ack==tok can't pre-match) */
    *htok = tok;
    *hgen = _host_gen;
    __sync_synchronize();
    *state = ASB_SLOT_CONNECTING;             /* arm (host-sole) */
    __sync_synchronize();
    pthread_mutex_unlock(&_lock);

    /* Wait for the guest to ACCEPT THIS arm (guest_ack == our token). *gack is read ONLY under _lock with
     * a _closed/_bar guard; the lock is held only for the read, never across the sleep. */
    int waited = 0;
    for (;;) {
        pthread_mutex_lock(&_lock);
        if (_closed || !_bar) { pthread_mutex_unlock(&_lock); return -1; }
        __sync_synchronize();
        uint64_t ack = *gack;
        pthread_mutex_unlock(&_lock);
        if (ack == tok) break;
        if (waited >= timeoutMs) {
            pthread_mutex_lock(&_lock);
            if (!_closed && _bar) { __sync_synchronize(); *htok = 0; *gack = 0; __sync_synchronize(); *state = ASB_SLOT_FREE; }
            pthread_mutex_unlock(&_lock);
            return -1;
        }
        usleep(2000); waited += 2;
    }

    /* Release the slot host-sole (host_token + guest_ack = 0 THEN state=FREE), BAR-guarded. */
    #define ASB_RELEASE_SLOT() do { pthread_mutex_lock(&_lock); \
        if (!_closed && _bar) { __sync_synchronize(); *htok = 0; *gack = 0; __sync_synchronize(); *state = ASB_SLOT_FREE; } \
        pthread_mutex_unlock(&_lock); } while (0)

    /* Publish ESTABLISHED (host-sole) + capture the guest's identity, under _lock. */
    uint64_t my_owner = 0;
    pthread_mutex_lock(&_lock);
    if (_closed || !_bar) { pthread_mutex_unlock(&_lock); return -1; }
    __sync_synchronize();
    *state = ASB_SLOT_ESTABLISHED;
    __sync_synchronize();
    my_owner = *(volatile uint64_t *)(slot + ASB_SLOT_OWNER_OFFSET);
    pthread_mutex_unlock(&_lock);

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) { ASB_RELEASE_SLOT(); return -1; }
    int noSigpipe = 1;
    if (setsockopt(sp[0], SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe)) != 0 ||
        setsockopt(sp[1], SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe)) != 0) {
        close(sp[0]); close(sp[1]); ASB_RELEASE_SLOT(); return -1;
    }
    /* The pump owns sp[1] and services BOTH directions on one thread; make its end non-blocking so a
     * backed-up guest->host write can never wedge the thread and starve host->guest. */
    fcntl(sp[1], F_SETFL, fcntl(sp[1], F_GETFL, 0) | O_NONBLOCK);

    AsbPump *p = (AsbPump *)calloc(1, sizeof(AsbPump));
    if (!p) { close(sp[0]); close(sp[1]); ASB_RELEASE_SLOT(); return -1; }
    p->state = state; p->owner_id = (volatile uint64_t *)(slot + ASB_SLOT_OWNER_OFFSET);
    p->host_token = htok; p->guest_ack = gack;
    p->guest_hb = (volatile uint64_t *)(slot + ASB_SLOT_GUEST_HB_OFFSET);
    p->g2h = g2h; p->g2hData = g2hData; p->h2g = h2g; p->h2gData = h2gData;
    p->internal_fd = sp[1];
    p->lock = &_lock; p->closed = &_closed;
    p->my_owner_id = my_owner; p->my_host_token = tok;

    /* Register the pump AND start its thread under one _lock with a final _closed recheck. */
    extern void *asb_ivshmem_pump_main(void *);
    pthread_mutex_lock(&_lock);
    /* Reap pumps that self-exited (channel retired / peer disconnected): join + free + compact, so
     * _pumpCount counts only live pumps. Without this the array fills with dead entries and reconnects
     * eventually hit MAX_PUMPS and fail. Skip while -close tears down (it joins every pump itself). */
    if (!_closed) {
        for (int i = 0; i < _pumpCount; ) {
            AsbPump *q = _pumps[i];
            if (q && q->done) { pthread_join(q->thread, NULL); free(q); _pumps[i] = _pumps[--_pumpCount]; _pumps[_pumpCount] = NULL; }
            else i++;
        }
    }
    if (_closed || !_bar || _pumpCount >= MAX_PUMPS) {
        pthread_mutex_unlock(&_lock);
        close(sp[0]); close(sp[1]); free(p);
        return -1;
    }
    if (pthread_create(&p->thread, NULL, asb_ivshmem_pump_main, p) != 0) {
        pthread_mutex_unlock(&_lock);
        close(sp[0]); close(sp[1]); free(p);
        return -1;
    }
    _pumps[_pumpCount++] = p;
    pthread_mutex_unlock(&_lock);
    #undef ASB_RELEASE_SLOT
    return sp[0];   /* caller's end */
}

- (void)close
{
    pthread_mutex_lock(&_lock);
    if (_closed) { pthread_mutex_unlock(&_lock); return; }   /* idempotent: called from QEMU exit AND core Stopped */
    _closed = 1;   /* connectChannel observes this under _lock and stops dereferencing the BAR */
    for (int i = 0; i < _pumpCount; i++) {
        AsbPump *p = _pumps[i];
        if (!p) continue;
        p->stop = 1;
        if (p->internal_fd >= 0) shutdown(p->internal_fd, SHUT_RDWR);   /* unblock the pump's select/recv (-1 once the pump closed it under _lock) */
    }
    int n = _pumpCount;
    pthread_mutex_unlock(&_lock);

    /* Join pumps OUTSIDE the lock (they take no lock, but may block briefly draining). After this no
     * pump thread touches the BAR, so the munmap below is safe w.r.t. pumps. */
    for (int i = 0; i < n; i++) {
        AsbPump *p = _pumps[i];
        if (p && p->thread) pthread_join(p->thread, NULL);
    }
    pthread_mutex_lock(&_lock);
    for (int i = 0; i < _pumpCount; i++) { free(_pumps[i]); _pumps[i] = NULL; }
    _pumpCount = 0;
    /* munmap UNDER _lock: connectChannel only reads the BAR while holding _lock with a _closed/_bar
     * guard, so this can never pull the mapping out from under a live dereference. */
    if (_bar) { munmap(_bar, (size_t)_barSize); _bar = NULL; _dir = NULL; }
    if (_fd >= 0) { close(_fd); _fd = -1; }
    pthread_mutex_unlock(&_lock);
}

- (void)dealloc { [self close]; pthread_mutex_destroy(&_lock); }

@end

/* Bridge one slot <-> the socketpair. Guest->host: drain g2h, write to the fd. Host->guest: read
 * the fd, write to h2g. Exits when the guest leaves ESTABLISHED, the caller closes its fd (EOF), or
 * -close signals stop. Releases the slot (FREE) so a later connectChannel can re-claim it. */
void *asb_ivshmem_pump_main(void *arg)
{
    AsbPump *p = (AsbPump *)arg;
    /* This single thread services BOTH directions of one ssh connection. It must never block on one
     * direction while the other has work, or a sustained full-duplex burst deadlocks: guest->host
     * blocked on a backed-up socketpair write would stop draining h2g, the guest's asb_send would
     * then spin forever on a full g2h, and the connection wedges (the "drops on larger output" bug).
     * So: internal_fd is non-blocking, we select() for read AND write readiness, and we carry any
     * partially-written guest->host chunk across iterations instead of blocking on it. */
    uint8_t g2hbuf[16384];   /* guest->host bytes drained from g2h, awaiting the socketpair write */
    int     g2hlen = 0, g2hoff = 0;   /* [g2hoff, g2hlen) still to be written to internal_fd */
    uint8_t h2gbuf[16384];   /* host->guest bytes read from internal_fd, awaiting the h2g ring */
    int     h2glen = 0, h2goff = 0;   /* [h2goff, h2glen) still to be written to h2g */
    uint64_t h2g_stall_since = 0;     /* when the h2g ring first went full-with-pending (0 = not stalled) */
    /* Guest liveness beacon: track the guest_hb word (bumped ~4Hz by the guest's ticker thread). We key on
     * it CHANGING — clock-domain-safe (guest and host clocks differ) — and only ENFORCE staleness after we
     * have seen it change at least once (hb_armed), so a legacy guest that never beats, or a stale value
     * left in the slot, is never false-killed. Once armed, no change for ASB_HB_DEAD_MS => the guest PROCESS
     * is dead (force-kill/crash), even on an idle or read-only channel that has no I/O to fail. */
    uint64_t hb_last      = p->guest_hb ? *p->guest_hb : 0;
    uint64_t hb_change_ms = asb_mono_ms();
    int      hb_armed     = 0;

    /* Exit conditions, all polled each iteration: caller asked us to stop; the slot left ESTABLISHED
     * (peer/host disconnect); OR owner_id changed — a NEW acceptance (a respawned guest, or a VDD
     * re-arm via asb_stream_reset) now owns this slot, so THIS pump no longer does. owner_id is read
     * lock-free with the same lifetime/safety as *p->state (the pump is joined before -close munmaps).
     * On any of these we fall into done:, which (unchanged) is the SOLE writer of *p->state=FREE on
     * our own exit. No clock, no host reaper, no new state-writer — identity, not elapsed time. */
    while (!p->stop && *p->state == ASB_SLOT_ESTABLISHED &&
           (__sync_synchronize(), *p->owner_id == p->my_owner_id) &&
           *p->host_token == p->my_host_token) {   /* a newer host arm (new token) also retires us */
        int did_work = 0;

        /* Guest liveness beacon staleness check (covers ALL channels + both directions + idle, unlike the
           write-only h2g stall below). Only enforced once the beacon has been seen to advance at least once. */
        if (p->guest_hb) {
            uint64_t hb = *p->guest_hb;
            if (hb != hb_last) { hb_last = hb; hb_change_ms = asb_mono_ms(); hb_armed = 1; }
            else if (hb_armed && asb_mono_ms() - hb_change_ms >= ASB_HB_DEAD_MS) goto done;  /* guest process dead */
        }

        /* host -> guest: push any pending bytes into the h2g ring (partial ok, never blocks). */
        int h2g_progress = 0;
        while (h2goff < h2glen) {
            int w = ring_write_host(p->h2g, p->h2gData, h2gbuf + h2goff, h2glen - h2goff);
            if (w > 0) { h2goff += w; did_work = 1; h2g_progress = 1; } else break;   /* ring full: guest draining */
        }
        if (h2goff >= h2glen) { h2goff = h2glen = 0; h2g_stall_since = 0; }   /* delivered -> not stalled */
        else if (h2g_progress) { h2g_stall_since = 0; }                       /* partial drain -> guest alive */
        else {
            /* Undelivered host->guest bytes, ring full, NO drain progress: the guest acceptor isn't
               consuming. ivshmem carries no peer-death signal (a force-killed guest helper leaves the slot
               ESTABLISHED forever), so synthesize one — if the ring stays full this long, treat the
               acceptor as dead and exit, FREEing the slot so the connector can re-arm and a respawned
               guest helper can re-accept (the analog of an hvsocket RST). Data-driven: this only arms
               while we are holding undeliverable bytes, so an IDLE channel never trips it. */
            uint64_t now = asb_mono_ms();
            if (h2g_stall_since == 0) h2g_stall_since = now;
            else if (now - h2g_stall_since >= ASB_H2G_DEAD_MS) goto done;
        }

        /* guest -> host: refill the staging buffer from g2h when it's empty. */
        if (g2hoff >= g2hlen) {
            g2hoff = g2hlen = 0;
            int n = ring_read_host(p->g2h, p->g2hData, g2hbuf, (int)sizeof(g2hbuf));
            if (n > 0) { g2hlen = n; did_work = 1; }
        }
        /* ...and flush as much of it to the socketpair as fits right now (non-blocking). */
        while (g2hoff < g2hlen) {
            ssize_t w = send(p->internal_fd, g2hbuf + g2hoff, (size_t)(g2hlen - g2hoff), 0);
            if (w > 0) { g2hoff += (int)w; did_work = 1; }
            else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   /* fd full: select below */
            else goto done;   /* peer closed/error */
        }

        /* If both directions made progress this pass, loop again immediately; otherwise wait for the
         * one thing we're blocked on (fd readable for more h2g, fd writable to flush g2h) with a short
         * timeout so a ring-full stall still re-checks the rings promptly. */
        if (did_work) continue;

        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        int want_read = (h2glen == 0);             /* only solicit host->guest bytes when we can stage them; */
        if (want_read) FD_SET(p->internal_fd, &rfds);  /* else don't spin on a readable fd we can't yet drain */
        int want_write = (g2hoff < g2hlen);        /* still have guest->host bytes to flush */
        if (want_write) FD_SET(p->internal_fd, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 2000 };
        int s = select(p->internal_fd + 1, want_read ? &rfds : NULL, want_write ? &wfds : NULL, NULL, &tv);
        if (s < 0) { if (errno == EINTR) continue; break; }
        if (s > 0 && want_read && FD_ISSET(p->internal_fd, &rfds)) {
            ssize_t rd = recv(p->internal_fd, h2gbuf, sizeof(h2gbuf), 0);
            if (rd == 0) break;   /* caller closed its end */
            if (rd < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            if (rd > 0) h2glen = (int)rd;
        }
    }
done:
    /* Close our fd AND release the slot under *p->lock, serialized against a concurrent connectChannel arm
     * and against -close. Closing internal_fd under the lock means -close's shutdown(internal_fd)
     * can never act on a fd number this pump already closed and the OS reused.
     * Slot release is host_token-guarded so a newer arm that already retook this slot is never clobbered;
     * if *p->closed, -close is tearing down (it munmaps under the lock AFTER joining us) — skip the write. */
    pthread_mutex_lock(p->lock);
    if (p->internal_fd >= 0) { close(p->internal_fd); p->internal_fd = -1; }
    if (!*p->closed) {
        __sync_synchronize();
        if (*p->host_token == p->my_host_token) {
            *p->guest_ack = 0; __sync_synchronize();   /* clear echo (host-sole) */
            *p->state = ASB_SLOT_FREE;                 /* guest sees != ESTABLISHED -> disconnect; slot reusable */
        }
        __sync_synchronize();
    }
    p->done = 1;          /* mark for reaping: connectChannel sweeps done pumps; -close joins the rest */
    pthread_mutex_unlock(p->lock);
    return NULL;
}
