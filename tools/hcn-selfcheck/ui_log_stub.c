/* ui_log_stub.c - local stand-in for the core DLL's ui_log.
 *
 * hcn_network.c logs through ui_log; the lines are mirrored to the
 * console so unexpected paths in the production source are visible in
 * the self-check output. Mirrors the production implementation's
 * _TRUNCATE formatting (asb_core.c). Compiled by both hcn-selfcheck
 * projects so the stub stays single-sourced. */

#include <winsock2.h>
#include <stdio.h>
#include <stdarg.h>
#include "ui.h"

void ui_log(const wchar_t *fmt, ...)
{
    va_list ap;
    wchar_t buf[4096];

    va_start(ap, fmt);
    _vsnwprintf_s(buf, (size_t)sizeof(buf) / sizeof(buf[0]), _TRUNCATE, fmt, ap);
    va_end(ap);
    wprintf(L"[hcn] %s\n", buf);
}
