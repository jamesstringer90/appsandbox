#include "win_file_hash.h"
#include <stdio.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s)  (((NTSTATUS)(s)) >= 0)
#endif

int prefetch_sha256_file(const wchar_t *path, char *hex_out)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD hashLen = 0, prop = 0;
    BYTE hash[32];
    int rc = -1;

    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0)))
        return -1;
    if (!NT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                                       (PBYTE)&hashLen, sizeof(hashLen), &prop, 0)))
        goto cleanup;
    if (hashLen != 32) goto cleanup;
    if (!NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0))) goto cleanup;

    hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) goto cleanup;

    BYTE buf[65536];
    DWORD br;
    while (ReadFile(hFile, buf, sizeof(buf), &br, NULL) && br > 0) {
        if (!NT_SUCCESS(BCryptHashData(hHash, buf, br, 0))) goto cleanup;
    }
    if (!NT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0))) goto cleanup;

    for (int i = 0; i < 32; i++)
        sprintf_s(hex_out + i * 2, 3, "%02x", hash[i]);
    hex_out[64] = 0;
    rc = 0;

cleanup:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return rc;
}

static BYTE sha256_hex_digit(char digit)
{
    return (BYTE)(digit <= '9' ? digit - '0' : digit - 'a' + 10);
}

BOOL asb_file_sha256(const wchar_t *path, BYTE digest[ASB_SHA256_SIZE])
{
    char hex[2 * ASB_SHA256_SIZE + 1];
    int i;
    SetLastError(ERROR_SUCCESS);
    if (prefetch_sha256_file(path, hex) != 0) {
        if (!GetLastError()) SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    for (i = 0; i < ASB_SHA256_SIZE; i++)
        digest[i] = (BYTE)((sha256_hex_digit(hex[2 * i]) << 4) | sha256_hex_digit(hex[2 * i + 1]));
    return TRUE;
}
