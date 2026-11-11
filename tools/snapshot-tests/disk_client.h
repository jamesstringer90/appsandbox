/* The tests' side of the elevated disk helper. */
#ifndef DISK_CLIENT_H
#define DISK_CLIENT_H

#include "disk_protocol.h"

/* The call itself, 0 or a Win32 error. A helper that is not running gives ERROR_FILE_NOT_FOUND. */
DWORD disk_call(const HelperRequest *req, HelperReply *rep);

/* Each returns 0 when the helper did it, otherwise the Win32 error. A request the helper could
   not be asked at all is fatal: it means the helper is gone, not that a test failed. */
DWORD disk_write(const wchar_t *vhdx, ULONGLONG offset, const char *text, HelperReply *rep);
DWORD disk_read(const wchar_t *vhdx, ULONGLONG offset, char *text, size_t text_len, HelperReply *rep);
DWORD disk_read_many(const wchar_t *vhdx, const ULONGLONG *offset, DWORD count, HelperReply *rep);
DWORD disk_try_attach(const wchar_t *vhdx, BOOL read_only, HelperReply *rep);
DWORD disk_hold(const wchar_t *vhdx, BOOL read_only, HelperReply *rep);
DWORD disk_release(HelperReply *rep);
DWORD disk_guard_probe(const wchar_t *vhdx, DWORD drive, DWORD skip, HelperReply *rep);

/* Runs the helper's own checks. Prints what it did, returns the number that failed. */
int disk_helper_selftest(const wchar_t *folder);

#endif /* DISK_CLIENT_H */
