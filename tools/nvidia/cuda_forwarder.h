#ifndef ASB_CUDA_FORWARDER_H
#define ASB_CUDA_FORWARDER_H

#include "cuda_package.h"

BOOL asb_cuda_prepare_package(const wchar_t *source, const wchar_t *generator, const wchar_t *cache,
                              const wchar_t *shim_dir, const wchar_t *files,
                              wchar_t output[MAX_PATH], BOOL *changed);

#endif
