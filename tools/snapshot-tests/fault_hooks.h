/* Force-included (/FI) when snapshot.c is compiled for the tests, and for nothing else:
   the product carries no test code. Every file operation snapshot.c performs goes
   through a wrapper in fault_hooks.c, which can record it, fail it, or kill the
   process just before or just after it. */
#ifndef FAULT_HOOKS_H
#define FAULT_HOOKS_H

#include <windows.h>
#include <stdio.h>
#include <io.h>
#include <wchar.h>
#include "disk_util.h"

BOOL    hook_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
BOOL    hook_DeleteFileW(LPCWSTR path);
UINT    hook_GetTempFileNameW(LPCWSTR dir, LPCWSTR prefix, UINT unique, LPWSTR out);
HANDLE  hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                         DWORD disposition, DWORD flags, HANDLE templ);
BOOL    hook_FlushFileBuffers(HANDLE h);
errno_t hook_wfopen_s(FILE **f, const wchar_t *path, const wchar_t *mode);
int     hook_fwprintf(FILE *f, const wchar_t *format, ...);
int     hook_ferror(FILE *f);
int     hook_fflush(FILE *f);
int     hook_commit(int fd);
int     hook_fclose(FILE *f);
HRESULT hook_vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path);

#undef CreateFile
#define MoveFileExW              hook_MoveFileExW
#define DeleteFileW              hook_DeleteFileW
#define GetTempFileNameW         hook_GetTempFileNameW
#define CreateFileW              hook_CreateFileW
#define FlushFileBuffers         hook_FlushFileBuffers
#define _wfopen_s                hook_wfopen_s
#define fwprintf                 hook_fwprintf
#define ferror                   hook_ferror
#define fflush                   hook_fflush
#define _commit                  hook_commit
#define fclose                   hook_fclose
#define vhdx_create_differencing hook_vhdx_create_differencing

#endif /* FAULT_HOOKS_H */
