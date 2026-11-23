#ifndef ASB_EDID_H
#define ASB_EDID_H

#include <string.h>

/* One base block and up to 255 extensions. A zero size selects Built-in. */
#define ASB_EDID_MAX_BYTES (256u * 128u)
typedef struct {
    unsigned int size;
    unsigned char bytes[ASB_EDID_MAX_BYTES];
} AsbEdid;

static inline int asb_edid_valid(const unsigned char *data, unsigned int size)
{
    static const unsigned char header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (!data || size < 128 || size != (1u + data[126]) * 128 ||
        memcmp(data, header, sizeof(header))) return 0;
    for (unsigned int block = 0; block < size; block += 128) {
        unsigned char sum = 0;
        for (unsigned int i = 0; i < 128; ++i) sum += data[block + i];
        if (sum) return 0;
    }
    return 1;
}

#endif
