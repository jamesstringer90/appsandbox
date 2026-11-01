#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201)
#include <d3dkmthk.h>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <aclapi.h>
#include <winioctl.h>
#include <winternl.h>
#pragma warning(pop)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "gl_vk_provision.h"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

BOOL gpu_prefers_system_opengl(void)
{
#if defined(_M_X64)
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    PFND3DKMT_ENUMADAPTERS2 enumerate;
    PFND3DKMT_QUERYADAPTERINFO query;
    PFND3DKMT_CLOSEADAPTER close;
    D3DKMT_ADAPTERINFO *adapters = NULL;
    D3DKMT_ENUMADAPTERS2 list = { 0 };
    ULONG i, capacity = 0;
    BOOL found = FALSE;

    if (!gdi) return FALSE;
    enumerate = (PFND3DKMT_ENUMADAPTERS2)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    query = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    close = (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!enumerate || !query || !close || enumerate(&list) < 0 ||
        !list.NumAdapters) goto done;
    capacity = list.NumAdapters;
    adapters = calloc(capacity, sizeof(*adapters));
    if (!adapters) goto done;
    list.pAdapters = adapters;
    if (enumerate(&list) < 0 || list.NumAdapters > capacity) goto cleanup;
    for (i = 0; i < list.NumAdapters; i++) {
        D3DKMT_QUERY_DEVICE_IDS ids = { 0 };
        D3DKMT_ADAPTERTYPE type = { 0 };
        D3DKMT_QUERYADAPTERINFO info = { adapters[i].hAdapter,
            KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids) };

        if (query(&info) < 0 || (ids.DeviceIds.VendorID != 0x1002 &&
                                ids.DeviceIds.VendorID != 0x8086)) continue;
        info.Type = KMTQAITYPE_ADAPTERTYPE_RENDER;
        info.pPrivateDriverData = &type;
        info.PrivateDriverDataSize = sizeof(type);
        if (query(&info) >= 0 && type.Paravirtualized) {
            found = TRUE;
            break;
        }
    }
cleanup:
    for (i = 0; i < capacity; i++) {
        if (adapters[i].hAdapter) {
            D3DKMT_CLOSEADAPTER closing = { adapters[i].hAdapter };
            close(&closing);
        }
    }
done:
    free(adapters);
    FreeLibrary(gdi);
    return found;
#else
    return FALSE;
#endif
}

static BOOL files_equal(const wchar_t *a, const wchar_t *b)
{
    HANDLE fa, fb;
    LARGE_INTEGER sa, sb;
    BYTE ba[16384], bb[16384];
    DWORD na, nb;
    BOOL equal = FALSE;

    fa = CreateFileW(a, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    fb = CreateFileW(b, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fa == INVALID_HANDLE_VALUE || fb == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(fa, &sa) || !GetFileSizeEx(fb, &sb) ||
        sa.QuadPart != sb.QuadPart) goto done;
    do {
        if (!ReadFile(fa, ba, sizeof(ba), &na, NULL) ||
            !ReadFile(fb, bb, sizeof(bb), &nb, NULL) ||
            na != nb || memcmp(ba, bb, na)) goto done;
    } while (na);
    equal = TRUE;
done:
    if (fa != INVALID_HANDLE_VALUE) CloseHandle(fa);
    if (fb != INVALID_HANDLE_VALUE) CloseHandle(fb);
    return equal;
}

static BOOL replace_file(const wchar_t *src, const wchar_t *dst)
{
    wchar_t folder[MAX_PATH], staged[MAX_PATH], previous[MAX_PATH], *slash;
    BOOL had_previous = FALSE, result = FALSE;
    DWORD error = ERROR_SUCCESS;

    if (files_equal(src, dst)) return TRUE;
    if (wcscpy_s(folder, MAX_PATH, dst)) return FALSE;
    slash = wcsrchr(folder, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    if (!GetTempFileNameW(folder, L"asb", 0, staged)) return FALSE;
    if (!CopyFileW(src, staged, FALSE) || !files_equal(src, staged)) goto done;
    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
        if (!GetTempFileNameW(folder, L"asb", 0, previous)) goto done;
        DeleteFileW(previous);
        if (!MoveFileExW(dst, previous, MOVEFILE_WRITE_THROUGH)) goto done;
        had_previous = TRUE;
    }
    result = MoveFileExW(staged, dst, MOVEFILE_WRITE_THROUGH);
    error = GetLastError();
    if (!result && had_previous) {
        if (!MoveFileExW(previous, dst, MOVEFILE_WRITE_THROUGH))
            CopyFileW(previous, dst, TRUE);
        had_previous = FALSE;
    }
    if (result && had_previous && !DeleteFileW(previous))
        MoveFileExW(previous, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
done:
    if (!result && !error) error = GetLastError();
    DeleteFileW(staged);
    if (!result) SetLastError(error);
    return result;
}

static BOOL verify_microsoft_signature(WINTRUST_DATA *trust)
{
    static const GUID verify_policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    CRYPT_PROVIDER_DATA *provider;
    CRYPT_PROVIDER_SGNR *signer;
    wchar_t name[256];
    BOOL result = FALSE;

    trust->dwUIChoice = WTD_UI_NONE;
    trust->fdwRevocationChecks = WTD_REVOKE_NONE;
    trust->dwStateAction = WTD_STATEACTION_VERIFY;
    trust->dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    if (WinVerifyTrust(NULL, (GUID *)&verify_policy, trust) == ERROR_SUCCESS) {
        provider = WTHelperProvDataFromStateData(trust->hWVTStateData);
        signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : NULL;
        if (signer && signer->csCertChain &&
            CertGetNameStringW(signer->pasCertChain[0].pCert,
                               CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256))
            result = !_wcsicmp(name, L"Microsoft Windows") ||
                     !_wcsicmp(name, L"Microsoft Corporation");
    }
    trust->dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, (GUID *)&verify_policy, trust);
    return result;
}

static BOOL is_microsoft_opengl(const wchar_t *path)
{
    WINTRUST_FILE_INFO file = { sizeof(file) };
    WINTRUST_DATA trust = { sizeof(trust) };
    wchar_t query[96], *filename;
    struct { WORD language, codepage; } *translations;
    BYTE *version;
    DWORD size, unused;
    UINT length, count, i;
    BOOL result = FALSE;

    size = GetFileVersionInfoSizeW(path, &unused);
    if (!size || size > 1024 * 1024) return FALSE;
    version = malloc(size);
    if (!version) return FALSE;
    if (GetFileVersionInfoW(path, 0, size, version) &&
        VerQueryValueW(version, L"\\VarFileInfo\\Translation",
                       (void **)&translations, &length)) {
        count = length / sizeof(*translations);
        for (i = 0; i < count; i++) {
            swprintf_s(query, 96, L"\\StringFileInfo\\%04x%04x\\OriginalFilename",
                       translations[i].language, translations[i].codepage);
            if (VerQueryValueW(version, query, (void **)&filename, &length) &&
                length && (!_wcsicmp(filename, L"opengl32.dll") ||
                           !_wcsicmp(filename, L"opengl32"))) result = TRUE;
        }
    }
    free(version);
    if (!result) return FALSE;
    file.pcwszFilePath = path;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    if (verify_microsoft_signature(&trust)) return TRUE;
    {
        const wchar_t *algorithms[] = { L"SHA256", L"SHA1" };
        HANDLE input = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        UINT algorithm;
        if (input == INVALID_HANDLE_VALUE) return FALSE;
        result = FALSE;
        for (algorithm = 0; algorithm < 2 && !result; algorithm++) {
            HCATADMIN admin;
            HCATINFO catalog = NULL;
            BYTE hash[64];
            DWORD hash_size = sizeof(hash);
            wchar_t tag[129];
            if (!CryptCATAdminAcquireContext2(&admin, NULL, algorithms[algorithm], NULL, 0))
                continue;
            SetFilePointer(input, 0, NULL, FILE_BEGIN);
            if (CryptCATAdminCalcHashFromFileHandle2(admin, input, &hash_size, hash, 0) &&
                hash_size <= sizeof(hash)) {
                for (i = 0; i < hash_size; i++)
                    swprintf_s(tag + i * 2, 129 - i * 2, L"%02X", hash[i]);
                while ((catalog = CryptCATAdminEnumCatalogFromHash(admin, hash, hash_size,
                                                                   0, &catalog)) != NULL) {
                    CATALOG_INFO info = { sizeof(info) };
                    WINTRUST_CATALOG_INFO member = { sizeof(member) };
                    if (!CryptCATCatalogInfoFromContext(catalog, &info, 0)) continue;
                    member.pcwszCatalogFilePath = info.wszCatalogFile;
                    member.pcwszMemberTag = tag;
                    member.pcwszMemberFilePath = path;
                    member.hMemberFile = input;
                    member.pbCalculatedFileHash = hash;
                    member.cbCalculatedFileHash = hash_size;
                    member.hCatAdmin = admin;
                    ZeroMemory(&trust, sizeof(trust));
                    trust.cbStruct = sizeof(trust);
                    trust.dwUnionChoice = WTD_CHOICE_CATALOG;
                    trust.pCatalog = &member;
                    if (verify_microsoft_signature(&trust)) {
                        result = TRUE;
                        CryptCATAdminReleaseCatalogContext(admin, catalog, 0);
                        break;
                    }
                }
            }
            CryptCATAdminReleaseContext(admin, 0);
        }
        CloseHandle(input);
    }
    return result;
}

static BOOL grant_system_file_control(const wchar_t *path)
{
    HANDLE token;
    TOKEN_PRIVILEGES privileges = { 1 };
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof(sid), result;
    EXPLICIT_ACCESSW access = { 0 };
    PACL acl = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) return FALSE;
    if (!LookupPrivilegeValueW(NULL, SE_TAKE_OWNERSHIP_NAME,
                              &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    CloseHandle(token);
    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, sid, &sid_size)) return FALSE;
    result = SetNamedSecurityInfoW((wchar_t *)path, SE_FILE_OBJECT,
                                  OWNER_SECURITY_INFORMATION, sid, NULL, NULL, NULL);
    if (result != ERROR_SUCCESS) return FALSE;
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = GRANT_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = (wchar_t *)sid;
    {
        PACL old_acl = NULL;
        PSECURITY_DESCRIPTOR descriptor = NULL;
        result = GetNamedSecurityInfoW((wchar_t *)path, SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, NULL, NULL,
                                      &old_acl, NULL, &descriptor);
        if (result == ERROR_SUCCESS)
            result = SetEntriesInAclW(1, &access, old_acl, &acl);
        if (descriptor) LocalFree(descriptor);
    }
    if (result == ERROR_SUCCESS)
        result = SetNamedSecurityInfoW((wchar_t *)path, SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL);
    if (acl) LocalFree(acl);
    return result == ERROR_SUCCESS;
}

#if defined(_M_X64)
static BOOL install_system_file(const wchar_t *source, const wchar_t *runtime)
{
    return files_equal(source, runtime) ||
        ((GetFileAttributesW(runtime) == INVALID_FILE_ATTRIBUTES || grant_system_file_control(runtime)) &&
         replace_file(source, runtime));
}

static BOOL dll_image_exports(HMODULE module, WORD machine, BYTE **image,
                              DWORD *image_size, IMAGE_EXPORT_DIRECTORY **exports)
{
    BYTE *base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY directory;
    DWORD size;

    if (!module) return FALSE;
    base = (BYTE *)((ULONG_PTR)module & ~(ULONG_PTR)3);
    nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->FileHeader.Machine != machine) return FALSE;
    if (machine == IMAGE_FILE_MACHINE_AMD64 &&
        nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        size = nt->OptionalHeader.SizeOfImage;
        directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (machine == IMAGE_FILE_MACHINE_I386 &&
               nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_NT_HEADERS32 *nt32 = (IMAGE_NT_HEADERS32 *)nt;
        size = nt32->OptionalHeader.SizeOfImage;
        directory = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else return FALSE;
    *image = base;
    *image_size = size;
    if (!exports) return TRUE;
    if (!directory.VirtualAddress || size < sizeof(**exports) ||
        directory.VirtualAddress > size - sizeof(**exports)) return FALSE;
    *exports = (IMAGE_EXPORT_DIRECTORY *)(base + directory.VirtualAddress);
    return (*exports)->NumberOfNames <= size / sizeof(DWORD) &&
        (*exports)->AddressOfNames <= size - (*exports)->NumberOfNames * sizeof(DWORD);
}

static BOOL dll_image_has_export(BYTE *base, DWORD size, IMAGE_EXPORT_DIRECTORY *exports,
                                 const char *required_export)
{
    DWORD *names, i;
    size_t export_length = strlen(required_export) + 1;

    names = (DWORD *)(base + exports->AddressOfNames);
    for (i = 0; i < exports->NumberOfNames; i++) {
        if (export_length <= size && names[i] <= size - export_length &&
            !memcmp(base + names[i], required_export, export_length)) return TRUE;
    }
    return FALSE;
}

static BOOL validate_dll_export(const wchar_t *path, WORD machine, const char *required_export)
{
    HMODULE module = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    BYTE *base;
    DWORD size;
    IMAGE_EXPORT_DIRECTORY *exports = NULL;
    BOOL found;

    if (!module) return FALSE;
    found = dll_image_exports(module, machine, &base, &size, required_export ? &exports : NULL);
    if (found && required_export) found = dll_image_has_export(base, size, exports, required_export);
    FreeLibrary(module);
    return found;
}

static BOOL fixed_file_version(const wchar_t *path, VS_FIXEDFILEINFO *out)
{
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path, &ignored);
    BYTE *data;
    VS_FIXEDFILEINFO *info = NULL;
    UINT info_size = 0;
    BOOL result = FALSE;

    if (!size) return FALSE;
    data = malloc(size);
    if (!data) return FALSE;
    if (GetFileVersionInfoW(path, 0, size, data) &&
        VerQueryValueW(data, L"\\", (void **)&info, &info_size) && info &&
        info_size >= sizeof(*info) && info->dwSignature == VS_FFI_SIGNATURE) {
        *out = *info;
        result = TRUE;
    }
    free(data);
    return result;
}

static BOOL matching_file_versions(const wchar_t *a_path, const wchar_t *b_path)
{
    VS_FIXEDFILEINFO a, b;
    return fixed_file_version(a_path, &a) && fixed_file_version(b_path, &b) &&
           a.dwFileVersionMS == b.dwFileVersionMS &&
           a.dwFileVersionLS == b.dwFileVersionLS;
}

static BOOL run_nvidia_smi_probe(const wchar_t *exe, char *detail, size_t detail_size,
                                 DWORD *exit_code)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    HANDLE read_pipe = INVALID_HANDLE_VALUE, write_pipe = INVALID_HANDLE_VALUE;
    wchar_t command[MAX_PATH + 4];
    DWORD wait, read_count;
    size_t used = 0;
    BOOL ok = FALSE;

    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return FALSE;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    swprintf_s(command, ARRAYSIZE(command), L"\"%s\"", exe);

    if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        if (detail && detail_size)
            sprintf_s(detail, detail_size, "CreateProcess failed (%lu)", GetLastError());
        goto done;
    }
    CloseHandle(write_pipe);
    write_pipe = INVALID_HANDLE_VALUE;
    wait = WaitForSingleObject(pi.hProcess, 10000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 2000);
        if (detail && detail_size) strcpy_s(detail, detail_size, "probe timed out");
        *exit_code = ERROR_TIMEOUT;
        goto process_done;
    }
    GetExitCodeProcess(pi.hProcess, exit_code);
    if (detail && detail_size) {
        while (used + 1 < detail_size &&
               ReadFile(read_pipe, detail + used, (DWORD)(detail_size - used - 1),
                        &read_count, NULL) && read_count) {
            used += read_count;
        }
        detail[used] = 0;
        while (used && (detail[used - 1] == '\r' || detail[used - 1] == '\n'))
            detail[--used] = 0;
    }
    ok = TRUE;

process_done:
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
done:
    if (write_pipe != INVALID_HANDLE_VALUE) CloseHandle(write_pipe);
    if (read_pipe != INVALID_HANDLE_VALUE) CloseHandle(read_pipe);
    return ok;
}

BOOL nvidia_smi_provision(NvidiaSmiFailure *failure, char *detail, size_t detail_size)
{
#if defined(_M_X64)
    wchar_t sys[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH];
    DWORD length, exit_code = 1;
    int attempt;

    if (failure) *failure = NVIDIA_SMI_FAILURE_FILES;
    if (detail && detail_size) detail[0] = 0;
    length = GetSystemDirectoryW(sys, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        if (detail && detail_size) strcpy_s(detail, detail_size, "GetSystemDirectory failed");
        return FALSE;
    }
    swprintf_s(exe, MAX_PATH, L"%s\\nvidia-smi.exe", sys);
    swprintf_s(dll, MAX_PATH, L"%s\\nvml.dll", sys);
    if (!validate_dll_export(exe, IMAGE_FILE_MACHINE_AMD64, NULL)) {
        if (detail && detail_size) strcpy_s(detail, detail_size,
            "nvidia-smi.exe is missing or is not a valid x64 PE executable");
        return FALSE;
    }
    if (!validate_dll_export(dll, IMAGE_FILE_MACHINE_AMD64, "nvmlInit_v2")) {
        if (detail && detail_size) strcpy_s(detail, detail_size,
            "nvml.dll is missing, invalid, or does not export nvmlInit_v2");
        return FALSE;
    }
    if (!matching_file_versions(exe, dll)) {
        if (detail && detail_size) strcpy_s(detail, detail_size,
            "nvidia-smi.exe and nvml.dll file versions do not match");
        return FALSE;
    }

    if (failure) *failure = NVIDIA_SMI_FAILURE_RUNTIME;
    for (attempt = 0; attempt < 3; attempt++) {
        if (run_nvidia_smi_probe(exe, detail, detail_size, &exit_code) && exit_code == 0) {
            if (failure) *failure = NVIDIA_SMI_FAILURE_NONE;
            return TRUE;
        }
        if (attempt < 2) Sleep(2000);
    }
    if (detail && detail_size && !detail[0])
        sprintf_s(detail, detail_size, "nvidia-smi exited with code %lu", exit_code);
    return FALSE;
#else
    if (failure) *failure = NVIDIA_SMI_FAILURE_FILES;
    if (detail && detail_size)
        strcpy_s(detail, detail_size, "x64 NVIDIA-SMI is unsupported on this guest architecture");
    return FALSE;
#endif
}

static BOOL validate_nvidia_opencl_dll(const wchar_t *path, BOOL shim)
{
    return validate_dll_export(path, IMAGE_FILE_MACHINE_AMD64, "clGetExtensionFunctionAddress") &&
        validate_dll_export(path, IMAGE_FILE_MACHINE_AMD64, "clGetPlatformInfo") &&
        validate_dll_export(path, IMAGE_FILE_MACHINE_AMD64, "appsandbox_opencl") == shim;
}

static BOOL dll_exports_covered(const wchar_t *source, const wchar_t *replacement, WORD machine)
{
    HMODULE original = LoadLibraryExW(source, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    HMODULE proxy = LoadLibraryExW(replacement, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    BYTE *base, *replacement_base;
    DWORD size, replacement_size, *names, i;
    IMAGE_EXPORT_DIRECTORY *exports, *replacement_exports;
    BOOL result = FALSE;

    if (!dll_image_exports(original, machine, &base, &size, &exports) ||
        !dll_image_exports(proxy, machine, &replacement_base, &replacement_size,
                           &replacement_exports)) goto done;
    names = (DWORD *)(base + exports->AddressOfNames);
    for (i = 0; i < exports->NumberOfNames; i++) {
        if (names[i] >= size || !memchr(base + names[i], 0, size - names[i]) ||
            !dll_image_has_export(replacement_base, replacement_size, replacement_exports,
                                  (char *)base + names[i])) goto done;
    }
    result = TRUE;
done:
    if (proxy) FreeLibrary(proxy);
    if (original) FreeLibrary(original);
    return result;
}

static BOOL validate_opengl_dll(const wchar_t *path, WORD machine, BOOL native)
{
    return validate_dll_export(path, machine, native ? "appsandbox_nvidia" : NULL);
}

static UINT find_nvidia_driver_files(wchar_t paths[32][MAX_PATH], const wchar_t *sys,
                                     const wchar_t *filename)
{
    typedef NTSTATUS (APIENTRY *EnumAdaptersFn)(const D3DKMT_ENUMADAPTERS2 *);
    typedef NTSTATUS (APIENTRY *QueryAdapterFn)(const D3DKMT_QUERYADAPTERINFO *);
    typedef NTSTATUS (APIENTRY *CloseAdapterFn)(const D3DKMT_CLOSEADAPTER *);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    EnumAdaptersFn enumerate;
    QueryAdapterFn query;
    CloseAdapterFn close;
    D3DKMT_ADAPTERINFO *adapters = NULL;
    D3DKMT_ENUMADAPTERS2 list = { 0 };
    ULONG i, capacity = 0;
    UINT count = 0;

    if (!gdi) return 0;
    enumerate = (EnumAdaptersFn)GetProcAddress(gdi, "D3DKMTEnumAdapters2");
    query = (QueryAdapterFn)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    close = (CloseAdapterFn)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!enumerate || !query || !close || enumerate(&list) < 0 ||
        !list.NumAdapters) goto done;
    capacity = list.NumAdapters;
    adapters = calloc(capacity, sizeof(*adapters));
    if (!adapters) goto done;
    list.pAdapters = adapters;
    if (enumerate(&list) < 0 || list.NumAdapters > capacity) goto cleanup;
    for (i = 0; i < list.NumAdapters; i++) {
        D3DKMT_QUERY_DEVICE_IDS ids = { 0 };
        D3DKMT_ADAPTERTYPE type = { 0 };
        D3DKMT_OPENGLINFO gl = { 0 };
        D3DKMT_QUERYADAPTERINFO info = { adapters[i].hAdapter,
            KMTQAITYPE_PHYSICALADAPTERDEVICEIDS, &ids, sizeof(ids) };
        wchar_t driver[MAX_PATH], path[MAX_PATH], *slash, *store;
        UINT j;

        if (query(&info) < 0 || ids.DeviceIds.VendorID != 0x10de) goto next;
        info.Type = KMTQAITYPE_ADAPTERTYPE_RENDER;
        info.pPrivateDriverData = &type;
        info.PrivateDriverDataSize = sizeof(type);
        if (query(&info) < 0 || !type.Paravirtualized) goto next;
        info.Type = KMTQAITYPE_UMOPENGLINFO;
        info.pPrivateDriverData = &gl;
        info.PrivateDriverDataSize = sizeof(gl);
        if (query(&info) < 0 || !gl.UmdOpenGlIcdFileName[0]) goto next;
        gl.UmdOpenGlIcdFileName[MAX_PATH - 1] = 0;
        if (!_wcsnicmp(gl.UmdOpenGlIcdFileName, L"\\SystemRoot\\", 12)) {
            wchar_t windows[MAX_PATH];
            if (!GetWindowsDirectoryW(windows, MAX_PATH)) goto next;
            swprintf_s(driver, MAX_PATH, L"%s\\%s", windows, gl.UmdOpenGlIcdFileName + 12);
        } else if (!_wcsnicmp(gl.UmdOpenGlIcdFileName, L"\\??\\", 4)) {
            wcscpy_s(driver, MAX_PATH, gl.UmdOpenGlIcdFileName + 4);
        } else if (gl.UmdOpenGlIcdFileName[1] == L':') {
            wcscpy_s(driver, MAX_PATH, gl.UmdOpenGlIcdFileName);
        } else {
            swprintf_s(driver, MAX_PATH, L"%s\\%s", sys, gl.UmdOpenGlIcdFileName);
        }
        if (GetFileAttributesW(driver) == INVALID_FILE_ATTRIBUTES &&
            (store = wcsstr(driver, L"\\DriverStore\\")) != NULL) {
            wchar_t suffix[MAX_PATH];
            wcscpy_s(suffix, MAX_PATH, store + 12);
            *store = 0;
            wcscat_s(driver, MAX_PATH, L"\\HostDriverStore");
            wcscat_s(driver, MAX_PATH, suffix);
        }
        slash = wcsrchr(driver, L'\\');
        if (!slash || _wcsicmp(slash + 1, L"nvoglv64.dll") ||
            GetFileAttributesW(driver) == INVALID_FILE_ATTRIBUTES) goto next;
        *slash = 0;
        if (!_wcsicmp(filename, L"nv-vk32.json")) {
            swprintf_s(path, MAX_PATH, L"%s\\nvoglv32.dll", driver);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) goto next;
        }
        swprintf_s(path, MAX_PATH, L"%s\\%s", driver, filename);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) goto next;
        for (j = 0; j < count; j++)
            if (!_wcsicmp(paths[j], path)) break;
        if (j == count && count < 32) wcscpy_s(paths[count++], MAX_PATH, path);
next:
        ;
    }
cleanup:
    for (i = 0; i < capacity; i++) {
        if (adapters[i].hAdapter) {
            D3DKMT_CLOSEADAPTER closing = { adapters[i].hAdapter };
            close(&closing);
        }
    }
done:
    free(adapters);
    FreeLibrary(gdi);
    return count;
}

typedef struct {
    DWORD tag;
    USHORT data_length, reserved;
    USHORT substitute_offset, substitute_length, print_offset, print_length;
    wchar_t paths[(MAX_PATH + 4) * 2];
} NvidiaNgxJunction;

static HANDLE create_ngx_junction_directory(const wchar_t *path, BOOL *created)
{
    typedef NTSTATUS (NTAPI *CreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    CreateFileFn create;
    HANDLE directory = INVALID_HANDLE_VALUE, token;
    TOKEN_PRIVILEGES privileges = { 1 }, previous = { 0 };
    DWORD previous_size = sizeof(previous), error;
    wchar_t native[MAX_PATH + 4];
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes = { sizeof(attributes) };
    IO_STATUS_BLOCK status;

    *created = CreateDirectoryW(path, NULL);
    if (*created) {
        directory = CreateFileW(path, GENERIC_WRITE | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (directory != INVALID_HANDLE_VALUE) return directory;
    }
    error = GetLastError();
    if (error != ERROR_ACCESS_DENIED) return INVALID_HANDLE_VALUE;
    create = (CreateFileFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
    if (!create || swprintf_s(native, ARRAYSIZE(native), L"\\??\\%s", path) < 0 ||
        !OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          &token)) return INVALID_HANDLE_VALUE;
    if (!LookupPrivilegeValueW(NULL, SE_RESTORE_NAME, &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return INVALID_HANDLE_VALUE;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(previous),
                               &previous, &previous_size) && GetLastError() == ERROR_SUCCESS) {
        name.Buffer = native;
        name.Length = (USHORT)(wcslen(native) * sizeof(wchar_t));
        name.MaximumLength = (USHORT)(name.Length + sizeof(wchar_t));
        attributes.ObjectName = &name;
        attributes.Attributes = OBJ_CASE_INSENSITIVE;
        if (create(&directory, GENERIC_WRITE | DELETE | SYNCHRONIZE, &attributes, &status,
            NULL, FILE_ATTRIBUTE_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            *created ? FILE_OPEN : FILE_CREATE,
            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_REPARSE_POINT, NULL, 0) >= 0)
            *created = TRUE;
        else
            directory = INVALID_HANDLE_VALUE;
    }
    if (previous.PrivilegeCount)
        AdjustTokenPrivileges(token, FALSE, &previous, 0, NULL, NULL);
    CloseHandle(token);
    return directory;
}

static BOOL nvidia_ngx_junction(const wchar_t *sys, const wchar_t *package)
{
    wchar_t host_prefix[MAX_PATH], driver_prefix[MAX_PATH], target[MAX_PATH];
    wchar_t link[MAX_PATH], file[MAX_PATH], native[MAX_PATH + 4];
    const wchar_t *leaf;
    NvidiaNgxJunction reparse = { 0 };
    HANDLE directory;
    DWORD attributes, bytes;
    size_t target_length, native_length;
    BOOL created = FALSE, result;

    if (swprintf_s(host_prefix, MAX_PATH, L"%s\\HostDriverStore\\FileRepository\\", sys) < 0 ||
        swprintf_s(driver_prefix, MAX_PATH, L"%s\\DriverStore\\FileRepository\\", sys) < 0)
        return FALSE;
    if (!_wcsnicmp(package, host_prefix, wcslen(host_prefix)))
        leaf = package + wcslen(host_prefix);
    else if (!_wcsnicmp(package, driver_prefix, wcslen(driver_prefix)))
        leaf = package + wcslen(driver_prefix);
    else return FALSE;
    if (!leaf[0] || !wcscmp(leaf, L".") || !wcscmp(leaf, L"..") || wcspbrk(leaf, L"\\/:") ||
        swprintf_s(target, MAX_PATH, L"%s%s", host_prefix, leaf) < 0 ||
        swprintf_s(link, MAX_PATH, L"%s%s", driver_prefix, leaf) < 0 ||
        swprintf_s(native, ARRAYSIZE(native), L"\\??\\%s", target) < 0)
        return FALSE;
    attributes = GetFileAttributesW(link);
    if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t host_file[MAX_PATH];
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            swprintf_s(file, MAX_PATH, L"%s\\_nvngx.dll", link) < 0 ||
            swprintf_s(host_file, MAX_PATH, L"%s\\_nvngx.dll", target) < 0 ||
            !validate_dll_export(file, IMAGE_FILE_MACHINE_AMD64, NULL)) return FALSE;
        return GetFileAttributesW(host_file) == INVALID_FILE_ATTRIBUTES || files_equal(file, host_file);
    }
    if (swprintf_s(file, MAX_PATH, L"%s\\_nvngx.dll", target) < 0 ||
        !validate_dll_export(file, IMAGE_FILE_MACHINE_AMD64, NULL)) return FALSE;
    native_length = wcslen(native) * sizeof(wchar_t);
    target_length = wcslen(target) * sizeof(wchar_t);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        directory = CreateFileW(link, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (directory == INVALID_HANDLE_VALUE) return FALSE;
        result = DeviceIoControl(directory, FSCTL_GET_REPARSE_POINT, NULL, 0,
                                  &reparse, sizeof(reparse), &bytes, NULL);
        CloseHandle(directory);
        return result && bytes >= (DWORD)FIELD_OFFSET(NvidiaNgxJunction, paths) &&
            reparse.tag == IO_REPARSE_TAG_MOUNT_POINT &&
            reparse.substitute_offset % sizeof(wchar_t) == 0 &&
            reparse.substitute_length == native_length &&
            (DWORD)reparse.substitute_offset + reparse.substitute_length <=
                bytes - (DWORD)FIELD_OFFSET(NvidiaNgxJunction, paths) &&
            !_wcsnicmp(reparse.paths + reparse.substitute_offset / sizeof(wchar_t),
                       native, native_length / sizeof(wchar_t));
    }
    directory = create_ngx_junction_directory(link, &created);
    if (directory == INVALID_HANDLE_VALUE) {
        if (created) RemoveDirectoryW(link);
        return FALSE;
    }
    reparse.tag = IO_REPARSE_TAG_MOUNT_POINT;
    reparse.substitute_length = (USHORT)native_length;
    reparse.print_offset = (USHORT)(native_length + sizeof(wchar_t));
    reparse.print_length = (USHORT)target_length;
    reparse.data_length = (USHORT)(8 + native_length + target_length + 2 * sizeof(wchar_t));
    memcpy(reparse.paths, native, native_length + sizeof(wchar_t));
    memcpy((BYTE *)reparse.paths + reparse.print_offset, target, target_length + sizeof(wchar_t));
    result = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &reparse,
                              reparse.data_length + 8, NULL, 0, &bytes, NULL);
    if (!result) {
        FILE_DISPOSITION_INFO disposition = { TRUE };
        SetFileInformationByHandle(directory, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    CloseHandle(directory);
    return result;
}

static BOOL provision_nvapi(const wchar_t *native_dir, const wchar_t *sys,
                            wchar_t sources[32][MAX_PATH], UINT count)
{
    wchar_t payload[MAX_PATH], backup[MAX_PATH], runtime[MAX_PATH];
    UINT i;

    if (!count || swprintf_s(payload, MAX_PATH, L"%s\\appsandbox-nvidia-dlss-shim.dll", native_dir) < 0 ||
        swprintf_s(runtime, MAX_PATH, L"%s\\nvapi64.dll", sys) < 0 ||
        swprintf_s(backup, MAX_PATH, L"%s\\appsandbox-nvapi64.dll", sys) < 0)
        return FALSE;
    if (!validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "appsandbox_nvapi") ||
        !validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "nvapi_QueryInterface") ||
        !validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "nvapi_QueryInterface") ||
        validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "appsandbox_nvapi")) return FALSE;
    for (i = 1; i < count; i++)
        if (!files_equal(sources[0], sources[i])) return FALSE;
    return replace_file(sources[0], backup) && install_system_file(payload, runtime);
}

static BOOL provision_nvidia_library(const wchar_t *sys, const wchar_t *filename,
                                    const char *required_export)
{
    wchar_t sources[32][MAX_PATH], runtime[MAX_PATH];
    UINT count = find_nvidia_driver_files(sources, sys, filename), i;

    if (!count) return TRUE;
    if (!validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, required_export) ||
        swprintf_s(runtime, MAX_PATH, L"%s\\%s", sys, filename) < 0) return FALSE;
    for (i = 1; i < count; i++)
        if (!files_equal(sources[0], sources[i])) return FALSE;
    return install_system_file(sources[0], runtime);
}

static BOOL provision_cuda(const wchar_t *native_dir, const wchar_t *sys)
{
    wchar_t sources[32][MAX_PATH], payload[MAX_PATH], runtime[MAX_PATH], backup[MAX_PATH];
    UINT count, i;

    if (!native_dir || !native_dir[0]) return TRUE;
    if (swprintf_s(payload, MAX_PATH, L"%s\\appsandbox-nvidia-cuda-shim.dll", native_dir) < 0)
        return FALSE;
    if (GetFileAttributesW(payload) == INVALID_FILE_ATTRIBUTES) return TRUE;
    count = find_nvidia_driver_files(sources, sys, L"nvcuda_loader64.dll");
    if (!count) return TRUE;
    if (!validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "appsandbox_cuda") ||
        !validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "cuInit") ||
        !validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "cuGetProcAddress") ||
        !validate_dll_export(payload, IMAGE_FILE_MACHINE_AMD64, "cuDeviceGetLuid") ||
        !validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "cuInit") ||
        !validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "cuGetProcAddress") ||
        !validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "cuDeviceGetLuid") ||
        validate_dll_export(sources[0], IMAGE_FILE_MACHINE_AMD64, "appsandbox_cuda") ||
        swprintf_s(runtime, MAX_PATH, L"%s\\nvcuda.dll", sys) < 0 ||
        swprintf_s(backup, MAX_PATH, L"%s\\appsandbox-nvcuda.dll", sys) < 0) return FALSE;
    for (i = 1; i < count; i++)
        if (!files_equal(sources[0], sources[i])) return FALSE;
    if (!dll_exports_covered(sources[0], payload, IMAGE_FILE_MACHINE_AMD64)) {
        if (validate_dll_export(runtime, IMAGE_FILE_MACHINE_AMD64, "appsandbox_cuda"))
            install_system_file(sources[0], runtime);
        return FALSE;
    }
    return replace_file(sources[0], backup) && install_system_file(payload, runtime);
}

static BOOL provision_nvidia_opencl(const wchar_t *native_dir, const wchar_t *sys)
{
    wchar_t runtimes[32][MAX_PATH], payload[MAX_PATH], backup[MAX_PATH], *slash;
    UINT count, i;
    BOOL result = TRUE;

    if (!native_dir || !native_dir[0]) return TRUE;
    if (swprintf_s(payload, MAX_PATH, L"%s\\appsandbox-nvidia-opencl-shim.dll", native_dir) < 0)
        return FALSE;
    if (GetFileAttributesW(payload) == INVALID_FILE_ATTRIBUTES) return TRUE;
    count = find_nvidia_driver_files(runtimes, sys, L"nvopencl64.dll");
    if (!count) return TRUE;
    if (!validate_nvidia_opencl_dll(payload, TRUE)) return FALSE;
    for (i = 0; i < count; i++) {
        const wchar_t *original;
        BOOL installed = validate_dll_export(runtimes[i], IMAGE_FILE_MACHINE_AMD64,
                                             "appsandbox_opencl");
        if (wcscpy_s(backup, MAX_PATH, runtimes[i]) || !(slash = wcsrchr(backup, L'\\')))
            return FALSE;
        *slash = 0;
        if (wcscat_s(backup, MAX_PATH, L"\\appsandbox-nvopencl64.dll")) return FALSE;
        original = installed ? backup : runtimes[i];
        if (!validate_nvidia_opencl_dll(original, FALSE)) {
            result = FALSE;
            continue;
        }
        if (!dll_exports_covered(original, payload, IMAGE_FILE_MACHINE_AMD64)) {
            if (installed) install_system_file(backup, runtimes[i]);
            result = FALSE;
            continue;
        }
        if ((!installed && !replace_file(runtimes[i], backup)) ||
            !install_system_file(payload, runtimes[i]))
            result = FALSE;
    }
    return result;
}

static const char *json_skip_whitespace(const char *p)
{
    while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t') p++;
    return p;
}

static const char *json_skip_string(const char *p)
{
    if (*p++ != '"') return NULL;
    while (*p && *p != '"') {
        if ((unsigned char)*p < 32) return NULL;
        if (*p++ == '\\') {
            int i;
            if (!*p) return NULL;
            if (*p == 'u') {
                p++;
                for (i = 0; i < 4; i++, p++)
                    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                          (*p >= 'A' && *p <= 'F'))) return NULL;
            } else if (strchr("\"\\/bfnrt", *p)) {
                p++;
            } else return NULL;
        }
    }
    return *p == '"' ? p + 1 : NULL;
}

static const char *json_skip_value(const char *p, int depth)
{
    char end;
    if (depth > 32) return NULL;
    p = json_skip_whitespace(p);
    if (*p == '"') return json_skip_string(p);
    if (*p == '{' || *p == '[') {
        BOOL object = *p == '{';
        end = object ? '}' : ']';
        p = json_skip_whitespace(p + 1);
        if (*p == end) return p + 1;
        for (;;) {
            if (object) {
                p = json_skip_string(p);
                if (!p || *(p = json_skip_whitespace(p)) != ':') return NULL;
                p++;
            }
            p = json_skip_value(p, depth + 1);
            if (!p) return NULL;
            p = json_skip_whitespace(p);
            if (*p == end) return p + 1;
            if (*p++ != ',') return NULL;
            p = json_skip_whitespace(p);
        }
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    if (*p == '-') p++;
    if (*p == '0') p++;
    else {
        if (*p < '1' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p < '0' || *p > '9') return NULL;
        while (*p >= '0' && *p <= '9') p++;
    }
    return p;
}

static const char *json_find_member(const char *p, const char *key)
{
    const char *name, *end, *found = NULL;
    size_t length = strlen(key);
    p = json_skip_whitespace(p);
    if (*p++ != '{') return NULL;
    p = json_skip_whitespace(p);
    while (*p && *p != '}') {
        name = p;
        end = json_skip_string(p);
        if (!end) return NULL;
        p = json_skip_whitespace(end);
        if (*p++ != ':') return NULL;
        p = json_skip_whitespace(p);
        if ((size_t)(end - name) == length + 2 && !memcmp(name + 1, key, length)) {
            if (found) return NULL;
            found = p;
        }
        p = json_skip_value(p, 0);
        if (!p) return NULL;
        p = json_skip_whitespace(p);
        if (*p == '}') break;
        if (*p++ != ',') return NULL;
        p = json_skip_whitespace(p);
    }
    return found;
}

static char *read_vulkan_manifest(const wchar_t *path, const char **library, const char **end)
{
    FILE *file;
    long length;
    char *text;
    const char *icd, *tail;
    if (_wfopen_s(&file, path, L"rb")) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || length > 65536 ||
        fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    text = calloc((size_t)length + 1, 1);
    if (!text) { fclose(file); return NULL; }
    if (fread(text, 1, length, file) != (size_t)length) {
        fclose(file); free(text); return NULL;
    }
    fclose(file);
    if (memchr(text, 0, length)) { free(text); return NULL; }
    tail = json_skip_value(text, 0);
    icd = json_find_member(text, "ICD");
    *library = icd ? json_find_member(icd, "library_path") : NULL;
    *end = *library ? json_skip_string(*library) : NULL;
    if (!tail || *json_skip_whitespace(tail) || !*library || !*end) {
        free(text);
        return NULL;
    }
    return text;
}

static BOOL update_vulkan_icd_path(const wchar_t *path, const wchar_t *runtime, BOOL restore)
{
    wchar_t backup[MAX_PATH], temporary[MAX_PATH], folder[MAX_PATH], *slash;
    char utf8[MAX_PATH * 4], escaped[MAX_PATH * 8 + 3];
    const char *library, *end;
    char *text;
    size_t i, n = 0;
    FILE *file;
    BOOL result = FALSE, ours;

    if (!WideCharToMultiByte(CP_UTF8, 0, runtime, -1, utf8, sizeof(utf8), NULL, NULL))
        return FALSE;
    escaped[n++] = '"';
    for (i = 0; utf8[i]; i++) {
        if (utf8[i] == '\\' || utf8[i] == '"') escaped[n++] = '\\';
        escaped[n++] = utf8[i];
    }
    escaped[n++] = '"';
    escaped[n] = 0;
    text = read_vulkan_manifest(path, &library, &end);
    if (!text) return FALSE;
    ours = (size_t)(end - library) == n && !memcmp(library, escaped, n);
    swprintf_s(backup, MAX_PATH, L"%s.asbak", path);
    if (restore || ours) {
        char *original;
        const char *old_library, *old_end;
        if (!ours) { free(text); return TRUE; }
        original = read_vulkan_manifest(backup, &old_library, &old_end);
        if (original) {
            BOOL valid = (size_t)(old_end - old_library) != n ||
                         memcmp(old_library, escaped, n);
            if (valid) result = !restore || replace_file(backup, path);
            free(original);
        }
        free(text);
        return result;
    }
    if (!replace_file(path, backup)) { free(text); return FALSE; }
    wcscpy_s(folder, MAX_PATH, path);
    slash = wcsrchr(folder, L'\\');
    if (!slash) { free(text); return FALSE; }
    *slash = 0;
    if (!GetTempFileNameW(folder, L"asb", 0, temporary)) { free(text); return FALSE; }
    if (!_wfopen_s(&file, temporary, L"wb")) {
        size_t prefix = (size_t)(library - text), suffix = strlen(end);
        result = fwrite(text, 1, prefix, file) == prefix &&
                 fwrite(escaped, 1, n, file) == n &&
                 fwrite(end, 1, suffix, file) == suffix;
        if (fclose(file)) result = FALSE;
        if (result) result = replace_file(temporary, path);
    }
    DeleteFileW(temporary);
    free(text);
    return result;
}

static BOOL restore_nvidia_vulkan_manifests(const wchar_t *sys, const wchar_t *runtime,
                              const wchar_t *manifest)
{
    const wchar_t *stores[] = { L"HostDriverStore", L"DriverStore" };
    WIN32_FIND_DATAW data;
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    HANDLE find;
    UINT i;
    BOOL result = TRUE;

    swprintf_s(path, MAX_PATH, L"%s\\%s.asbak", sys, manifest);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        path[wcslen(path) - wcslen(L".asbak")] = 0;
        if (!update_vulkan_icd_path(path, runtime, TRUE)) result = FALSE;
    }
    for (i = 0; i < sizeof(stores) / sizeof(stores[0]); i++) {
        swprintf_s(pattern, MAX_PATH, L"%s\\%s\\FileRepository\\*", sys, stores[i]);
        find = FindFirstFileW(pattern, &data);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
                result = FALSE;
            continue;
        }
        do {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == L'.')
                continue;
            swprintf_s(path, MAX_PATH, L"%s\\%s\\FileRepository\\%s\\%s.asbak",
                       sys, stores[i], data.cFileName, manifest);
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) continue;
            path[wcslen(path) - wcslen(L".asbak")] = 0;
            if (!update_vulkan_icd_path(path, runtime, TRUE)) result = FALSE;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    return result;
}
#endif

static BOOL provision_gl_vk_for_arch(const wchar_t *dir, const wchar_t *native_dir,
                               const wchar_t *sys, const wchar_t *driver_sys,
                               WORD machine, BOOL system_runtime, BOOL *native_runtime)
{
    wchar_t runtime[MAX_PATH], backup[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    BOOL microsoft;

    if (native_runtime) *native_runtime = FALSE;
    swprintf_s(runtime, MAX_PATH, L"%s\\opengl32.dll", sys);
    swprintf_s(backup, MAX_PATH, L"%s\\opengl32.dll.msbak", sys);
    microsoft = is_microsoft_opengl(runtime);
#if defined(_M_X64)
    if (microsoft && !validate_opengl_dll(runtime, machine, FALSE)) return FALSE;
#endif
    if (microsoft && !replace_file(runtime, backup)) return FALSE;
#if defined(_M_X64)
    {
        wchar_t manifests[32][MAX_PATH], backend[MAX_PATH], previous[MAX_PATH];
        const wchar_t *payload = machine == IMAGE_FILE_MACHINE_I386 ?
            L"appsandbox-nvidia-vk-gl-shim32.dll" : L"appsandbox-nvidia-vk-gl-shim.dll";
        const wchar_t *manifest = machine == IMAGE_FILE_MACHINE_I386 ?
            L"nv-vk32.json" : L"nv-vk64.json";
        UINT count = 0, i;
        src[0] = 0;
        if (native_dir && native_dir[0])
            swprintf_s(src, MAX_PATH, L"%s\\%s", native_dir, payload);
        if (src[0] && validate_opengl_dll(src, machine, TRUE) &&
            (count = find_nvidia_driver_files(manifests, driver_sys, manifest)) != 0 &&
            is_microsoft_opengl(backup) && validate_opengl_dll(backup, machine, FALSE)) {
            swprintf_s(backend, MAX_PATH, L"%s\\appsandbox-opengl32.dll", sys);
            if (replace_file(backup, backend) &&
                GetTempFileNameW(sys, L"asb", 0, previous)) {
                BOOL saved = CopyFileW(runtime, previous, FALSE);
                BOOL installed = saved &&
                    (files_equal(src, runtime) || grant_system_file_control(runtime)) &&
                    replace_file(src, runtime);
                if (installed) {
                    for (i = 0; i < count; i++)
                        if (!update_vulkan_icd_path(manifests[i], runtime, FALSE)) break;
                    if (i == count) {
                        DeleteFileW(previous);
                        if (native_runtime) *native_runtime = TRUE;
                        return TRUE;
                    }
                    {
                        BOOL manifests_restored = restore_nvidia_vulkan_manifests(driver_sys, runtime, manifest);
                        BOOL runtime_restored = replace_file(previous, runtime);
                        if (!runtime_restored) return FALSE;
                        if (!manifests_restored) {
                            DeleteFileW(previous);
                            return FALSE;
                        }
                    }
                }
                DeleteFileW(previous);
            }
        }
        if (!restore_nvidia_vulkan_manifests(driver_sys, runtime, manifest)) return FALSE;
        if (system_runtime) {
            BOOL installed = microsoft ||
                (is_microsoft_opengl(backup) && validate_opengl_dll(backup, machine, FALSE) &&
                 (GetFileAttributesW(runtime) == INVALID_FILE_ATTRIBUTES ||
                  grant_system_file_control(runtime)) && replace_file(backup, runtime));
            if (native_runtime) *native_runtime = installed;
            return installed;
        }
        if ((!dir || !dir[0]) && validate_opengl_dll(runtime, machine, TRUE)) {
            if (!is_microsoft_opengl(backup) || !validate_opengl_dll(backup, machine, FALSE) ||
                !grant_system_file_control(runtime) ||
                !replace_file(backup, runtime)) return FALSE;
        }
    }
#else
    (void)native_dir;
    (void)driver_sys;
    (void)machine;
    (void)system_runtime;
#endif
    if (!dir || !dir[0]) return machine == IMAGE_FILE_MACHINE_I386 && is_microsoft_opengl(runtime);
    swprintf_s(src, MAX_PATH, L"%s\\gallium_wgl.dll", dir);
    swprintf_s(dst, MAX_PATH, L"%s\\gallium_wgl.dll", sys);
    if (!replace_file(src, dst)) return FALSE;
    swprintf_s(src, MAX_PATH, L"%s\\z-1.dll", dir);
    swprintf_s(dst, MAX_PATH, L"%s\\z-1.dll", sys);
    if (!replace_file(src, dst)) return FALSE;
    swprintf_s(src, MAX_PATH, L"%s\\opengl32.dll", dir);
    if (files_equal(src, runtime)) return TRUE;
    if (GetFileAttributesW(backup) == INVALID_FILE_ATTRIBUTES && !microsoft) return FALSE;
    return (GetFileAttributesW(runtime) == INVALID_FILE_ATTRIBUTES ||
            grant_system_file_control(runtime)) && replace_file(src, runtime);
}

BOOL gl_vk_provision_runtime(const wchar_t *dir, const wchar_t *native_dir,
                             const wchar_t *sys, BOOL *native_runtime)
{
#if defined(_M_X64)
    wchar_t wow[MAX_PATH], payload[MAX_PATH], runtime[MAX_PATH];
    BOOL system_runtime = gpu_prefers_system_opengl();
    BOOL result = provision_gl_vk_for_arch(dir, native_dir, sys, sys, IMAGE_FILE_MACHINE_AMD64,
                                    system_runtime, native_runtime);
    if (GetSystemWow64DirectoryW(wow, MAX_PATH)) {
        payload[0] = 0;
        if (native_dir && native_dir[0])
            swprintf_s(payload, MAX_PATH, L"%s\\appsandbox-nvidia-vk-gl-shim32.dll", native_dir);
        swprintf_s(runtime, MAX_PATH, L"%s\\opengl32.dll", wow);
        if (system_runtime ||
            (payload[0] && GetFileAttributesW(payload) != INVALID_FILE_ATTRIBUTES) ||
            validate_opengl_dll(runtime, IMAGE_FILE_MACHINE_I386, TRUE)) {
            if (!provision_gl_vk_for_arch(NULL, native_dir, wow, sys, IMAGE_FILE_MACHINE_I386,
                                    system_runtime, NULL))
                result = FALSE;
        }
    }
    return result;
#else
    return provision_gl_vk_for_arch(dir, native_dir, sys, sys, IMAGE_FILE_MACHINE_ARM64, FALSE,
                              native_runtime);
#endif
}

BOOL nvidia_runtime_provision(const wchar_t *native_dir)
{
#if defined(_M_X64)
    wchar_t sys[MAX_PATH], payload[MAX_PATH], packages[32][MAX_PATH];
    UINT count, i, length;
    BOOL result = TRUE;

    length = GetSystemDirectoryW(sys, MAX_PATH);
    if (!length || length >= MAX_PATH) return FALSE;
    if (!provision_nvidia_library(sys, L"nvofapi64.dll", "NvOFAPICreateInstanceCuda")) result = FALSE;
    if (!provision_nvidia_library(sys, L"nvoptix.dll", "optixQueryFunctionTable")) result = FALSE;
    if (!provision_cuda(native_dir, sys)) result = FALSE;
    if (!provision_nvidia_opencl(native_dir, sys)) result = FALSE;
    if (!native_dir || !native_dir[0]) return result;
    if (swprintf_s(payload, MAX_PATH, L"%s\\appsandbox-nvidia-dlss-shim.dll", native_dir) < 0)
        return FALSE;
    if (GetFileAttributesW(payload) == INVALID_FILE_ATTRIBUTES) return result;
    count = find_nvidia_driver_files(packages, sys, L"nvapi64.dll");
    if (count && !provision_nvapi(native_dir, sys, packages, count)) result = FALSE;
    count = find_nvidia_driver_files(packages, sys, L"_nvngx.dll");
    for (i = 0; i < count; i++) {
        wchar_t *slash = wcsrchr(packages[i], L'\\');
        if (!slash) return FALSE;
        *slash = 0;
        if (!nvidia_ngx_junction(sys, packages[i])) result = FALSE;
    }
    return result;
#else
    (void)native_dir;
    return TRUE;
#endif
}

BOOL nvidia_opencl_copy_target(const wchar_t *path, wchar_t target[MAX_PATH])
{
    target[0] = 0;
#if defined(_M_X64)
    {
        const wchar_t *slash = wcsrchr(path, L'\\');
        if (!slash || _wcsicmp(slash + 1, L"nvopencl64.dll") ||
            !validate_dll_export(path, IMAGE_FILE_MACHINE_AMD64, "appsandbox_opencl")) return TRUE;
        if (wcsncpy_s(target, MAX_PATH, path, slash + 1 - path) ||
            wcscat_s(target, MAX_PATH, L"appsandbox-nvopencl64.dll")) return FALSE;
    }
#else
    (void)path;
#endif
    return TRUE;
}

BOOL nvidia_opencl_commit_copy(const wchar_t *source, const wchar_t *target)
{
#if defined(_M_X64)
    return validate_nvidia_opencl_dll(source, FALSE) && replace_file(source, target);
#else
    (void)source;
    (void)target;
    return FALSE;
#endif
}
