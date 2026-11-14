#pragma once

#include <windows.h>

#define CLIP_FILE_BUFFER_SIZE (1024 * 1024)

typedef struct ClipFileWriter {
    HANDLE handle;
    BYTE *buffer;
    UINT64 size;
    UINT64 written;
    DWORD alignment;
    BOOL unbuffered;
    wchar_t path[MAX_PATH];
} ClipFileWriter;

BOOL clip_file_writer_open(ClipFileWriter *file, const wchar_t *path, UINT64 size);
BOOL clip_file_writer_write(ClipFileWriter *file, DWORD length);
BOOL clip_file_writer_finish(ClipFileWriter *file);
void clip_file_writer_abort(ClipFileWriter *file);
