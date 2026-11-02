#ifndef ASB_VM_ID_H
#define ASB_VM_ID_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <stdlib.h>
#endif

typedef uint64_t AsbVmId;
#define ASB_VM_ID_TEXT_SIZE 20

static inline void asb_vm_id_format(AsbVmId id, char out[ASB_VM_ID_TEXT_SIZE]) {
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "vm-", 3);
    for (int i = 0; i < 16; ++i) out[3 + i] = hex[(id >> (60 - i * 4)) & 15];
    out[19] = 0;
}

static inline AsbVmId asb_vm_id_parse(const char *text) {
    AsbVmId id = 0;
    if (!text || strlen(text) != 19 || memcmp(text, "vm-", 3)) return 0;
    for (int i = 3; i < 19; ++i) {
        unsigned c = (unsigned char)text[i];
        if (c >= '0' && c <= '9') c -= '0';
        else if (c >= 'a' && c <= 'f') c -= 'a' - 10;
        else return 0;
        id = (id << 4) | c;
    }
    return id;
}

static inline void asb_vm_id_format_w(AsbVmId id, wchar_t out[ASB_VM_ID_TEXT_SIZE]) {
    char text[ASB_VM_ID_TEXT_SIZE];
    asb_vm_id_format(id, text);
    for (int i = 0; i < ASB_VM_ID_TEXT_SIZE; ++i) out[i] = text[i];
}

static inline AsbVmId asb_vm_id_parse_w(const wchar_t *text) {
    char narrow[ASB_VM_ID_TEXT_SIZE];
    if (!text || wcslen(text) != 19) return 0;
    for (int i = 0; i < 20; ++i) {
        if ((unsigned)text[i] > 127) return 0;
        narrow[i] = (char)text[i];
    }
    return asb_vm_id_parse(narrow);
}

static inline AsbVmId asb_vm_id_new(void) {
    AsbVmId id = 0;
    do {
#ifdef _WIN32
        if (BCryptGenRandom(NULL, (PUCHAR)&id, sizeof(id), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
            return 0;
#else
        arc4random_buf(&id, sizeof(id));
#endif
    } while (!id);
    return id;
}

static inline int asb_vm_ids_ensure(AsbVmId *ids, size_t count) {
    int changed = 0;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (ids[i] == ids[j]) { ids[i] = 0; break; }
        }
        if (ids[i]) continue;
        for (;;) {
            AsbVmId candidate = asb_vm_id_new();
            size_t j;
            if (!candidate) return -1;
            for (j = 0; j < count && ids[j] != candidate; ++j) {}
            if (j == count) { ids[i] = candidate; changed = 1; break; }
        }
    }
    return changed;
}
#endif
