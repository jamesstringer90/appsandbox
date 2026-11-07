/* What the test program asks the elevated disk helper to do, over a named pipe.
   Attaching a virtual disk is the only thing these tests need admin rights for,
   so it is the only thing that runs elevated. See README.md. */
#ifndef DISK_PROTOCOL_H
#define DISK_PROTOCOL_H

#include <windows.h>

#define HELPER_PIPE_NAME   L"\\\\.\\pipe\\asb-snapshot-tests-disk-helper"
#define HELPER_VERSION     1

/* The helper refuses any disk that is not exactly this size. */
#define HELPER_DISK_SIZE   (64ULL * 1024 * 1024)
#define HELPER_SECTOR      512
#define HELPER_MAX_RECORDS 32
#define HELPER_RECORD_SIZE 64
#define HELPER_MAX_PATH    1024

enum {
    OP_PING = 1,
    OP_WRITE,        /* attach, write each record to its sector, flush, detach */
    OP_READ,         /* attach read-only, read each record, detach */
    OP_TRY_ATTACH,   /* attach, detach, report the result */
    OP_HOLD,         /* attach and stay attached until OP_RELEASE (one at a time) */
    OP_RELEASE,
    OP_GUARD_PROBE,  /* run the raw-disk guard against a drive number. Cannot write. */
    OP_QUIT
};

#define HELPER_FLAG_READ_ONLY 1

/* Only OP_GUARD_PROBE honours these. They let a test show that each guard check
   refuses a real drive by itself, with no way to reach a write. */
#define GUARD_SKIP_REAL   1
#define GUARD_SKIP_NUMBER 2
#define GUARD_SKIP_BUS    4
#define GUARD_SKIP_LENGTH 8

enum {
    STAGE_NONE = 0,
    STAGE_REQUEST,       /* malformed request */
    STAGE_PATH,          /* outside the test folder, or not a .vhdx */
    STAGE_OPEN,
    STAGE_SIZE,          /* not a test-sized disk */
    STAGE_ATTACH,
    STAGE_PHYSICAL,
    STAGE_GUARD_REAL,    /* the drive existed when the helper started */
    STAGE_GUARD_NUMBER,  /* the device is not the drive it was named as */
    STAGE_GUARD_BUS,     /* not a file-backed virtual disk */
    STAGE_GUARD_LENGTH,  /* not the size of a test disk */
    STAGE_GUARD_BACKING, /* its backing file is not the disk we attached */
    STAGE_IO
};

typedef struct {
    DWORD     op;
    DWORD     flags;
    DWORD     count;                      /* records in offset[] and data[] */
    DWORD     probe_drive;                /* OP_GUARD_PROBE */
    DWORD     probe_skip;                 /* OP_GUARD_PROBE */
    ULONGLONG offset[HELPER_MAX_RECORDS]; /* sector-aligned */
    BYTE      data[HELPER_MAX_RECORDS][HELPER_RECORD_SIZE];
    wchar_t   path[HELPER_MAX_PATH];
} HelperRequest;

typedef struct {
    DWORD   error;                        /* Win32 error, 0 = done */
    DWORD   stage;                        /* where it stopped */
    DWORD   version;                      /* OP_PING */
    BYTE    data[HELPER_MAX_RECORDS][HELPER_RECORD_SIZE];
    wchar_t text[HELPER_MAX_PATH];        /* detail for a human, or the root on OP_PING */
} HelperReply;

#endif /* DISK_PROTOCOL_H */
