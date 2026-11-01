#ifndef ASB_NET_LIMITS_H
#define ASB_NET_LIMITS_H

#include <stddef.h>

/* asb_net_limits.h - sockets-free shared limits for network-value
 * contracts.
 *
 * Included by BOTH hcs_vm.h (VmInstance.internal_switch[]) and
 * hcn_network.h (the census entry shapes): the dependency directions
 * are VM->limits and HCN->limits, never VM->HCN. Deliberately includes
 * <stddef.h> ONLY - never windows.h, never any HCN header - so it can
 * be pulled into any translation unit without the winsock2-before-
 * windows.h ordering contract hcn_network.h carries.
 *
 * The character rule is shared by every layer that must agree on what
 * a storable internal-switch name is: the interactive entrances
 * (setter, create entry, headless edit), the loader, the census name
 * eligibility, and the offline fixtures. One rule, no drift. The
 * rejection set is derived from measured baseline-writer behavior: an
 * unpaired surrogate merges two distinct names into one U+FFFD name
 * on save, and U+FFFF truncates the value at every save with no error
 * signal - a name the baseline writer cannot
 * round-trip must never be stored. CR/LF break the line-oriented
 * vms.cfg format. */

#define INTERNAL_SWITCH_CAP 256   /* wchars, incl. terminator */

/* Non-zero when the FIRST len code units of name are storable: no
   CR/LF/NUL, no U+FFFF, and surrogate halves PAIRED. The length is a
   parameter so the WMI layer can pass SysStringLen's count (a BSTR can
   carry embedded NULs that wcslen cannot see) while the entrances pass
   wcslen's. Header-defined static inline: the host probe compiles
   hcn_wmi.c without asb_config.c, so a prototype here with a body in
   the config TU would add a link consumer; a plain static would warn
   C4505 in every non-calling TU. */
static inline int asb_internal_switch_value_chars_ok(const wchar_t *name,
                                                      size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        wchar_t c = name[i];
        if (c == L'\r' || c == L'\n' || c == 0 || c == 0xFFFF)
            return 0;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= len)
                return 0;                    /* lone high surrogate */
            if (!(name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF))
                return 0;                    /* high half not paired */
            i++;                             /* consume the pair */
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return 0;                        /* lone low surrogate */
        }
    }
    return 1;
}

#endif /* ASB_NET_LIMITS_H */
