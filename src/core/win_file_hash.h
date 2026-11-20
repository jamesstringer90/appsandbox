#ifndef ASB_WIN_FILE_HASH_H
#define ASB_WIN_FILE_HASH_H

#include <windows.h>
#include <bcrypt.h>
#include <string.h>
#pragma comment(lib, "bcrypt.lib")

#define ASB_SHA256_SIZE 32

typedef struct {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
} AsbSha256;

static inline void asb_sha256_close(AsbSha256 *state)
{
    if (state->hash) BCryptDestroyHash(state->hash);
    if (state->algorithm) BCryptCloseAlgorithmProvider(state->algorithm, 0);
    memset(state, 0, sizeof(*state));
}

static inline BOOL asb_sha256_begin(AsbSha256 *state)
{
    memset(state, 0, sizeof(*state));
    if (BCryptOpenAlgorithmProvider(&state->algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
        BCryptCreateHash(state->algorithm, &state->hash, NULL, 0, NULL, 0, 0) >= 0)
        return TRUE;
    asb_sha256_close(state);
    SetLastError(ERROR_GEN_FAILURE);
    return FALSE;
}

static inline BOOL asb_sha256_update(AsbSha256 *state, const BYTE *data, DWORD size)
{
    if (BCryptHashData(state->hash, (PUCHAR)data, size, 0) >= 0) return TRUE;
    SetLastError(ERROR_GEN_FAILURE);
    return FALSE;
}

static inline BOOL asb_sha256_finish(AsbSha256 *state, BYTE digest[ASB_SHA256_SIZE])
{
    BOOL ok = BCryptFinishHash(state->hash, digest, ASB_SHA256_SIZE, 0) >= 0;
    asb_sha256_close(state);
    if (!ok) SetLastError(ERROR_GEN_FAILURE);
    return ok;
}

BOOL asb_file_sha256(const wchar_t *path, BYTE digest[ASB_SHA256_SIZE]);

static inline BOOL asb_files_sha256_equal(const wchar_t *a, const wchar_t *b)
{
    WIN32_FILE_ATTRIBUTE_DATA aa, bb;
    BYTE ah[ASB_SHA256_SIZE], bh[ASB_SHA256_SIZE];
    return GetFileAttributesExW(a, GetFileExInfoStandard, &aa) &&
        GetFileAttributesExW(b, GetFileExInfoStandard, &bb) &&
        !((aa.dwFileAttributes | bb.dwFileAttributes) & FILE_ATTRIBUTE_DIRECTORY) &&
        aa.nFileSizeHigh == bb.nFileSizeHigh && aa.nFileSizeLow == bb.nFileSizeLow &&
        asb_file_sha256(a, ah) && asb_file_sha256(b, bh) && !memcmp(ah, bh, sizeof(ah));
}

#endif
