#include "cuda_forwarder.h"
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    wchar_t generator[MAX_PATH], output[MAX_PATH];
    BOOL changed;
    DWORD length;
    if (argc != 9 || wcscmp(argv[1], L"--source") || wcscmp(argv[3], L"--cache") ||
        wcscmp(argv[5], L"--shim-dir") || wcscmp(argv[7], L"--files")) {
        fwprintf(stderr, L"Usage: appsandbox-cuda-updater --source <NVIDIA DLL> --cache <directory> --shim-dir <directory> --files <file list>\n");
        return 2;
    }
    length = GetModuleFileNameW(NULL, generator, MAX_PATH);
    if (!length || length >= MAX_PATH ||
        !asb_cuda_prepare_package(argv[2], generator, argv[4], argv[6], argv[8], output, &changed)) {
        DWORD error = GetLastError();
        if (!error) error = ERROR_GEN_FAILURE;
        fwprintf(stderr, L"CUDA package preparation failed: %lu\n", error);
        return (int)error;
    }
    wprintf(L"%s: %s\n", changed ? L"Generated" : L"Unchanged", output);
    return 0;
}
