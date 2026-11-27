/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-001::REQ-001 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-019 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-009::REQ-020 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
#include "vgd_shm.h"
#include <string.h>

#if defined(_MSC_VER)
#include <intrin.h>
static void vgd_memory_barrier(void)
{
    _ReadWriteBarrier();
#if defined(_M_AMD64) || defined(_M_IX86)
    _mm_mfence();
#elif defined(_M_ARM64)
    __dmb(_ARM64_BARRIER_ISH);
#endif
}
#else
static void vgd_memory_barrier(void)
{
    __sync_synchronize();
}
#endif

typedef char vgd_shm_size_check[(sizeof(VgdShmBlock) == VGD_SHM_BLOCK_SIZE) ? 1 : -1];

static int copy_with_slot_ascii(const char *prefix, uint32_t slot, char *out, size_t cap)
{
    size_t plen;
    if (!out || slot >= VGD_MAX_SLOTS) return -1;
    plen = strlen(prefix);
    if (cap < plen + 2u) return -1;
    memcpy(out, prefix, plen);
    out[plen] = (char)('0' + (char)slot);
    out[plen + 1u] = '\0';
    return 0;
}

static int copy_with_slot_utf16(const char *prefix, uint32_t slot, uint16_t *out, size_t cap)
{
    size_t plen, i;
    if (!out || slot >= VGD_MAX_SLOTS) return -1;
    plen = strlen(prefix);
    if (cap < plen + 2u) return -1;
    for (i = 0; i < plen; i++) {
        out[i] = (uint16_t)(unsigned char)prefix[i];
    }
    out[plen] = (uint16_t)('0' + (char)slot);
    out[plen + 1u] = 0u;
    return 0;
}

int vgd_shm_format_names(uint32_t slot,
                         char *shm_name, size_t shm_cap,
                         char *in_evt_name, size_t in_cap,
                         char *out_evt_name, size_t out_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (shm_name && copy_with_slot_ascii("Global\\AppSandboxGamepad", slot, shm_name, shm_cap) != 0)
        return -1;
    if (in_evt_name && copy_with_slot_ascii("Global\\AppSandboxGamepadIn", slot, in_evt_name, in_cap) != 0)
        return -1;
    if (out_evt_name && copy_with_slot_ascii("Global\\AppSandboxGamepadOut", slot, out_evt_name, out_cap) != 0)
        return -1;
    return 0;
}

int vgd_shm_format_names_w(uint32_t slot,
                           uint16_t *shm_name, size_t shm_cap,
                           uint16_t *in_evt_name, size_t in_cap,
                           uint16_t *out_evt_name, size_t out_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (shm_name && copy_with_slot_utf16("Global\\AppSandboxGamepad", slot, shm_name, shm_cap) != 0)
        return -1;
    if (in_evt_name && copy_with_slot_utf16("Global\\AppSandboxGamepadIn", slot, in_evt_name, in_cap) != 0)
        return -1;
    if (out_evt_name && copy_with_slot_utf16("Global\\AppSandboxGamepadOut", slot, out_evt_name, out_cap) != 0)
        return -1;
    return 0;
}

int vgd_shm_format_doorbells(uint32_t slot,
                             char *xusb_in_name, size_t xusb_cap,
                             char *hid_in_name, size_t hid_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (xusb_in_name && copy_with_slot_ascii("Global\\AppSandboxGamepadXusbIn", slot, xusb_in_name, xusb_cap) != 0)
        return -1;
    if (hid_in_name && copy_with_slot_ascii("Global\\AppSandboxGamepadHidIn", slot, hid_in_name, hid_cap) != 0)
        return -1;
    return 0;
}

int vgd_shm_format_doorbells_w(uint32_t slot,
                               uint16_t *xusb_in_name, size_t xusb_cap,
                               uint16_t *hid_in_name, size_t hid_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (xusb_in_name && copy_with_slot_utf16("Global\\AppSandboxGamepadXusbIn", slot, xusb_in_name, xusb_cap) != 0)
        return -1;
    if (hid_in_name && copy_with_slot_utf16("Global\\AppSandboxGamepadHidIn", slot, hid_in_name, hid_cap) != 0)
        return -1;
    return 0;
}

int vgd_format_hwids(uint32_t slot,
                     char *xusb_hwid, size_t xusb_cap,
                     char *hid_hwid, size_t hid_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (xusb_hwid && copy_with_slot_ascii("root\\AppSandboxVGD_XUSB_", slot, xusb_hwid, xusb_cap) != 0)
        return -1;
    if (hid_hwid && copy_with_slot_ascii("root\\AppSandboxVGD_HID_", slot, hid_hwid, hid_cap) != 0)
        return -1;
    return 0;
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-015 */
int vgd_format_pnp_instance_ids(uint32_t slot,
                                char *xusb_inst, size_t xusb_cap,
                                char *hid_inst, size_t hid_cap)
{
    if (slot >= VGD_MAX_SLOTS) return -1;
    if (xusb_inst && copy_with_slot_ascii("ROOT\\VID_045E&PID_028E&XI_00\\ASB_SLOT_", slot, xusb_inst, xusb_cap) != 0)
        return -1;
    if (hid_inst && copy_with_slot_ascii("ROOT\\VID_045E&PID_028E&IG_00\\ASB_SLOT_", slot, hid_inst, hid_cap) != 0)
        return -1;
    return 0;
}

int vgd_format_pnp_hwids_msz(uint32_t slot, int is_xusb,
                             char *out, size_t cap, size_t *out_len)
{
    const char *primary = is_xusb ? "root\\VID_045E&PID_028E&XI_00"
                                  : "root\\VID_045E&PID_028E&IG_00";
    char secondary[64];
    size_t l1, l2, total;

    if (!out || slot >= VGD_MAX_SLOTS) return -1;
    if (is_xusb) {
        if (vgd_format_hwids(slot, secondary, sizeof(secondary), NULL, 0) != 0) return -1;
    } else {
        if (vgd_format_hwids(slot, NULL, 0, secondary, sizeof(secondary)) != 0) return -1;
    }
    l1 = strlen(primary);
    l2 = strlen(secondary);
    total = l1 + 1u + l2 + 2u; /* primary\0secondary\0\0 */
    if (cap < total) return -1;
    memcpy(out, primary, l1 + 1u);
    memcpy(out + l1 + 1u, secondary, l2 + 1u);
    out[total - 1u] = '\0';
    if (out_len) *out_len = total;
    return 0;
}

const char *vgd_get_compat_ids_msz(int is_xusb, size_t *out_len)
{
    static const char k_xusb_compat[] =
        "USB\\MS_COMP_XUSB10\0"
        "USB\\Class_FF&SubClass_5D&Prot_01\0"
        "USB\\Class_FF&SubClass_5D\0"
        "USB\\Class_FF\0";
    static const char k_hid_compat[] =
        "USB\\MS_COMP_XUSB10\0"
        "USB\\Class_FF&SubClass_5D&Prot_01\0"
        "USB\\Class_FF&SubClass_5D\0";

    if (is_xusb) {
        if (out_len) *out_len = sizeof(k_xusb_compat);
        return k_xusb_compat;
    }
    if (out_len) *out_len = sizeof(k_hid_compat);
    return k_hid_compat;
}

static char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static int find_ci_suffix_digit(const char *haystack, const char *needle)
{
    size_t hlen, nlen, i, j;
    if (!haystack || !needle) return -1;
    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (hlen < nlen + 1u) return -1;
    for (i = 0; i + nlen < hlen; i++) {
        int match = 1;
        for (j = 0; j < nlen; j++) {
            if (ascii_upper(haystack[i + j]) != ascii_upper(needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            char d = haystack[i + nlen];
            char next = haystack[i + nlen + 1u];
            if (d >= '0' && d < (char)('0' + (char)VGD_MAX_SLOTS) &&
                (next == '\0' || next == '\\' || next == '&')) {
                return (int)(d - '0');
            }
        }
    }
    return -1;
}

int vgd_parse_slot_from_hwid(const char *hwid)
{
    int slot;
    if (!hwid) return -1;
    slot = find_ci_suffix_digit(hwid, "AppSandboxVGD_XUSB_");
    if (slot >= 0) return slot;
    return find_ci_suffix_digit(hwid, "AppSandboxVGD_HID_");
}

int vgd_shm_init(VgdShmBlock *blk, uint32_t slot)
{
    if (!blk || slot >= VGD_MAX_SLOTS) return -1;
    memset(blk, 0, sizeof(*blk));
    blk->magic = VGD_SHM_MAGIC;
    blk->version = VGD_SHM_VERSION;
    blk->slot = slot;
    blk->connected = 1u;
    blk->input_seq = 0u;
    blk->rumble_seq = 0u;
    blk->ring_write_idx = 0u;
    blk->ring_magic = VGD_RING_MAGIC;
    vgd_memory_barrier();
    return 0;
}

int vgd_shm_is_valid(const VgdShmBlock *blk)
{
    if (!blk) return 0;
    return (blk->magic == VGD_SHM_MAGIC &&
            blk->version == VGD_SHM_VERSION &&
            blk->slot < VGD_MAX_SLOTS) ? 1 : 0;
}

int vgd_shm_write_state(VgdShmBlock *blk, const GamepadState *state)
{
    uint32_t seq, widx;
    if (!blk || !state || !vgd_shm_is_valid(blk)) return -1;
    seq = blk->input_seq;
    if ((seq & 1u) != 0u) seq++; /* recover odd sequence if interrupted */
    blk->input_seq = seq + 1u;
    vgd_memory_barrier();
    blk->state = *state;
    widx = blk->ring_write_idx;
    blk->ring[widx & VGD_RING_MASK] = *state;
    vgd_memory_barrier();
    blk->ring_write_idx = widx + 1u;
    vgd_memory_barrier();
    blk->input_seq = seq + 2u;
    vgd_memory_barrier();
    return 0;
}

int vgd_shm_read_state(const VgdShmBlock *blk, GamepadState *out_state, uint32_t *out_gen)
{
    int attempt;
    if (!blk || !out_state || !vgd_shm_is_valid(blk)) return -1;
    for (attempt = 0; attempt < 64; attempt++) {
        uint32_t seq1 = blk->input_seq;
        GamepadState snap;
        uint32_t seq2;
        if ((seq1 & 1u) != 0u) continue;
        vgd_memory_barrier();
        snap = blk->state;
        vgd_memory_barrier();
        seq2 = blk->input_seq;
        if (seq1 == seq2) {
            *out_state = snap;
            if (out_gen) *out_gen = seq1 >> 1;
            return 0;
        }
    }
    return -1;
}

int vgd_shm_read_next_state(const VgdShmBlock *blk, uint32_t *cursor_inout,
                            uint16_t *last_buttons_inout,
                            GamepadState *out_state, uint32_t *out_gen)
{
    int attempt;
    if (!blk || !cursor_inout || !out_state || !vgd_shm_is_valid(blk)) return -1;
    if (blk->ring_magic != VGD_RING_MAGIC) return -1;

    for (attempt = 0; attempt < 64; attempt++) {
        uint32_t seq1 = blk->input_seq;
        uint32_t widx, cur, target, seq2;
        uint16_t ref_buttons;
        GamepadState snap;

        if ((seq1 & 1u) != 0u) continue;
        vgd_memory_barrier();
        widx = blk->ring_write_idx;
        cur = *cursor_inout;
        if (cur == widx) {
            vgd_memory_barrier();
            if (blk->input_seq == seq1) return 0;
            continue;
        }
        if ((widx - cur) > VGD_RING_CAP) {
            cur = widx - VGD_RING_CAP;
        }
        ref_buttons = last_buttons_inout ? *last_buttons_inout : 0u;
        target = widx - 1u;
        {
            uint32_t i;
            for (i = cur; i < widx; i++) {
                if (blk->ring[i & VGD_RING_MASK].buttons != ref_buttons) {
                    target = i;
                    break;
                }
            }
        }
        snap = blk->ring[target & VGD_RING_MASK];
        vgd_memory_barrier();
        seq2 = blk->input_seq;
        if (seq1 == seq2) {
            *out_state = snap;
            *cursor_inout = target + 1u;
            if (last_buttons_inout) *last_buttons_inout = snap.buttons;
            if (out_gen) *out_gen = target + 1u;
            return 1;
        }
    }
    return -1;
}

int vgd_shm_write_rumble(VgdShmBlock *blk, uint8_t large_motor, uint8_t small_motor)
{
    uint32_t seq;
    if (!blk || !vgd_shm_is_valid(blk)) return -1;
    seq = blk->rumble_seq;
    if ((seq & 1u) != 0u) seq++;
    blk->rumble_seq = seq + 1u;
    vgd_memory_barrier();
    blk->large_motor = large_motor;
    blk->small_motor = small_motor;
    vgd_memory_barrier();
    blk->rumble_seq = seq + 2u;
    vgd_memory_barrier();
    return 0;
}

int vgd_shm_read_rumble(const VgdShmBlock *blk, uint8_t *out_large, uint8_t *out_small, uint32_t *out_gen)
{
    int attempt;
    if (!blk || !out_large || !out_small || !vgd_shm_is_valid(blk)) return -1;
    for (attempt = 0; attempt < 64; attempt++) {
        uint32_t seq1 = blk->rumble_seq;
        uint8_t lm, sm;
        uint32_t seq2;
        if ((seq1 & 1u) != 0u) continue;
        vgd_memory_barrier();
        lm = blk->large_motor;
        sm = blk->small_motor;
        vgd_memory_barrier();
        seq2 = blk->rumble_seq;
        if (seq1 == seq2) {
            *out_large = lm;
            *out_small = sm;
            if (out_gen) *out_gen = seq1 >> 1;
            return 0;
        }
    }
    return -1;
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
int vgd_shm_set_product_name_utf8(VgdShmBlock *blk, const char *utf8_name)
{
    uint16_t tmp[VGD_PRODUCT_NAME_MAX];
    size_t out_i = 0;
    const unsigned char *p;

    if (!blk || !vgd_shm_is_valid(blk)) return -1;
    memset(tmp, 0, sizeof(tmp));

    if (utf8_name && utf8_name[0] != '\0') {
        p = (const unsigned char *)utf8_name;
        while (*p != 0u && out_i + 1u < VGD_PRODUCT_NAME_MAX) {
            uint32_t cp;
            if (*p < 0x80u) {
                cp = *p++;
            } else if ((*p & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
                p += 2;
            } else if ((*p & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
                p += 3;
            } else if ((*p & 0xF8u) == 0xF0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
                cp = ((uint32_t)(p[0] & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
                     ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
                p += 4;
            } else {
                cp = (uint32_t)'?';
                p++;
            }

            if (cp <= 0xFFFFu) {
                tmp[out_i++] = (uint16_t)cp;
            } else if (cp <= 0x10FFFFu && out_i + 2u < VGD_PRODUCT_NAME_MAX) {
                cp -= 0x10000u;
                tmp[out_i++] = (uint16_t)(0xD800u + (cp >> 10));
                tmp[out_i++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
            }
        }
    }
    tmp[out_i] = 0u;

    memcpy(blk->product_name, tmp, sizeof(tmp));
    vgd_memory_barrier();
    return 0;
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
size_t vgd_shm_get_product_name_w(const VgdShmBlock *blk, uint16_t *out_w, size_t out_cap)
{
    size_t i = 0, limit;
    if (!blk || !out_w || out_cap < 2u || !vgd_shm_is_valid(blk)) {
        if (out_w && out_cap > 0u) out_w[0] = 0u;
        return 0u;
    }
    vgd_memory_barrier();
    limit = (out_cap - 1u < VGD_PRODUCT_NAME_MAX - 1u) ? (out_cap - 1u) : (VGD_PRODUCT_NAME_MAX - 1u);
    while (i < limit && blk->product_name[i] != 0u) {
        out_w[i] = blk->product_name[i];
        i++;
    }
    out_w[i] = 0u;
    return i;
}
