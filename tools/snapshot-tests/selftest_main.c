/* Checks the disk helper by itself: its fences, and that a non-admin program can use it. */
#include <stdio.h>
#include <windows.h>
#include "disk_client.h"

int wmain(int argc, wchar_t **argv)
{
    if (argc != 2) {
        wprintf(L"usage: helper-selftest <test folder>\n");
        return 2;
    }
    return disk_helper_selftest(argv[1]) == 0 ? 0 : 1;
}
