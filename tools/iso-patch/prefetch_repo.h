#ifndef PREFETCH_REPO_H
#define PREFETCH_REPO_H

#include <windows.h>

int do_prefetch_repo(const wchar_t *tag, const wchar_t *mesa_tag,
                     const wchar_t *dxgkrnl_tag, const wchar_t *cache_dir,
                     const wchar_t *out_dir);

#endif
