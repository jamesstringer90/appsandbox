#ifndef ASB_CUDA_PACKAGE_H
#define ASB_CUDA_PACKAGE_H

#include "../../src/core/win_file_hash.h"

#define ASB_CUDA_SHIM L"appsandbox-nvidia-cuda-shim.dll"
#define ASB_CUDA_CACHE_HASH L".cuda-share-sha256"

BOOL asb_cuda_package_id(const wchar_t *driver, const wchar_t *generator,
                         const wchar_t *directory, const wchar_t *files, wchar_t id[65]);
BOOL asb_cuda_share_hash(const wchar_t *directory, const wchar_t *files, BYTE digest[ASB_SHA256_SIZE]);
BOOL asb_cuda_package_valid(const wchar_t *directory, const wchar_t *files);
BOOL asb_cuda_package_record(const wchar_t *directory, const wchar_t *files);
BOOL asb_cuda_copy_shims(const wchar_t *directory, const wchar_t *target, const wchar_t *files);
void asb_cuda_remove_package(const wchar_t *directory, const wchar_t *files);

#endif
