#include "cuda_forwarder.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef struct {
    BYTE *bytes;
    DWORD size;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_EXPORT_DIRECTORY exports;
    const IMAGE_SECTION_HEADER *sections;
    const BYTE *functions;
    const BYTE *names;
    const BYTE *ordinals;
} CudaExports;

typedef struct { const char *name; WORD index; } CudaName;

static const char *const cuda_hooks[] = { "cuDeviceGetLuid", "cuGetProcAddress", "cuGetProcAddress_v2" };

static DWORD read_dword(const BYTE *p)
{
    DWORD value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static WORD read_word(const BYTE *p)
{
    WORD value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static const BYTE *rva_bytes(const CudaExports *image, DWORD rva, DWORD *available)
{
    UINT i;
    if (rva < image->nt.OptionalHeader.SizeOfHeaders) {
        *available = image->nt.OptionalHeader.SizeOfHeaders - rva;
        return image->bytes + rva;
    }
    for (i = 0; i < image->nt.FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *section = &image->sections[i];
        if (rva >= section->VirtualAddress && rva - section->VirtualAddress < section->SizeOfRawData) {
            DWORD offset = rva - section->VirtualAddress;
            *available = section->SizeOfRawData - offset;
            return image->bytes + section->PointerToRawData + offset;
        }
    }
    return NULL;
}

static const BYTE *rva_span(const CudaExports *image, DWORD rva, DWORD size)
{
    DWORD available;
    const BYTE *p = rva_bytes(image, rva, &available);
    return p && size <= available ? p : NULL;
}

static const char *export_name(const CudaExports *image, DWORD index)
{
    DWORD available = 0;
    const BYTE *p = rva_bytes(image, read_dword(image->names + index * 4), &available);
    if (available > 4096) available = 4096;
    return p && memchr(p, 0, available) ? (const char *)p : NULL;
}

static int export_index(const CudaExports *image, const char *name)
{
    DWORD i;
    for (i = 0; i < image->exports.NumberOfNames; i++)
        if (!strcmp(export_name(image, i), name)) return read_word(image->ordinals + i * 2);
    return -1;
}

static BOOL read_exports(const wchar_t *path, CudaExports *image)
{
    HANDLE file;
    LARGE_INTEGER size;
    IMAGE_DOS_HEADER dos;
    DWORD read, section_offset, i, j;
    IMAGE_DATA_DIRECTORY directory;
    const BYTE *data;
    const char *previous = NULL;
    BOOL ok;

    memset(image, 0, sizeof(*image));
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = GetFileSizeEx(file, &size) && size.QuadPart >= sizeof(dos) && size.QuadPart <= 128 * 1024 * 1024;
    if (ok) image->bytes = malloc((size_t)size.QuadPart);
    ok = ok && image->bytes && ReadFile(file, image->bytes, (DWORD)size.QuadPart, &read, NULL) &&
         read == (DWORD)size.QuadPart;
    CloseHandle(file);
    if (!ok) goto invalid;
    image->size = (DWORD)size.QuadPart;
    memcpy(&dos, image->bytes, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof(dos) ||
        (DWORD)dos.e_lfanew > image->size || sizeof(image->nt) > image->size - (DWORD)dos.e_lfanew)
        goto invalid;
    memcpy(&image->nt, image->bytes + dos.e_lfanew, sizeof(image->nt));
    if (image->nt.Signature != IMAGE_NT_SIGNATURE || image->nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(image->nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        image->nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        image->nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        image->nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
        !image->nt.FileHeader.NumberOfSections || image->nt.FileHeader.NumberOfSections > 96 ||
        image->nt.OptionalHeader.SizeOfHeaders > image->size ||
        image->nt.OptionalHeader.SizeOfImage < image->nt.OptionalHeader.SizeOfHeaders)
        goto invalid;
    section_offset = (DWORD)dos.e_lfanew + sizeof(image->nt);
    if (section_offset % sizeof(DWORD) || section_offset > image->size ||
        image->nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > image->size - section_offset ||
        section_offset + image->nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) >
            image->nt.OptionalHeader.SizeOfHeaders)
        goto invalid;
    image->sections = (const IMAGE_SECTION_HEADER *)(image->bytes + section_offset);
    for (i = 0; i < image->nt.FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *section = &image->sections[i];
        DWORD extent = max(section->Misc.VirtualSize, section->SizeOfRawData);
        if (section->PointerToRawData > image->size ||
            section->SizeOfRawData > image->size - section->PointerToRawData ||
            section->VirtualAddress < image->nt.OptionalHeader.SizeOfHeaders ||
            extent > MAXDWORD - section->VirtualAddress ||
            section->VirtualAddress + extent > image->nt.OptionalHeader.SizeOfImage)
            goto invalid;
        for (j = 0; j < i; j++) {
            const IMAGE_SECTION_HEADER *other = &image->sections[j];
            DWORD other_extent = max(other->Misc.VirtualSize, other->SizeOfRawData);
            if (extent && other_extent && section->VirtualAddress < other->VirtualAddress + other_extent &&
                other->VirtualAddress < section->VirtualAddress + extent)
                goto invalid;
        }
    }
    directory = image->nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    data = rva_span(image, directory.VirtualAddress, directory.Size);
    if (!directory.VirtualAddress || directory.Size < sizeof(image->exports) || !data) goto invalid;
    memcpy(&image->exports, data, sizeof(image->exports));
    if (!image->exports.Base || image->exports.Base >= 65535 || !image->exports.NumberOfFunctions ||
        image->exports.NumberOfFunctions > 65535 - image->exports.Base ||
        !image->exports.NumberOfNames || image->exports.NumberOfNames > 65535)
        goto invalid;
    image->functions = rva_span(image, image->exports.AddressOfFunctions, image->exports.NumberOfFunctions * 4);
    image->names = rva_span(image, image->exports.AddressOfNames, image->exports.NumberOfNames * 4);
    image->ordinals = rva_span(image, image->exports.AddressOfNameOrdinals, image->exports.NumberOfNames * 2);
    if (!image->functions || !image->names || !image->ordinals) goto invalid;
    for (i = 0; i < image->exports.NumberOfFunctions; i++) {
        DWORD rva = read_dword(image->functions + i * 4);
        if (rva >= image->nt.OptionalHeader.SizeOfImage) goto invalid;
    }
    for (i = 0; i < image->exports.NumberOfNames; i++) {
        const char *name = export_name(image, i);
        WORD ordinal = read_word(image->ordinals + i * 2);
        if (!name || !*name || ordinal >= image->exports.NumberOfFunctions ||
            (previous && strcmp(previous, name) >= 0)) goto invalid;
        previous = name;
    }
    return TRUE;
invalid:
    free(image->bytes);
    memset(image, 0, sizeof(*image));
    SetLastError(ERROR_BAD_EXE_FORMAT);
    return FALSE;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(((const CudaName *)a)->name, ((const CudaName *)b)->name);
}

static DWORD append_string(BYTE *data, DWORD section_rva, DWORD *used, const char *text)
{
    DWORD rva = section_rva + *used;
    size_t length = strlen(text) + 1;
    memcpy(data + *used, text, length);
    *used += (DWORD)length;
    return rva;
}

static DWORD local_export(const CudaExports *shim, const char *name)
{
    DWORD rva, i;
    int index = export_index(shim, name);
    if (index < 0) return 0;
    rva = read_dword(shim->functions + index * 4);
    for (i = 0; i < shim->nt.FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *section = &shim->sections[i];
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section->VirtualAddress &&
            rva - section->VirtualAddress < section->SizeOfRawData)
            return rva;
    }
    return 0;
}

static BOOL generate_shim(const wchar_t *source, const wchar_t *template_path, const wchar_t *output)
{
    static const char vendor[] = "appsandbox-nvcuda.";
    CudaExports original, shim;
    BYTE *data = NULL, *file = NULL;
    CudaName *sorted = NULL;
    const char **routes = NULL;
    DWORD functions_count, names_count, i, used, written;
    DWORD file_align, section_align, raw_size, raw_offset, section_rva, file_end, file_size;
    DWORD local_rvas[ARRAYSIZE(cuda_hooks)], local_slots[ARRAYSIZE(cuda_hooks)], marker_rva;
    size_t capacity;
    IMAGE_EXPORT_DIRECTORY *exports;
    DWORD *functions, *names;
    WORD *ordinals;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *section;
    IMAGE_DATA_DIRECTORY certificate;
    HANDLE target = INVALID_HANDLE_VALUE;
    BOOL ok = FALSE;
    DWORD error = ERROR_BAD_EXE_FORMAT;
    int init;

    if (!read_exports(source, &original)) return FALSE;
    if (!read_exports(template_path, &shim)) {
        error = GetLastError();
        goto done;
    }
    init = export_index(&original, "cuInit");
    marker_rva = local_export(&shim, "appsandbox_cuda");
    if (!marker_rva || export_index(&original, "appsandbox_cuda") >= 0 || init < 0 ||
        !read_dword(original.functions + init * 4)) goto done;

    section_rva = shim.nt.OptionalHeader.SizeOfImage;
    section_align = shim.nt.OptionalHeader.SectionAlignment;
    file_align = shim.nt.OptionalHeader.FileAlignment;
    if (file_align < 512 || file_align > 65536 || (file_align & (file_align - 1)) ||
        section_align < file_align || (section_align & (section_align - 1)) ||
        (section_rva & (section_align - 1)) || shim.nt.FileHeader.NumberOfSections >= 96 ||
        shim.nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_SECURITY)
        goto done;
    {
        const BYTE *next_section = (const BYTE *)(shim.sections + shim.nt.FileHeader.NumberOfSections);
        if ((size_t)(next_section - shim.bytes) + sizeof(*section) > shim.nt.OptionalHeader.SizeOfHeaders)
            goto done;
        for (i = 0; i < sizeof(*section); i++)
            if (next_section[i]) goto done;
    }

    file_end = shim.nt.OptionalHeader.SizeOfHeaders;
    for (i = 0; i < shim.nt.FileHeader.NumberOfSections; i++)
        file_end = max(file_end, shim.sections[i].PointerToRawData + shim.sections[i].SizeOfRawData);
    certificate = shim.nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
    if (certificate.Size) {
        if (certificate.VirtualAddress < file_end || certificate.VirtualAddress > shim.size ||
            certificate.Size != shim.size - certificate.VirtualAddress)
            goto done;
        file_end = certificate.VirtualAddress;
    } else {
        if (certificate.VirtualAddress) goto done;
        file_end = shim.size;
    }

    for (i = 0; i < ARRAYSIZE(cuda_hooks); i++) {
        int index = export_index(&original, cuda_hooks[i]);
        DWORD j;
        if (index < 0 || !read_dword(original.functions + index * 4)) goto done;
        local_slots[i] = (DWORD)index;
        local_rvas[i] = local_export(&shim, cuda_hooks[i]);
        if (!local_rvas[i]) goto done;
        for (j = 0; j < i; j++)
            if (local_slots[i] == local_slots[j]) goto done;
    }

    functions_count = original.exports.NumberOfFunctions + 1;
    names_count = original.exports.NumberOfNames + 1;
    capacity = sizeof(*exports) + (size_t)functions_count * (4 + sizeof(vendor) + 8) +
        (size_t)names_count * 6 + 32;
    for (i = 0; i < names_count - 1; i++) capacity += 2 * (strlen(export_name(&original, i)) + 1);
    if (capacity > 64 * 1024 * 1024 || capacity > MAXDWORD - section_rva) goto done;
    data = calloc(1, capacity);
    sorted = calloc(names_count, sizeof(*sorted));
    routes = calloc(functions_count, sizeof(*routes));
    if (!data || !sorted || !routes) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    for (i = 0; i < names_count - 1; i++) {
        sorted[i].name = export_name(&original, i);
        sorted[i].index = read_word(original.ordinals + i * 2);
        if (!routes[sorted[i].index]) routes[sorted[i].index] = sorted[i].name;
    }
    sorted[names_count - 1].name = "appsandbox_cuda";
    sorted[names_count - 1].index = (WORD)(functions_count - 1);
    qsort(sorted, names_count, sizeof(*sorted), compare_names);

    exports = (IMAGE_EXPORT_DIRECTORY *)data;
    exports->Base = original.exports.Base;
    exports->NumberOfFunctions = functions_count;
    exports->NumberOfNames = names_count;
    used = sizeof(*exports);
    exports->AddressOfFunctions = section_rva + used;
    functions = (DWORD *)(data + used); used += functions_count * 4;
    exports->AddressOfNames = section_rva + used;
    names = (DWORD *)(data + used); used += names_count * 4;
    exports->AddressOfNameOrdinals = section_rva + used;
    ordinals = (WORD *)(data + used); used += names_count * 2;
    exports->Name = append_string(data, section_rva, &used, "nvcuda.dll");
    for (i = 0; i < ARRAYSIZE(cuda_hooks); i++) functions[local_slots[i]] = local_rvas[i];
    functions[functions_count - 1] = marker_rva;
    for (i = 0; i < functions_count - 1; i++) {
        char forward[4096 + sizeof(vendor)];
        int length;
        if (functions[i] || !read_dword(original.functions + i * 4)) continue;
        if (routes[i]) length = sprintf_s(forward, sizeof(forward), "%s%s", vendor, routes[i]);
        else length = sprintf_s(forward, sizeof(forward), "%s#%lu", vendor, exports->Base + i);
        if (length < 0) goto done;
        functions[i] = append_string(data, section_rva, &used, forward);
    }
    for (i = 0; i < names_count; i++) {
        names[i] = append_string(data, section_rva, &used, sorted[i].name);
        ordinals[i] = sorted[i].index;
    }

    raw_offset = (file_end + file_align - 1) & ~(file_align - 1);
    raw_size = (used + file_align - 1) & ~(file_align - 1);
    file_size = raw_offset + raw_size;
    if ((ULONGLONG)section_rva + raw_size + section_align - 1 > MAXDWORD ||
        raw_size > MAXDWORD - shim.nt.OptionalHeader.SizeOfInitializedData)
        goto done;
    file = calloc(1, file_size);
    if (!file) { error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    memcpy(file, shim.bytes, file_end);
    memcpy(file + raw_offset, data, used);
    nt = (IMAGE_NT_HEADERS64 *)(file + ((const IMAGE_DOS_HEADER *)file)->e_lfanew);
    section = IMAGE_FIRST_SECTION(nt) + nt->FileHeader.NumberOfSections++;
    memcpy(section->Name, ".asbcuda", IMAGE_SIZEOF_SHORT_NAME);
    section->VirtualAddress = section_rva;
    section->Misc.VirtualSize = used;
    section->PointerToRawData = raw_offset;
    section->SizeOfRawData = raw_size;
    section->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    nt->OptionalHeader.SizeOfInitializedData += raw_size;
    nt->OptionalHeader.SizeOfImage = (section_rva + raw_size + section_align - 1) & ~(section_align - 1);
    nt->OptionalHeader.CheckSum = 0;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY] = (IMAGE_DATA_DIRECTORY){0};
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = (IMAGE_DATA_DIRECTORY){section_rva, used};

    target = CreateFileW(output, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (target == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    ok = WriteFile(target, file, file_size, &written, NULL) && written == file_size && FlushFileBuffers(target);
    if (!ok) error = GetLastError();
done:
    if (target != INVALID_HANDLE_VALUE) {
        CloseHandle(target);
        if (!ok) DeleteFileW(output);
    }
    free(file);
    free(data);
    free(sorted);
    free(routes);
    free(shim.bytes);
    free(original.bytes);
    if (!ok) SetLastError(error ? error : ERROR_GEN_FAILURE);
    return ok;
}

BOOL asb_cuda_prepare_package(const wchar_t *source, const wchar_t *generator, const wchar_t *cache,
                              const wchar_t *shim_dir, const wchar_t *files,
                              wchar_t output[MAX_PATH], BOOL *changed)
{
    wchar_t id[65], final_id[65], template_path[MAX_PATH], path[MAX_PATH], lock_path[MAX_PATH];
    wchar_t temporary[MAX_PATH] = {0}, previous[MAX_PATH] = {0};
    HANDLE lock = INVALID_HANDLE_VALUE;
    ULONGLONG deadline;
    BOOL ok = FALSE;
    DWORD error = ERROR_GEN_FAILURE;

    *changed = FALSE;
    if (!asb_cuda_package_id(source, generator, shim_dir, files, id) ||
        swprintf_s(output, MAX_PATH, L"%s\\%s", cache, id) < 0 ||
        swprintf_s(template_path, MAX_PATH, L"%s\\%s", shim_dir, ASB_CUDA_SHIM) < 0 ||
        swprintf_s(lock_path, MAX_PATH, L"%s\\.cuda-update-lock", cache) < 0)
        return FALSE;
    if (asb_cuda_package_valid(output, files)) return TRUE;
    if (!CreateDirectoryW(cache, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    deadline = GetTickCount64() + 8000;
    do {
        lock = CreateFileW(lock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (lock != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION || GetTickCount64() >= deadline) return FALSE;
        Sleep(50);
    } while (TRUE);
    if (asb_cuda_package_valid(output, files)) { ok = TRUE; goto done; }
    if (!GetTempFileNameW(cache, L"acu", 0, temporary) ||
        !DeleteFileW(temporary) || !CreateDirectoryW(temporary, NULL) ||
        !asb_cuda_copy_shims(shim_dir, temporary, files) ||
        swprintf_s(path, MAX_PATH, L"%s\\%s", temporary, ASB_CUDA_SHIM) < 0 ||
        !generate_shim(source, template_path, path) ||
        !asb_cuda_package_record(temporary, files) ||
        !asb_cuda_package_id(source, generator, shim_dir, files, final_id)) goto failed;
    if (wcscmp(id, final_id)) {
        error = ERROR_RETRY;
        goto done;
    }
    if (GetFileAttributesW(output) != INVALID_FILE_ATTRIBUTES) {
        if (!GetTempFileNameW(cache, L"old", 0, previous) || !DeleteFileW(previous) ||
            !MoveFileExW(output, previous, MOVEFILE_WRITE_THROUGH)) goto failed;
    }
    if (!MoveFileExW(temporary, output, MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        if (previous[0]) MoveFileExW(previous, output, MOVEFILE_WRITE_THROUGH);
        previous[0] = 0;
        goto done;
    }
    temporary[0] = 0;
    *changed = TRUE;
    ok = TRUE;
    goto done;
failed:
    error = GetLastError();
done:
    if (temporary[0]) asb_cuda_remove_package(temporary, files);
    if (previous[0]) asb_cuda_remove_package(previous, files);
    CloseHandle(lock);
    if (!ok) SetLastError(error ? error : ERROR_GEN_FAILURE);
    return ok;
}
