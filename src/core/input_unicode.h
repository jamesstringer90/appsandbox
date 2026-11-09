#ifndef ASB_INPUT_UNICODE_H
#define ASB_INPUT_UNICODE_H

#include <stdint.h>

static unsigned asb_unicode_utf16(uint32_t scalar,uint16_t units[2])
{
    if (!scalar || scalar>0x10ffff || (scalar>=0xd800 && scalar<=0xdfff)) return 0;
    if (scalar<=0xffff) { units[0]=(uint16_t)scalar;return 1; }
    scalar-=0x10000;
    units[0]=(uint16_t)(0xd800+(scalar>>10));
    units[1]=(uint16_t)(0xdc00+(scalar&0x3ff));
    return 2;
}

#endif
