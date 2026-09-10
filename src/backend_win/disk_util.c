#include "disk_util.h"
#include "ui.h"
#include "../../tools/provision/win_provision.h"
#include <virtdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <imapi2fs.h>

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "urlmon.lib")

#include <urlmon.h>

#if ASB_IS_ARM64
#  define ASB_UA_ARCH   L"arm64"
#else
#  define ASB_UA_ARCH   L"amd64"
#endif

#if ASB_IS_ARM64
#  define SSH_MSI_URL     L"https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-ARM64-v10.0.0.0.msi"
#  define SSH_MSI_NAME    L"OpenSSH-ARM64-v10.0.0.0.msi"
#  define SSH_MSI_NAME_A   "OpenSSH-ARM64-v10.0.0.0.msi"
#else
#  define SSH_MSI_URL     L"https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64-v10.0.0.0.msi"
#  define SSH_MSI_NAME    L"OpenSSH-Win64-v10.0.0.0.msi"
#  define SSH_MSI_NAME_A   "OpenSSH-Win64-v10.0.0.0.msi"
#endif

static const GUID VHDX_VENDOR_MS = {
    0xec984aec, 0xa0f9, 0x47e9,
    { 0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b }
};

HRESULT vhdx_create(const wchar_t *path, ULONGLONG size_gb)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!path || size_gb == 0)
        return E_INVALIDARG;

    /* Reuse existing VHDX if it already exists */
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return S_OK;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.MaximumSize = size_gb * 1024ULL * 1024ULL * 1024ULL;
    params.Version2.BlockSizeInBytes = 0; /* default */
    params.Version2.SectorSizeInBytes = 512;
    params.Version2.PhysicalSectorSizeInBytes = 4096;

    result = CreateVirtualDisk(
        &storage_type,
        path,
        VIRTUAL_DISK_ACCESS_NONE,
        NULL,
        CREATE_VIRTUAL_DISK_FLAG_NONE, /* dynamically expanding */
        0,
        &params,
        NULL,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    CloseHandle(vhd_handle);
    return S_OK;
}

HRESULT vhdx_create_differencing(const wchar_t *child_path, const wchar_t *parent_path)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    CREATE_VIRTUAL_DISK_PARAMETERS params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!child_path || !parent_path)
        return E_INVALIDARG;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&params, sizeof(params));
    params.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    params.Version2.ParentPath = parent_path;

    result = CreateVirtualDisk(
        &storage_type,
        child_path,
        VIRTUAL_DISK_ACCESS_NONE,
        NULL,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &params,
        NULL,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    CloseHandle(vhd_handle);
    return S_OK;
}

static DWORD vhdx_query_virtual_size(HANDLE vhd_handle, ULONGLONG *size_bytes)
{
    GET_VIRTUAL_DISK_INFO info;
    ULONG info_size = sizeof(info);
    DWORD result;

    ZeroMemory(&info, sizeof(info));
    info.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    result = GetVirtualDiskInformation(vhd_handle, &info_size, &info, NULL);
    if (result == ERROR_SUCCESS)
        *size_bytes = info.Size.VirtualSize;
    return result;
}

HRESULT vhdx_get_virtual_size(const wchar_t *path, ULONGLONG *size_bytes)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!size_bytes)
        return E_INVALIDARG;
    *size_bytes = 0;
    if (!path || !path[0])
        return E_INVALIDARG;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;
    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.GetInfoOnly = TRUE;
    open_params.Version2.ReadOnly = TRUE;

    result = OpenVirtualDisk(&storage_type, path, VIRTUAL_DISK_ACCESS_NONE,
                             OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params,
                             &vhd_handle);
    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    result = vhdx_query_virtual_size(vhd_handle, size_bytes);
    CloseHandle(vhd_handle);
    return HRESULT_FROM_WIN32(result);
}

HRESULT vhdx_grow(const wchar_t *path, ULONGLONG size_gb)
{
    const ULONGLONG bytes_per_gb = 1024ULL * 1024ULL * 1024ULL;
    VIRTUAL_STORAGE_TYPE storage_type;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    RESIZE_VIRTUAL_DISK_PARAMETERS resize_params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    ULONGLONG current_size = 0;
    ULONGLONG requested_size;
    DWORD result;

    if (!path || !path[0] || size_gb == 0 ||
        size_gb > (~(ULONGLONG)0) / bytes_per_gb)
        return E_INVALIDARG;
    requested_size = size_gb * bytes_per_gb;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;
    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;
    open_params.Version2.GetInfoOnly = FALSE;
    open_params.Version2.ReadOnly = FALSE;

    result = OpenVirtualDisk(&storage_type, path, VIRTUAL_DISK_ACCESS_NONE,
                             OPEN_VIRTUAL_DISK_FLAG_NONE, &open_params,
                             &vhd_handle);
    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    result = vhdx_query_virtual_size(vhd_handle, &current_size);
    if (result == ERROR_SUCCESS && requested_size < current_size) {
        CloseHandle(vhd_handle);
        return E_INVALIDARG;
    }
    if (result == ERROR_SUCCESS && requested_size > current_size) {
        ZeroMemory(&resize_params, sizeof(resize_params));
        resize_params.Version = RESIZE_VIRTUAL_DISK_VERSION_1;
        resize_params.Version1.NewSize = requested_size;
        result = ResizeVirtualDisk(vhd_handle, RESIZE_VIRTUAL_DISK_FLAG_NONE,
                                   &resize_params, NULL);
        if (result == ERROR_SUCCESS) {
            result = vhdx_query_virtual_size(vhd_handle, &current_size);
            if (result == ERROR_SUCCESS && current_size != requested_size)
                result = ERROR_INVALID_DATA;
        }
    }

    CloseHandle(vhd_handle);
    return HRESULT_FROM_WIN32(result);
}

HRESULT vhdx_merge(const wchar_t *child_path)
{
    VIRTUAL_STORAGE_TYPE storage_type;
    OPEN_VIRTUAL_DISK_PARAMETERS open_params;
    MERGE_VIRTUAL_DISK_PARAMETERS merge_params;
    HANDLE vhd_handle = INVALID_HANDLE_VALUE;
    DWORD result;

    if (!child_path)
        return E_INVALIDARG;

    storage_type.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storage_type.VendorId = VHDX_VENDOR_MS;

    ZeroMemory(&open_params, sizeof(open_params));
    open_params.Version = OPEN_VIRTUAL_DISK_VERSION_2;

    result = OpenVirtualDisk(
        &storage_type,
        child_path,
        VIRTUAL_DISK_ACCESS_NONE,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &open_params,
        &vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    ZeroMemory(&merge_params, sizeof(merge_params));
    merge_params.Version = MERGE_VIRTUAL_DISK_VERSION_1;
    merge_params.Version1.MergeDepth = 1;

    result = MergeVirtualDisk(
        vhd_handle,
        MERGE_VIRTUAL_DISK_FLAG_NONE,
        &merge_params,
        NULL);

    CloseHandle(vhd_handle);

    if (result != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(result);

    return S_OK;
}

/* ---- Resources ISO for unattended install ---- */

/* Windows unattend base64 password encoding:
   UTF-16LE( password + "Password" ) -> base64.
   When PlainText=false, Windows Setup decodes this at runtime. */
static BOOL encode_unattend_password(const wchar_t *pass, wchar_t *b64_out, int b64_max)
{
    wchar_t combined[256];
    DWORD bin_len, b64_len;
    char *b64_narrow;

    swprintf_s(combined, 256, L"%sPassword", pass);
    bin_len = (DWORD)(wcslen(combined) * sizeof(wchar_t));

    /* Get required base64 length */
    b64_len = 0;
    CryptBinaryToStringA((const BYTE *)combined, bin_len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len);
    b64_narrow = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len + 1);
    if (!b64_narrow) {
        SecureZeroMemory(combined, sizeof(combined));
        return FALSE;
    }

    CryptBinaryToStringA((const BYTE *)combined, bin_len,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64_narrow, &b64_len);
    SecureZeroMemory(combined, sizeof(combined));

    MultiByteToWideChar(CP_UTF8, 0, b64_narrow, -1, b64_out, b64_max);
    SecureZeroMemory(b64_narrow, b64_len);
    HeapFree(GetProcessHeap(), 0, b64_narrow);
    return TRUE;
}

static BOOL xml_escape_text(const wchar_t *text, wchar_t *out, size_t capacity)
{
    size_t used = 0;
    if (!text || !out || !capacity) return FALSE;
    while (*text) {
        const wchar_t *replacement = NULL;
        size_t count;
        switch (*text) {
            case L'&': replacement = L"&amp;"; break;
            case L'<': replacement = L"&lt;"; break;
            case L'>': replacement = L"&gt;"; break;
            case L'"': replacement = L"&quot;"; break;
            case L'\'': replacement = L"&apos;"; break;
        }
        count = replacement ? wcslen(replacement) : 1;
        if (used + count >= capacity) { out[0] = 0; return FALSE; }
        memcpy(out + used, replacement ? replacement : text, count * sizeof(wchar_t));
        used += count;
        text++;
    }
    out[used] = 0;
    return TRUE;
}

/* Map language tag to LCID:KLID format for InputLocale */
static const wchar_t *lang_to_input_locale(const wchar_t *lang)
{
    static const struct { const wchar_t *tag; const wchar_t *klid; } map[] = {
        { L"en-US", L"0409:00000409" }, { L"en-GB", L"0809:00000809" },
        { L"de-DE", L"0407:00000407" }, { L"fr-FR", L"040c:0000040c" },
        { L"fr-CA", L"0c0c:00001009" }, { L"es-ES", L"0c0a:0000040a" },
        { L"es-MX", L"080a:0000080a" }, { L"it-IT", L"0410:00000410" },
        { L"pt-BR", L"0416:00000416" }, { L"pt-PT", L"0816:00000816" },
        { L"ja-JP", L"0411:00000411" }, { L"ko-KR", L"0412:00000412" },
        { L"zh-CN", L"0804:00000804" }, { L"zh-TW", L"0404:00000404" },
        { L"ru-RU", L"0419:00000419" }, { L"pl-PL", L"0415:00000415" },
        { L"nl-NL", L"0413:00000413" }, { L"sv-SE", L"041d:0000041d" },
        { L"nb-NO", L"0414:00000414" }, { L"da-DK", L"0406:00000406" },
        { L"fi-FI", L"040b:0000040b" }, { L"cs-CZ", L"0405:00000405" },
        { L"hu-HU", L"040e:0000040e" }, { L"tr-TR", L"041f:0000041f" },
        { L"ar-SA", L"0401:00000401" }, { L"he-IL", L"040d:0000040d" },
        { L"th-TH", L"041e:0000041e" }, { L"uk-UA", L"0422:00000422" },
        { L"ro-RO", L"0418:00000418" }, { L"el-GR", L"0408:00000408" },
        { L"bg-BG", L"0402:00020402" }, { L"hr-HR", L"041a:0000041a" },
        { L"sk-SK", L"041b:0000041b" }, { L"sl-SI", L"0424:00000424" },
        { L"et-EE", L"0425:00000425" }, { L"lv-LV", L"0426:00000426" },
        { L"lt-LT", L"0427:00000427" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) {
        if (_wcsicmp(lang, map[i].tag) == 0)
            return map[i].klid;
    }
    return L"0409:00000409";  /* fallback */
}

static BOOL system_keyboard_layout(DWORD klid)
{
    wchar_t key[96], file[MAX_PATH], path[MAX_PATH];
    DWORD size = sizeof(file), attributes;
    UINT len;

    if (!klid || (klid & 0xF0000000)) return FALSE;
    swprintf_s(key, ARRAYSIZE(key),
               L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\%08X", klid);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Layout File", RRF_RT_REG_SZ,
                     NULL, file, &size) != ERROR_SUCCESS ||
        _wcsnicmp(file, L"kbd", 3) != 0 || wcspbrk(file, L"\\/:") != NULL)
        return FALSE;
    len = GetSystemDirectoryW(path, ARRAYSIZE(path));
    if (!len || len + wcslen(file) + 2 > ARRAYSIZE(path)) return FALSE;
    wcscat_s(path, ARRAYSIZE(path), L"\\");
    wcscat_s(path, ARRAYSIZE(path), file);
    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static DWORD keyboard_layout_id(HKL layout)
{
    wchar_t name[KL_NAMELENGTH];
    DWORD klid = 0;
    WORD device = HIWORD((ULONG_PTR)layout);
    HKEY layouts;

    if (layout == GetKeyboardLayout(0) && GetKeyboardLayoutNameW(name)) {
        klid = wcstoul(name, NULL, 16);
        if (system_keyboard_layout(klid)) return klid;
        klid = 0;
    }
    if ((device & 0xF000) != 0xF000)
        return system_keyboard_layout(device) ? device : 0;

    /* Variant HKLs contain a registry Layout Id, not the full KLID. */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
                      0, KEY_READ, &layouts) == ERROR_SUCCESS) {
        DWORD index;
        for (index = 0;; index++) {
            wchar_t id[16];
            DWORD length = ARRAYSIZE(name), size = sizeof(id);
            LONG result = RegEnumKeyExW(layouts, index, name, &length,
                                        NULL, NULL, NULL, NULL);
            if (result == ERROR_MORE_DATA) continue;
            if (result != ERROR_SUCCESS) break;
            if (RegGetValueW(layouts, name, L"Layout Id", RRF_RT_REG_SZ,
                             NULL, id, &size) == ERROR_SUCCESS &&
                wcstoul(id, NULL, 16) == (DWORD)(device & 0x0FFF)) {
                DWORD candidate = wcstoul(name, NULL, 16);
                if (system_keyboard_layout(candidate)) {
                    klid = candidate;
                    break;
                }
            }
        }
        RegCloseKey(layouts);
    }
    return klid;
}

void get_host_keyboard_settings(wchar_t *input_locale, size_t size,
                                DWORD *klid, WORD *langid)
{
    HWND foreground = GetForegroundWindow();
    DWORD thread = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
    HKL layout = thread ? GetKeyboardLayout(thread) : NULL;
    WORD language;
    DWORD keyboard;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH], result[128];
    const wchar_t *ime = NULL;

    if (!layout) layout = GetKeyboardLayout(0);
    language = layout ? LOWORD((ULONG_PTR)layout) : GetUserDefaultLangID();
    keyboard = layout ? keyboard_layout_id(layout) : 0;
    switch (language) {
    case 0x0411: ime = L"ja-JP"; break;
    case 0x0412: ime = L"ko-KR"; break;
    case 0x0804: case 0x1004: ime = L"zh-CN"; break;
    case 0x0404: case 0x0C04: case 0x1404: ime = L"zh-TW"; break;
    }
    if (ime) {
        wcscpy_s(result, ARRAYSIZE(result), ime);
        if (!keyboard)
            keyboard = PRIMARYLANGID(language) == LANG_CHINESE ? 0x0409 : language;
    } else {
        if (!keyboard) {
            const wchar_t *fallback;
            if (!LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale,
                                  ARRAYSIZE(locale), 0))
                wcscpy_s(locale, ARRAYSIZE(locale), L"en-US");
            fallback = wcschr(lang_to_input_locale(locale), L':');
            keyboard = wcstoul(fallback + 1, NULL, 16);
        }
        swprintf_s(result, ARRAYSIZE(result), L"%04X:%08X", language, keyboard);
    }
    if (input_locale && size) wcsncpy_s(input_locale, size, result, _TRUNCATE);
    if (klid) *klid = keyboard;
    if (langid) *langid = language;
}

#if ASB_IS_ARM64
/* ARM Windows HCS exposes no vTPM, so the Windows 11 Setup hardware checks are
   bypassed via the HKLM\SYSTEM\Setup\LabConfig keys (x64 gets a real vTPM and
   needs none). Writes one <RunSynchronousCommand> per key into an already-open
   <RunSynchronous>, starting at <Order> `order`; returns the next order. */
static int write_tpm_bypass_commands(FILE *f, int order)
{
    static const wchar_t *const keys[] = {
        L"BypassTPMCheck", L"BypassSecureBootCheck", L"BypassRAMCheck",
        L"BypassStorageCheck", L"BypassCPUCheck"
    };
    int i;
    for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v %s /t REG_DWORD /d 1 /f</Path>\n"
            L"                </RunSynchronousCommand>\n",
            order++, keys[i]);
    }
    return order;
}
#endif

/* Generate autounattend.xml with credentials and VM name substituted.
   If is_template, adds SecureStartup-FilterDriver to disable BitLocker
   (required for sysprep). */
static BOOL generate_autounattend(const wchar_t *output_path,
                                   const wchar_t *vm_name,
                                   const wchar_t *admin_user,
                                   const wchar_t *b64_password,
                                   BOOL is_template,
                                   BOOL test_mode,
                                   const wchar_t *lang)
{
    FILE *f;
    wchar_t user_xml[1024];
    wchar_t comp_name[16];
    wchar_t input_locale[128];
    get_host_keyboard_settings(input_locale, ARRAYSIZE(input_locale), NULL, NULL);
    if (!xml_escape_text(admin_user, user_xml, ARRAYSIZE(user_xml)))
        return FALSE;
    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    /* windowsPE + specialize (up through Deployment component) */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"windowsPE\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core-WinPE\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <SetupUILanguage><UILanguage>%s</UILanguage></SetupUILanguage>\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Setup\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <UserData>\n"
        L"                <AcceptEula>true</AcceptEula>\n"
        L"                <ProductKey><WillShowUI>Never</WillShowUI></ProductKey>\n"
        L"            </UserData>\n"
        L"            <ImageInstall><OSImage>\n"
        L"                <InstallFrom><MetaData wcm:action=\"add\">\n"
        L"                    <Key>/IMAGE/NAME</Key>\n"
        L"                    <Value>Windows 11 Pro</Value>\n"
        L"                </MetaData></InstallFrom>\n"
        L"                <InstallTo><DiskID>0</DiskID><PartitionID>3</PartitionID></InstallTo>\n"
        L"            </OSImage></ImageInstall>\n"
        L"            <DiskConfiguration><Disk wcm:action=\"add\">\n"
        L"                <DiskID>0</DiskID><WillWipeDisk>true</WillWipeDisk>\n"
        L"                <CreatePartitions>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>1</Order><Type>EFI</Type><Size>260</Size></CreatePartition>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>2</Order><Type>MSR</Type><Size>16</Size></CreatePartition>\n"
        L"                    <CreatePartition wcm:action=\"add\"><Order>3</Order><Type>Primary</Type><Extend>true</Extend></CreatePartition>\n"
        L"                </CreatePartitions>\n"
        L"                <ModifyPartitions>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>1</Order><PartitionID>1</PartitionID><Format>FAT32</Format><Label>System</Label></ModifyPartition>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>2</Order><PartitionID>2</PartitionID></ModifyPartition>\n"
        L"                    <ModifyPartition wcm:action=\"add\"><Order>3</Order><PartitionID>3</PartitionID><Format>NTFS</Format><Label>Windows</Label><Letter>C</Letter></ModifyPartition>\n"
        L"                </ModifyPartitions>\n"
        L"            </Disk></DiskConfiguration>\n",
        lang, input_locale, lang, lang, lang);

#if ASB_IS_ARM64
    /* windowsPE Setup pass — bypass keys must run before the Win11 appraiser. */
    fwprintf(f, L"            <RunSynchronous>\n");
    write_tpm_bypass_commands(f, 1);
    fwprintf(f, L"            </RunSynchronous>\n");
#endif

    fwprintf(f,
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        comp_name);

    {
        int order = 2;
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            L"                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fwprintf(f,
                L"                <RunSynchronousCommand wcm:action=\"add\">\n"
                L"                    <Order>%d</Order>\n"
                L"                    <Path>bcdedit /set testsigning on</Path>\n"
                L"                </RunSynchronousCommand>\n", order++);
        }
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>cmd /c mkdir C:\\Windows\\Setup\\Scripts</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>cmd /c for %%d in (D E F G H I J) do @if exist %%d:\\SetupComplete.cmd copy /Y %%d:\\SetupComplete.cmd C:\\Windows\\Setup\\Scripts\\</Path>\n"
            L"                </RunSynchronousCommand>\n",
            order, order + 1);
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n");

    /* Template: disable BitLocker so sysprep can run */
    if (is_template) {
        fwprintf(f,
            L"        <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\"\n"
            L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <PreventDeviceEncryption>true</PreventDeviceEncryption>\n"
            L"        </component>\n");
    }

    fwprintf(f,
        L"    </settings>\n"
        L"\n");

    if (is_template) {
        /* Template: boot into audit mode, run sysprep from auditUser pass.
           No user account or OOBE needed. */
        fwprintf(f,
            L"    <settings pass=\"oobeSystem\">\n"
            L"        <component name=\"Microsoft-Windows-Deployment\"\n"
            L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <Reseal>\n"
            L"                <Mode>Audit</Mode>\n"
            L"            </Reseal>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"\n"
            L"    <settings pass=\"auditUser\">\n"
            L"        <component name=\"Microsoft-Windows-Deployment\"\n"
            L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <RunSynchronous>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>1</Order>\n"
            L"                    <Description>Generalize VM and shut down for templating</Description>\n"
            L"                    <Path>C:\\Windows\\System32\\Sysprep\\sysprep.exe /generalize /oobe /shutdown /mode:vm</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"            </RunSynchronous>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"</unattend>\n");
    } else {
        /* Normal VM: full OOBE with user account, AutoLogon, agent install */
        fwprintf(f,
            L"    <settings pass=\"oobeSystem\">\n"
            L"        <component name=\"Microsoft-Windows-International-Core\"\n"
            L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <InputLocale>%s</InputLocale>\n"
            L"            <SystemLocale>%s</SystemLocale>\n"
            L"            <UILanguage>%s</UILanguage>\n"
            L"            <UserLocale>%s</UserLocale>\n"
            L"        </component>\n"
            L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
            L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <OOBE>\n"
            L"                <HideEULAPage>true</HideEULAPage>\n"
            L"                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
            L"                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
            L"                <ProtectYourPC>3</ProtectYourPC>\n"
            L"            </OOBE>\n"
            L"            <UserAccounts><LocalAccounts>\n"
            L"                <LocalAccount wcm:action=\"add\">\n"
            L"                    <Name>%s</Name>\n"
            L"                    <Group>Administrators</Group>\n"
            L"                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
            L"                </LocalAccount>\n"
            L"            </LocalAccounts></UserAccounts>\n"
            L"            <AutoLogon>\n"
            L"                <Enabled>true</Enabled>\n"
            L"                <Username>%s</Username>\n"
            L"                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
            L"                <LogonCount>1</LogonCount>\n"
            L"            </AutoLogon>\n"
            L"            <FirstLogonCommands>\n"
            L"                <SynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>1</Order>\n"
            L"                    <Description>Run AppSandbox setup</Description>\n"
            L"                    <CommandLine>cmd /c \"for %%d in (D E F G H I J) do @if exist %%d:\\setup.cmd call %%d:\\setup.cmd\"</CommandLine>\n"
            L"                    <RequiresUserInput>false</RequiresUserInput>\n"
            L"                </SynchronousCommand>\n"
            L"            </FirstLogonCommands>\n"
            L"        </component>\n"
            L"    </settings>\n"
            L"</unattend>\n",
            input_locale, lang, lang, lang,
            user_xml, b64_password,
            user_xml, b64_password);
    }

    fclose(f);
    return TRUE;
}

/* ---- ISO creation via IMAPI2 ---- */

/* GUIDs for IMAPI2 file system image COM objects */
static const GUID CLSID_MsftFSImage =
    {0x2C941FC5, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};
static const GUID IID_IFSImage =
    {0x2C941FE1, 0x975B, 0x59BE, {0xA9, 0x60, 0x9A, 0x2A, 0x26, 0x28, 0x53, 0xA5}};

/* Write an IStream to a file */
static HRESULT write_stream_to_file(IStream *stream, const wchar_t *path)
{
    HANDLE hFile;
    BYTE buf[65536];
    ULONG bytes_read;
    DWORD bytes_written;
    HRESULT hr;
    LARGE_INTEGER zero;

    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return HRESULT_FROM_WIN32(GetLastError());

    zero.QuadPart = 0;
    stream->lpVtbl->Seek(stream, zero, STREAM_SEEK_SET, NULL);

    hr = S_OK;
    for (;;) {
        hr = stream->lpVtbl->Read(stream, buf, sizeof(buf), &bytes_read);
        if (FAILED(hr)) break;                        /* propagate Read failure */
        if (bytes_read == 0) { hr = S_OK; break; }    /* clean end of stream */
        if (!WriteFile(hFile, buf, bytes_read, &bytes_written, NULL) ||
            bytes_written != bytes_read) {
            DWORD err = GetLastError();
            hr = (err != ERROR_SUCCESS) ? HRESULT_FROM_WIN32(err)
                                        : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
            break;
        }
    }

    CloseHandle(hFile);
    return hr;
}

/* Build an ISO9660+Joliet image from a staging directory using IMAPI2 */
static HRESULT create_iso_from_dir(const wchar_t *iso_path, const wchar_t *staging_dir,
                                    const wchar_t *volume_label)
{
    IFileSystemImage *pImage = NULL;
    IFsiDirectoryItem *pRoot = NULL;
    IFileSystemImageResult *pResult = NULL;
    IStream *pStream = NULL;
    BSTR bstrDir = NULL, bstrLabel = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MsftFSImage, NULL, CLSCTX_ALL,
                          &IID_IFSImage, (void **)&pImage);
    if (FAILED(hr)) {
        ui_log(L"Error: CoCreateInstance(MsftFileSystemImage) failed (0x%08X)", hr);
        return hr;
    }

    /* ISO 9660 + Joliet (needed for long filenames like autounattend.xml) */
    pImage->lpVtbl->put_FileSystemsToCreate(pImage,
        FsiFileSystemISO9660 | FsiFileSystemJoliet);

    bstrLabel = SysAllocString(volume_label);
    pImage->lpVtbl->put_VolumeName(pImage, bstrLabel);

    hr = pImage->lpVtbl->get_Root(pImage, &pRoot);
    if (FAILED(hr)) {
        ui_log(L"Error: get_Root failed (0x%08X)", hr);
        goto cleanup;
    }

    bstrDir = SysAllocString(staging_dir);
    hr = pRoot->lpVtbl->AddTree(pRoot, bstrDir, VARIANT_FALSE);
    if (FAILED(hr)) {
        ui_log(L"Error: AddTree failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = pImage->lpVtbl->CreateResultImage(pImage, &pResult);
    if (FAILED(hr)) {
        ui_log(L"Error: CreateResultImage failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = pResult->lpVtbl->get_ImageStream(pResult, &pStream);
    if (FAILED(hr)) {
        ui_log(L"Error: get_ImageStream failed (0x%08X)", hr);
        goto cleanup;
    }

    hr = write_stream_to_file(pStream, iso_path);

cleanup:
    if (pStream) pStream->lpVtbl->Release(pStream);
    if (pResult) pResult->lpVtbl->Release(pResult);
    if (pRoot) pRoot->lpVtbl->Release(pRoot);
    if (pImage) pImage->lpVtbl->Release(pImage);
    SysFreeString(bstrDir);
    SysFreeString(bstrLabel);
    return hr;
}

/* Remove a directory and all files in it (two levels deep for subdirs like drivers\) */
static void remove_staging_dir(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
                (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                continue;
            swprintf_s(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* Recurse one level for subdirectories (e.g. drivers\) */
                remove_staging_dir(full);
            } else {
                DeleteFileW(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir, BOOL ssh_enabled);

/* ---- SSH MSI download / cache ---- */

BOOL ensure_ssh_msi_cached(wchar_t *msi_path_out, int max_chars)
{
    wchar_t exe_dir[MAX_PATH], *slash;

    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = L'\0';

    swprintf_s(msi_path_out, max_chars, L"%s\\%s", exe_dir, SSH_MSI_NAME);

    /* Already cached? */
    if (GetFileAttributesW(msi_path_out) != INVALID_FILE_ATTRIBUTES)
        return TRUE;

    ui_log(L"Downloading OpenSSH MSI...");
    if (URLDownloadToFileW(NULL, SSH_MSI_URL, msi_path_out, 0, NULL) == S_OK) {
        ui_log(L"OpenSSH MSI downloaded.");
        return TRUE;
    }

    ui_log(L"Failed to download OpenSSH MSI.");
    return FALSE;
}

HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode,
                              BOOL ssh_enabled,
                              const wchar_t *lang)
{
    wchar_t dir[MAX_PATH];
    wchar_t staging[MAX_PATH];
    wchar_t b64_pass[512];
    wchar_t file_path[MAX_PATH];
    wchar_t *last_slash;
    HRESULT hr;

    if (!iso_path || !vm_name || !admin_user || !admin_pass)
        return E_INVALIDARG;

    /* Get output directory from iso_path */
    wcscpy_s(dir, MAX_PATH, iso_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));
        return E_FAIL;
    }
    SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));

    /* Delete stale ISO */
    if (GetFileAttributesW(iso_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(iso_path);

    /* Create staging directory */
    swprintf_s(staging, MAX_PATH, L"%s\\_iso_staging", dir);
    CreateDirectoryW(staging, NULL);

    ui_log(L"Creating resources ISO...");

    /* autounattend.xml */
    swprintf_s(file_path, MAX_PATH, L"%s\\autounattend.xml", staging);
    if (!generate_autounattend(file_path, vm_name, admin_user, b64_pass, is_template, test_mode, lang))
        ui_log(L"Warning: failed to write autounattend.xml");

    /* Agent + input helper + VDD driver files + SSH MSI */
    stage_agent_and_setup(staging, res_dir, ssh_enabled);

    /* setup.cmd — first-logon script: installs agent service.
       This runs from the ISO drive, so %~dp0 is the ISO root. */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    {
        FILE *cmd;
        if (_wfopen_s(&cmd, file_path, L"w") == 0 && cmd) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === setup.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install guest agent\r\n"
                "if exist \"%~dp0appsandbox-agent.exe\" (\r\n"
                "    copy /Y \"%~dp0appsandbox-agent.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-input.exe\" copy /Y \"%~dp0appsandbox-input.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-displays.exe\" copy /Y \"%~dp0appsandbox-displays.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard.exe\" copy /Y \"%~dp0appsandbox-clipboard.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard-reader.exe\" copy /Y \"%~dp0appsandbox-clipboard-reader.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-audio.exe\" copy /Y \"%~dp0appsandbox-audio.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    \"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "echo === setup.cmd finished === >> \"%LOG%\"\r\n",
                cmd);
            fclose(cmd);
        } else {
            ui_log(L"Warning: failed to write setup.cmd");
        }
    }

    /* SetupComplete.cmd — runs as SYSTEM after setup, before first logon.
       Copied to C:\Windows\Setup\Scripts\ during specialize pass.
       Installs VDD driver (cert + test signing + devcon) + optionally OpenSSH. */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    {
        FILE *sc;
        if (_wfopen_s(&sc, file_path, L"w") == 0 && sc) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Find ISO drive\r\n"
                "set ISODRV=\r\n"
                "for %%d in (D E F G H I J) do @if exist %%d:\\drivers\\AppSandboxVDD.inf set ISODRV=%%d:\r\n"
                "if \"%ISODRV%\"==\"\" (\r\n"
                "    echo [VDD] Could not find ISO drive with driver files >> \"%LOG%\"\r\n"
                "    goto :done\r\n"
                ")\r\n"
                "echo [VDD] Found driver files on %ISODRV% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Enable test signing\r\n"
                "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
                "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
                "\r\n"
                "REM Install certificate\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" (\r\n"
                "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
                "    certutil -addstore Root \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                "    certutil -f -addstore TrustedPublisher \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "\r\n"
                "REM Install driver with devcon\r\n"
                "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "dir \"%ISODRV%\\drivers\\\" >> \"%LOG%\" 2>&1\r\n"
                "\"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
                "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Disable display sleep so IDD swap chain stays alive\r\n"
                "powercfg /change monitor-timeout-ac 0\r\n"
                "powercfg /change monitor-timeout-dc 0\r\n"
                "powercfg /change standby-timeout-ac 0\r\n"
                "powercfg /change standby-timeout-dc 0\r\n"
                "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install VAD driver with devcon\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" (\r\n"
                "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "    \"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
                "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                ")\r\n"
                "\r\n",
                sc);

            if (ssh_enabled) {
                fputs(
                    "REM Install OpenSSH Server\r\n"
                    "if exist \"%ISODRV%\\" SSH_MSI_NAME_A "\" (\r\n"
                    "    echo [SSH] Installing OpenSSH Server... >> \"%LOG%\"\r\n"
                    "    msiexec /i \"%ISODRV%\\" SSH_MSI_NAME_A "\" /qn /norestart >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] msiexec exit code: %errorlevel% >> \"%LOG%\"\r\n"
                    "    sc config sshd start= auto >> \"%LOG%\" 2>&1\r\n"
                    "    net start sshd >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] Done >> \"%LOG%\"\r\n"
                    ")\r\n"
                    "\r\n",
                    sc);
            }

            fputs(
                ":done\r\n"
                "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
                sc);
            fclose(sc);
        }
    }

    /* Build ISO from staging directory */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = create_iso_from_dir(iso_path, staging, L"APPSETUP");
    CoUninitialize();

    /* Clean up staging directory */
    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    remove_staging_dir(staging);

    if (SUCCEEDED(hr))
        ui_log(L"Resources ISO created: %s", iso_path);

    return hr;
}

/* ---- Template VM resources ---- */


/* Generate unattend.xml for instances created from templates.
   Post-sysprep mini-setup uses "unattend.xml" (not "autounattend.xml")
   when searching removable media. Contains specialize + oobeSystem passes. */
static BOOL generate_unattend_instance(const wchar_t *output_path,
                                        const wchar_t *vm_name,
                                        const wchar_t *admin_user,
                                        const wchar_t *b64_password,
                                        const wchar_t *lang)
{
    FILE *f;
    BOOL written;
    wchar_t user_xml[1024];
    wchar_t comp_name[16];
    wchar_t input_locale[128];
    get_host_keyboard_settings(input_locale, ARRAYSIZE(input_locale), NULL, NULL);
    if (!xml_escape_text(admin_user, user_xml, ARRAYSIZE(user_xml)))
        return FALSE;
    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ExtendOSPartition><Extend>true</Extend></ExtendOSPartition>\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>2</Order>\n"
        L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>3</Order>\n"
        L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>4</Order>\n"
        L"                    <Path>cmd /c mkdir C:\\Windows\\Setup\\Scripts</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>5</Order>\n"
        L"                    <Path>cmd /c for %%d in (D E F G H I J) do @if exist %%d:\\SetupComplete.cmd copy /Y %%d:\\SetupComplete.cmd C:\\Windows\\Setup\\Scripts\\</Path>\n"
        L"                </RunSynchronousCommand>\n"
#if ASB_IS_ARM64
        /* ARM Windows HCS has no vTPM — Windows 11 LabConfig hardware-check bypass keys. */
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>6</Order>\n"
        L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>7</Order>\n"
        L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>8</Order>\n"
        L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>9</Order>\n"
        L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassStorageCheck /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>10</Order>\n"
        L"                    <Path>reg add HKLM\\SYSTEM\\Setup\\LabConfig /v BypassCPUCheck /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n"
#endif
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <OOBE>\n"
        L"                <HideEULAPage>true</HideEULAPage>\n"
        L"                <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n"
        L"                <HideWirelessSetupInOOBE>true</HideWirelessSetupInOOBE>\n"
        L"                <ProtectYourPC>3</ProtectYourPC>\n"
        L"            </OOBE>\n"
        L"            <UserAccounts><LocalAccounts>\n"
        L"                <LocalAccount wcm:action=\"add\">\n"
        L"                    <Name>%s</Name>\n"
        L"                    <Group>Administrators</Group>\n"
        L"                    <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                </LocalAccount>\n"
        L"            </LocalAccounts></UserAccounts>\n"
        L"            <AutoLogon>\n"
        L"                <Enabled>true</Enabled>\n"
        L"                <Username>%s</Username>\n"
        L"                <Password><Value>%s</Value><PlainText>false</PlainText></Password>\n"
        L"                <LogonCount>1</LogonCount>\n"
        L"            </AutoLogon>\n"
        L"            <FirstLogonCommands>\n"
        L"                <SynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Description>Run AppSandbox setup</Description>\n"
        L"                    <CommandLine>cmd /c \"for %%d in (D E F G H I J) do @if exist %%d:\\setup.cmd call %%d:\\setup.cmd\"</CommandLine>\n"
        L"                    <RequiresUserInput>false</RequiresUserInput>\n"
        L"                </SynchronousCommand>\n"
        L"            </FirstLogonCommands>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n",
        comp_name,
        input_locale, lang, lang, lang,
        user_xml, b64_password,
        user_xml, b64_password);

    written = !ferror(f);
    if (fclose(f) != 0)
        written = FALSE;
    return written;
}

/* Helper: copy agent exe to staging and write setup.cmd */
static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir, BOOL ssh_enabled)
{
    wchar_t src_path[MAX_PATH], file_path[MAX_PATH];
    BOOL found_agent = FALSE;

    /* appsandbox-agent.exe */
    if (res_dir) {
        swprintf_s(src_path, MAX_PATH, L"%s\\appsandbox-agent.exe", res_dir);
        if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
            found_agent = TRUE;
    }
    if (!found_agent) {
        wchar_t exe_dir[MAX_PATH];
        wchar_t *slash;
        GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
        slash = wcsrchr(exe_dir, L'\\');
        if (slash) *slash = L'\0';
        swprintf_s(src_path, MAX_PATH, L"%s\\appsandbox-agent.exe", exe_dir);
        if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
            found_agent = TRUE;
    }
    if (found_agent) {
        wchar_t input_src[MAX_PATH], input_dst[MAX_PATH];
        swprintf_s(file_path, MAX_PATH, L"%s\\appsandbox-agent.exe", staging);
        if (!CopyFileW(src_path, file_path, FALSE))
            ui_log(L"Warning: failed to copy appsandbox-agent.exe (error %lu)", GetLastError());

        /* appsandbox-input.exe — same directory as agent */
        wcscpy_s(input_src, MAX_PATH, src_path);
        {
            wchar_t *s = wcsrchr(input_src, L'\\');
            if (s) *(s + 1) = L'\0';
        }
        wcscat_s(input_src, MAX_PATH, L"appsandbox-input.exe");
        swprintf_s(input_dst, MAX_PATH, L"%s\\appsandbox-input.exe", staging);
        if (GetFileAttributesW(input_src) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileW(input_src, input_dst, FALSE))
                ui_log(L"Warning: failed to copy appsandbox-input.exe (error %lu)", GetLastError());
            else
                ui_log(L"Staged appsandbox-input.exe for ISO.");
        } else {
            ui_log(L"Warning: appsandbox-input.exe not found at %s", input_src);
        }

        /* appsandbox-displays.exe — same directory as agent */
        {
            wchar_t displays_src[MAX_PATH], displays_dst[MAX_PATH];
            wcscpy_s(displays_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(displays_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(displays_src, MAX_PATH, L"appsandbox-displays.exe");
            swprintf_s(displays_dst, MAX_PATH, L"%s\\appsandbox-displays.exe", staging);
            if (GetFileAttributesW(displays_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(displays_src, displays_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-displays.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-displays.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-displays.exe not found at %s", displays_src);
            }
        }

        /* appsandbox-clipboard.exe — same directory as agent */
        {
            wchar_t clip_src[MAX_PATH], clip_dst[MAX_PATH];
            wcscpy_s(clip_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(clip_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(clip_src, MAX_PATH, L"appsandbox-clipboard.exe");
            swprintf_s(clip_dst, MAX_PATH, L"%s\\appsandbox-clipboard.exe", staging);
            if (GetFileAttributesW(clip_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(clip_src, clip_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-clipboard.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-clipboard.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-clipboard.exe not found at %s", clip_src);
            }
        }

        /* appsandbox-clipboard-reader.exe — same directory as agent */
        {
            wchar_t reader_src[MAX_PATH], reader_dst[MAX_PATH];
            wcscpy_s(reader_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(reader_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(reader_src, MAX_PATH, L"appsandbox-clipboard-reader.exe");
            swprintf_s(reader_dst, MAX_PATH, L"%s\\appsandbox-clipboard-reader.exe", staging);
            if (GetFileAttributesW(reader_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(reader_src, reader_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-clipboard-reader.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-clipboard-reader.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-clipboard-reader.exe not found at %s", reader_src);
            }
        }

        /* appsandbox-audio.exe — same directory as agent */
        {
            wchar_t audio_src[MAX_PATH], audio_dst[MAX_PATH];
            wcscpy_s(audio_src, MAX_PATH, input_src);
            {
                wchar_t *s = wcsrchr(audio_src, L'\\');
                if (s) *(s + 1) = L'\0';
            }
            wcscat_s(audio_src, MAX_PATH, L"appsandbox-audio.exe");
            swprintf_s(audio_dst, MAX_PATH, L"%s\\appsandbox-audio.exe", staging);
            if (GetFileAttributesW(audio_src) != INVALID_FILE_ATTRIBUTES) {
                if (!CopyFileW(audio_src, audio_dst, FALSE))
                    ui_log(L"Warning: failed to copy appsandbox-audio.exe (error %lu)", GetLastError());
                else
                    ui_log(L"Staged appsandbox-audio.exe for ISO.");
            } else {
                ui_log(L"Warning: appsandbox-audio.exe not found at %s", audio_src);
            }
        }
    } else {
        ui_log(L"Warning: appsandbox-agent.exe not found");
    }

    /* VDD driver files — copy to drivers\ subdirectory if available */
    {
        wchar_t drivers_staging[MAX_PATH];
        wchar_t vdd_dir[MAX_PATH];
        BOOL found_vdd = FALSE;
        const wchar_t *vdd_files[] = {
            L"AppSandboxVDD.dll", L"AppSandboxVDD.inf",
            L"AppSandboxVDD.cat", L"AppSandboxVDD.cer"
        };
        int vf;
        wchar_t vdd_src[MAX_PATH];

        swprintf_s(drivers_staging, MAX_PATH, L"%s\\drivers", staging);

        if (res_dir) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            wchar_t exe_dir2[MAX_PATH];
            wchar_t *slash2;
            GetModuleFileNameW(NULL, exe_dir2, MAX_PATH);
            slash2 = wcsrchr(exe_dir2, L'\\');
            if (slash2) *slash2 = L'\0';
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", exe_dir2);
            swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
            if (!found_vdd) {
                wcscpy_s(vdd_dir, MAX_PATH, exe_dir2);
                swprintf_s(vdd_src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
                if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                    found_vdd = TRUE;
            }
        }
        if (found_vdd) {
            CreateDirectoryW(drivers_staging, NULL);
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(vdd_src, MAX_PATH, L"%s\\%s", vdd_dir, vdd_files[vf]);
                swprintf_s(file_path, MAX_PATH, L"%s\\%s", drivers_staging, vdd_files[vf]);
                if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                    CopyFileW(vdd_src, file_path, FALSE);
            }
            /* devcon.exe — alongside VDD driver files */
            swprintf_s(vdd_src, MAX_PATH, L"%s\\devcon.exe", vdd_dir);
            swprintf_s(file_path, MAX_PATH, L"%s\\devcon.exe", drivers_staging);
            if (GetFileAttributesW(vdd_src) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(vdd_src, file_path, FALSE);
        }
    }

    /* VAD driver files — copy to drivers\ subdirectory if available */
    {
        wchar_t drivers_staging[MAX_PATH];
        wchar_t vad_dir[MAX_PATH];
        BOOL found_vad = FALSE;
        const wchar_t *vad_files[] = {
            L"AppSandboxVAD.sys", L"AppSandboxVAD.inf",
            L"AppSandboxVAD.cat", L"AppSandboxVAD.cer"
        };
        int vf;
        wchar_t vad_src[MAX_PATH];

        swprintf_s(drivers_staging, MAX_PATH, L"%s\\drivers", staging);

        if (res_dir) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            wchar_t exe_dir2[MAX_PATH];
            wchar_t *slash2;
            GetModuleFileNameW(NULL, exe_dir2, MAX_PATH);
            slash2 = wcsrchr(exe_dir2, L'\\');
            if (slash2) *slash2 = L'\0';
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", exe_dir2);
            swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
            if (!found_vad) {
                wcscpy_s(vad_dir, MAX_PATH, exe_dir2);
                swprintf_s(vad_src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
                if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                    found_vad = TRUE;
            }
        }
        if (found_vad) {
            CreateDirectoryW(drivers_staging, NULL);
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(vad_src, MAX_PATH, L"%s\\%s", vad_dir, vad_files[vf]);
                swprintf_s(file_path, MAX_PATH, L"%s\\%s", drivers_staging, vad_files[vf]);
                if (GetFileAttributesW(vad_src) != INVALID_FILE_ATTRIBUTES)
                    CopyFileW(vad_src, file_path, FALSE);
            }
        }
    }

    /* SSH MSI — copy to staging root if ssh_enabled */
    if (ssh_enabled) {
        wchar_t msi_path[MAX_PATH];
        if (ensure_ssh_msi_cached(msi_path, MAX_PATH)) {
            swprintf_s(file_path, MAX_PATH, L"%s\\%s", staging, SSH_MSI_NAME);
            if (!CopyFileW(msi_path, file_path, FALSE))
                ui_log(L"Warning: failed to copy SSH MSI (error %lu)", GetLastError());
            else
                ui_log(L"Staged %s for ISO.", SSH_MSI_NAME);
        }
    }

    /* setup.cmd — first-logon script: installs agent service only */
    swprintf_s(file_path, MAX_PATH, L"%s\\setup.cmd", staging);
    {
        FILE *cmd;
        if (_wfopen_s(&cmd, file_path, L"w") == 0 && cmd) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === instance setup.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install guest agent\r\n"
                "if exist \"%~dp0appsandbox-agent.exe\" (\r\n"
                "    copy /Y \"%~dp0appsandbox-agent.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-input.exe\" copy /Y \"%~dp0appsandbox-input.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-displays.exe\" copy /Y \"%~dp0appsandbox-displays.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard.exe\" copy /Y \"%~dp0appsandbox-clipboard.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-clipboard-reader.exe\" copy /Y \"%~dp0appsandbox-clipboard-reader.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    if exist \"%~dp0appsandbox-audio.exe\" copy /Y \"%~dp0appsandbox-audio.exe\" \"%SystemRoot%\\AppSandbox\\\" >> \"%LOG%\" 2>&1\r\n"
                "    \"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "echo === instance setup.cmd finished === >> \"%LOG%\"\r\n",
                cmd);
            fclose(cmd);
        }
    }

    /* SetupComplete.cmd — runs as SYSTEM after setup, before first logon.
       Copied to C:\Windows\Setup\Scripts\ during specialize pass.
       Installs VDD driver (cert + test signing + devcon) + optionally OpenSSH. */
    swprintf_s(file_path, MAX_PATH, L"%s\\SetupComplete.cmd", staging);
    {
        FILE *sc;
        if (_wfopen_s(&sc, file_path, L"w") == 0 && sc) {
            fputs(
                "@echo off\r\n"
                "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
                "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
                "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Find ISO drive\r\n"
                "set ISODRV=\r\n"
                "for %%d in (D E F G H I J) do @if exist %%d:\\drivers\\AppSandboxVDD.inf set ISODRV=%%d:\r\n"
                "if \"%ISODRV%\"==\"\" (\r\n"
                "    echo [VDD] Could not find ISO drive with driver files >> \"%LOG%\"\r\n"
                "    goto :done\r\n"
                ")\r\n"
                "echo [VDD] Found driver files on %ISODRV% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Enable test signing\r\n"
                "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
                "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
                "\r\n"
                "REM Install certificate\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" (\r\n"
                "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
                "    certutil -addstore Root \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                "    certutil -f -addstore TrustedPublisher \"%ISODRV%\\drivers\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "\r\n"
                "REM Install driver with devcon\r\n"
                "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "dir \"%ISODRV%\\drivers\\\" >> \"%LOG%\" 2>&1\r\n"
                "\"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
                "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Disable display sleep so IDD swap chain stays alive\r\n"
                "powercfg /change monitor-timeout-ac 0\r\n"
                "powercfg /change monitor-timeout-dc 0\r\n"
                "powercfg /change standby-timeout-ac 0\r\n"
                "powercfg /change standby-timeout-dc 0\r\n"
                "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
                "\r\n"
                "REM Install VAD driver with devcon\r\n"
                "if exist \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" (\r\n"
                "    echo [VAD] Installing driver with devcon... >> \"%LOG%\"\r\n"
                "    \"%ISODRV%\\drivers\\devcon.exe\" install \"%ISODRV%\\drivers\\AppSandboxVAD.inf\" Root\\AppSandboxVAD >> \"%LOG%\" 2>&1\r\n"
                "    echo [VAD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
                ")\r\n"
                "\r\n",
                sc);

            if (ssh_enabled) {
                fputs(
                    "REM Install OpenSSH Server\r\n"
                    "if exist \"%ISODRV%\\" SSH_MSI_NAME_A "\" (\r\n"
                    "    echo [SSH] Installing OpenSSH Server... >> \"%LOG%\"\r\n"
                    "    msiexec /i \"%ISODRV%\\" SSH_MSI_NAME_A "\" /qn /norestart >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] msiexec exit code: %errorlevel% >> \"%LOG%\"\r\n"
                    "    sc config sshd start= auto >> \"%LOG%\" 2>&1\r\n"
                    "    net start sshd >> \"%LOG%\" 2>&1\r\n"
                    "    echo [SSH] Done >> \"%LOG%\"\r\n"
                    ")\r\n"
                    "\r\n",
                    sc);
            }

            fputs(
                ":done\r\n"
                "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
                sc);
            fclose(sc);
        }
    }
}


HRESULT iso_create_instance_resources(const wchar_t *iso_path,
                                       const wchar_t *vm_name,
                                       const wchar_t *admin_user,
                                       wchar_t *admin_pass,
                                       const wchar_t *res_dir,
                                       BOOL ssh_enabled,
                                       const wchar_t *lang)
{
    wchar_t dir[MAX_PATH];
    wchar_t staging[MAX_PATH];
    wchar_t file_path[MAX_PATH];
    wchar_t b64_pass[512];
    wchar_t *last_slash;
    HRESULT hr;

    if (!iso_path || !vm_name || !admin_user || !admin_pass)
        return E_INVALIDARG;

    /* Get output directory from iso_path */
    wcscpy_s(dir, MAX_PATH, iso_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) *last_slash = L'\0';

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));
        return E_FAIL;
    }
    SecureZeroMemory(admin_pass, wcslen(admin_pass) * sizeof(wchar_t));

    /* Delete stale ISO */
    if (GetFileAttributesW(iso_path) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(iso_path);

    /* Create staging directory */
    swprintf_s(staging, MAX_PATH, L"%s\\_iso_staging", dir);
    CreateDirectoryW(staging, NULL);

    ui_log(L"Creating instance resources ISO...");

    /* unattend.xml (NOT autounattend.xml — post-sysprep mini-setup uses this name) */
    swprintf_s(file_path, MAX_PATH, L"%s\\unattend.xml", staging);
    if (!generate_unattend_instance(file_path, vm_name, admin_user, b64_pass, lang)) {
        ui_log(L"Error: failed to write instance unattend.xml");
        SecureZeroMemory(b64_pass, sizeof(b64_pass));
        remove_staging_dir(staging);
        return E_FAIL;
    }

    /* Agent exe + setup.cmd + SSH MSI */
    stage_agent_and_setup(staging, res_dir, ssh_enabled);

    /* Build ISO */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = create_iso_from_dir(iso_path, staging, L"APPSETUP");
    CoUninitialize();

    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    remove_staging_dir(staging);

    if (SUCCEEDED(hr))
        ui_log(L"Instance resources ISO created: %s", iso_path);

    return hr;
}

/* ---- VHDX-First VM Creation ---- */

#include "gpu_enum.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

/* Generate unattend.xml for VHDX-first boot.
   No windowsPE pass (DISM already applied the image).
   Only specialize + oobeSystem for first-boot mini-setup.
   Delegates to the shared platform-neutral generator (tools/provision/win_provision.c)
   so the Windows-host and macOS-host builders stage byte-identical answer files. The
   wide inputs are converted to UTF-8 and the file is opened "wb" (the generator writes
   the UTF-8 BOM itself). */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode,
                             const wchar_t *lang,
                             const wchar_t *input_locale)
{
    FILE *f;
    char vm_u[64], user_u[256], pass_u[512], lang_u[32], input_u[128];
    int rc;

    if (!output_path || !vm_name || !admin_user || !admin_pass)
        return FALSE;

    /* Convert the wide inputs to UTF-8 for the shared generator. A 0 return means the
       value didn't fit its buffer (WideCharToMultiByte leaves it unterminated), so bail
       rather than emit garbage. Realistic name/user/pass/lang values fit comfortably. */
    if (!WideCharToMultiByte(CP_UTF8, 0, vm_name, -1, vm_u, sizeof vm_u, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, admin_user, -1, user_u, sizeof user_u, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, admin_pass, -1, pass_u, sizeof pass_u, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, lang ? lang : L"en-US", -1, lang_u, sizeof lang_u, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, input_locale ? input_locale : L"", -1,
                             input_u, sizeof input_u, NULL, NULL)) {
        SecureZeroMemory(pass_u, sizeof pass_u);
        return FALSE;
    }

    if (_wfopen_s(&f, output_path, L"wb") != 0 || !f) {
        SecureZeroMemory(pass_u, sizeof pass_u);
        return FALSE;
    }

    rc = asb_provision_unattend(f, vm_u, user_u, pass_u,
                                ASB_IS_ARM64 ? "arm64" : "amd64",
                                test_mode ? 1 : 0, ASB_IS_ARM64, lang_u, input_u);

    fclose(f);
    SecureZeroMemory(pass_u, sizeof pass_u);
    return rc == 0;
}

/* Generate unattend.xml for VHDX-first *template* boot.
   No windowsPE (DISM already applied the image).
   specialize: ComputerName + BypassNRO + testsigning + BitLocker disable
   oobeSystem: Reseal Audit (no user account, no OOBE)
   auditUser:  sysprep /generalize /oobe /shutdown /mode:vm */
BOOL generate_unattend_vhdx_template(const wchar_t *output_path,
                                      const wchar_t *vm_name,
                                      BOOL test_mode)
{
    FILE *f;
    wchar_t comp_name[16];

    if (!output_path || !vm_name)
        return FALSE;

    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f)
        return FALSE;

    /* specialize pass: ComputerName + BypassNRO + testsigning + BitLocker disable */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        comp_name);

    {
        int order = 2;
        fwprintf(f,
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set recoveryenabled No</Path>\n"
            L"                </RunSynchronousCommand>\n"
            L"                <RunSynchronousCommand wcm:action=\"add\">\n"
            L"                    <Order>%d</Order>\n"
            L"                    <Path>bcdedit /set bootstatuspolicy IgnoreAllFailures</Path>\n"
            L"                </RunSynchronousCommand>\n", order, order + 1);
        order += 2;
        if (test_mode) {
            fwprintf(f,
                L"                <RunSynchronousCommand wcm:action=\"add\">\n"
                L"                    <Order>%d</Order>\n"
                L"                    <Path>bcdedit /set testsigning on</Path>\n"
                L"                </RunSynchronousCommand>\n", order++);
        }
#if ASB_IS_ARM64
        write_tpm_bypass_commands(f, order);
#endif
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <PreventDeviceEncryption>true</PreventDeviceEncryption>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n");

    /* oobeSystem: Reseal Audit mode (no user account, boots into audit) */
    /* auditUser: sysprep /generalize /oobe /shutdown /mode:vm */
    fwprintf(f,
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <Reseal>\n"
        L"                <Mode>Audit</Mode>\n"
        L"            </Reseal>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"auditUser\">\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"" ASB_UA_ARCH L"\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Description>Generalize VM and shut down for templating</Description>\n"
        L"                    <Path>C:\\Windows\\System32\\Sysprep\\sysprep.exe /generalize /oobe /shutdown /mode:vm</Path>\n"
        L"                </RunSynchronousCommand>\n"
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n");

    fclose(f);
    return TRUE;
}

/* Generate setup.cmd for VHDX-first boot (agent already on disk).
   Delegates to the shared platform-neutral generator (tools/provision/win_provision.c)
   so the Windows-host and macOS-host builders stage byte-identical scripts. "wb" keeps
   the generator's exact CRLF bytes (no text-mode \r doubling). */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path)
{
    FILE *cmd;
    int rc;
    if (_wfopen_s(&cmd, output_path, L"wb") != 0 || !cmd)
        return FALSE;
    rc = asb_provision_setup_cmd(cmd);
    fclose(cmd);
    return rc == 0;
}

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk).
   Delegates to the shared platform-neutral generator (tools/provision/win_provision.c)
   so the Windows-host and macOS-host builders stage byte-identical scripts. The shared
   script additionally installs the AppSandboxSHM (ivshmem) + NetKVM (virtio-net) PCI
   drivers; both steps are if-exist-guarded and a no-op on a Windows host (no 1af4 PCI
   device present). "wb" keeps the generator's exact CRLF bytes. */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path, BOOL ssh_enabled)
{
    FILE *sc;
    int rc;
    if (_wfopen_s(&sc, output_path, L"wb") != 0 || !sc)
        return FALSE;
    rc = asb_provision_setupcomplete(sc, ssh_enabled ? SSH_MSI_NAME_A : NULL);
    fclose(sc);
    return rc == 0;
}

/* Find the exe directory (directory containing AppSandbox.exe) */
static void get_exe_dir(wchar_t *out, size_t out_len)
{
    wchar_t *slash;
    GetModuleFileNameW(NULL, out, (DWORD)out_len);
    slash = wcsrchr(out, L'\\');
    if (slash) *slash = L'\0';
}

/* Recursively enumerate files in a directory matching semicolon-separated glob filters.
   Writes matching files as manifest entries to the file handle.
   host_prefix: the host-side base path (e.g. C:\Windows\System32\DriverStore\...).
   guest_prefix: the guest-side base path (e.g. \Windows\System32\HostDriverStore\...).
   filter: semicolon-separated patterns (e.g. "*.dll;*.sys;nv*.inf") or empty for all.
   Returns the number of entries written. */
static int enumerate_gpu_share_files(FILE *mf,
                                      const wchar_t *host_prefix,
                                      const wchar_t *guest_prefix,
                                      const wchar_t *filter)
{
    wchar_t pattern[MAX_PATH], host_full[MAX_PATH], guest_full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", host_prefix);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == L'\0' ||
            (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;

        swprintf_s(host_full, MAX_PATH, L"%s\\%s", host_prefix, fd.cFileName);
        swprintf_s(guest_full, MAX_PATH, L"%s\\%s", guest_prefix, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += enumerate_gpu_share_files(mf, host_full, guest_full, filter);
        } else {
            /* Check filter: if filter is empty, accept all; otherwise check each pattern */
            BOOL match = FALSE;
            if (!filter || filter[0] == L'\0') {
                match = TRUE;
            } else {
                /* PathMatchSpecW supports semicolon-separated patterns natively */
                match = PathMatchSpecW(fd.cFileName, filter);
            }
            if (match) {
                fwprintf(mf, L"%s\t%s\n", host_full, guest_full);
                count++;
            }
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return count;
}

/* Generate staging manifest for iso-patch --stage.
   Writes tab-separated source\tdest lines for all files to pre-stage on the VHDX. */
int generate_vhdx_manifest(const wchar_t *manifest_path,
                            const wchar_t *staging_dir,
                            const wchar_t *res_dir,
                            const void *gpu_shares_ptr,
                            BOOL ssh_enabled)
{
    const GpuDriverShareList *gpu_shares = (const GpuDriverShareList *)gpu_shares_ptr;
    FILE *mf;
    int count = 0;
    wchar_t src[MAX_PATH], exe_dir[MAX_PATH];

    if (_wfopen_s(&mf, manifest_path, L"w,ccs=UTF-8") != 0 || !mf)
        return -1;

    get_exe_dir(exe_dir, MAX_PATH);

    /* 1. Staging files: unattend.xml, setup.cmd, SetupComplete.cmd */
    swprintf_s(src, MAX_PATH, L"%s\\unattend.xml", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\Panther\\unattend.xml\n", src);
        count++;
    }

    swprintf_s(src, MAX_PATH, L"%s\\setup.cmd", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\setup.cmd\n", src);
        count++;
    }

    swprintf_s(src, MAX_PATH, L"%s\\SetupComplete.cmd", staging_dir);
    if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
        fwprintf(mf, L"%s\t\\Windows\\Setup\\Scripts\\SetupComplete.cmd\n", src);
        count++;
    }

    /* 2. Agent + input helper executables */
    {
        const wchar_t *bins[] = { L"appsandbox-agent.exe", L"appsandbox-input.exe", L"appsandbox-displays.exe", L"appsandbox-clipboard.exe", L"appsandbox-clipboard-reader.exe", L"appsandbox-audio.exe" };
        int bi;
        for (bi = 0; bi < (int)(sizeof(bins) / sizeof(bins[0])); bi++) {
            BOOL found = FALSE;
            if (res_dir) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", res_dir, bins[bi]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                    found = TRUE;
            }
            if (!found) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", exe_dir, bins[bi]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                    found = TRUE;
            }
            if (found) {
                fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\%s\n", src, bins[bi]);
                count++;
            }
        }
    }

    /* 3. VDD driver files */
    {
        const wchar_t *vdd_files[] = {
            L"AppSandboxVDD.dll", L"AppSandboxVDD.inf",
            L"AppSandboxVDD.cat", L"AppSandboxVDD.cer", L"devcon.exe"
        };
        wchar_t vdd_dir[MAX_PATH];
        BOOL found_vdd = FALSE;
        int vf;

        /* Search for VDD files in res_dir\drivers, exe_dir\drivers, or exe_dir */
        if (res_dir) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            swprintf_s(vdd_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }
        if (!found_vdd) {
            wcscpy_s(vdd_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVDD.dll", vdd_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vdd = TRUE;
        }

        if (found_vdd) {
            for (vf = 0; vf < 5; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", vdd_dir, vdd_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, vdd_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3a. VAD driver files */
    {
        const wchar_t *vad_files[] = {
            L"AppSandboxVAD.sys", L"AppSandboxVAD.inf",
            L"AppSandboxVAD.cat", L"AppSandboxVAD.cer"
        };
        wchar_t vad_dir[MAX_PATH];
        BOOL found_vad = FALSE;
        int vf;

        if (res_dir) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            swprintf_s(vad_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }
        if (!found_vad) {
            wcscpy_s(vad_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxVAD.sys", vad_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_vad = TRUE;
        }

        if (found_vad) {
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", vad_dir, vad_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, vad_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3a-i. AppSandboxSHM (ivshmem shared-memory transport) driver files.
       Harmless on a Windows host (no ivshmem PCI device, so SetupComplete's devcon
       update is a no-op) but staged for parity with the macOS builder and so a
       release-signed Windows build carries the same payload. Existence-gated: if the
       driver isn't built into res_dir\drivers/exe_dir\drivers, nothing is staged. */
    {
        const wchar_t *shm_files[] = {
            L"AppSandboxSHM.sys", L"AppSandboxSHM.inf",
            L"AppSandboxSHM.cat", L"AppSandboxSHM.cer"
        };
        wchar_t shm_dir[MAX_PATH];
        BOOL found_shm = FALSE;
        int vf;

        if (res_dir) {
            swprintf_s(shm_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxSHM.sys", shm_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_shm = TRUE;
        }
        if (!found_shm) {
            swprintf_s(shm_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxSHM.sys", shm_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_shm = TRUE;
        }
        if (!found_shm) {
            wcscpy_s(shm_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\AppSandboxSHM.sys", shm_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_shm = TRUE;
        }

        if (found_shm) {
            for (vf = 0; vf < 4; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", shm_dir, shm_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, shm_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3a-ii. NetKVM (virtio-net NIC) driver files + netkvmp.exe + license. WHQL-signed
       (no cert). No-op on a Windows host (Hyper-V synthetic NIC; no virtio-net device).
       netkvmp.exe must sit beside the INF (the INF CopyFiles-es it). Existence-gated. */
    {
        const wchar_t *net_files[] = {
            L"netkvm.inf", L"netkvm.sys", L"netkvm.cat",
            L"netkvmp.exe", L"virtio-win_license.txt"
        };
        wchar_t net_dir[MAX_PATH];
        BOOL found_net = FALSE;
        int vf;

        if (res_dir) {
            swprintf_s(net_dir, MAX_PATH, L"%s\\drivers", res_dir);
            swprintf_s(src, MAX_PATH, L"%s\\netkvm.inf", net_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_net = TRUE;
        }
        if (!found_net) {
            swprintf_s(net_dir, MAX_PATH, L"%s\\drivers", exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\netkvm.inf", net_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_net = TRUE;
        }
        if (!found_net) {
            wcscpy_s(net_dir, MAX_PATH, exe_dir);
            swprintf_s(src, MAX_PATH, L"%s\\netkvm.inf", net_dir);
            if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
                found_net = TRUE;
        }

        if (found_net) {
            for (vf = 0; vf < 5; vf++) {
                swprintf_s(src, MAX_PATH, L"%s\\%s", net_dir, net_files[vf]);
                if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES) {
                    fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\drivers\\%s\n", src, net_files[vf]);
                    count++;
                }
            }
        }
    }

    /* 3b. SSH MSI */
    if (ssh_enabled) {
        wchar_t msi_path[MAX_PATH];
        if (ensure_ssh_msi_cached(msi_path, MAX_PATH)) {
            fwprintf(mf, L"%s\t\\Windows\\AppSandbox\\%s\n", msi_path, SSH_MSI_NAME);
            count++;
        }
    }

    /* 4. GPU driver files (one line per file, enumerated from host DriverStore paths).
       guest_path from GpuDriverShare is absolute (e.g. "C:\Windows\System32\HostDriverStore\...").
       The manifest dest must be relative to the VHDX root (e.g. "\Windows\...").
       Strip the drive letter prefix ("C:") to make it relative. */
    if (gpu_shares && gpu_shares->count > 0) {
        int i;
        for (i = 0; i < gpu_shares->count; i++) {
            const GpuDriverShare *ds = &gpu_shares->shares[i];
            const wchar_t *rel_guest = ds->guest_path;
            if (GetFileAttributesW(ds->host_path) == INVALID_FILE_ATTRIBUTES)
                continue;
            /* These shares need guest-side provisioning over Plan9. DRS is
               mutable profile data: the agent seeds it with guest permissions
               and excludes the host's runtime lock file. */
            if (_wcsicmp(ds->share_name, L"AppSandbox.GlLayers") == 0 ||
                _wcsicmp(ds->share_name, L"AppSandbox.NvidiaDrs") == 0)
                continue;
            /* Strip "C:" or any drive prefix — keep the leading backslash */
            if (rel_guest[0] != L'\0' && rel_guest[1] == L':')
                rel_guest += 2;
            count += enumerate_gpu_share_files(mf, ds->host_path, rel_guest, ds->file_filter);
        }
    }

    fclose(mf);
    return count;
}

/* ---- Language JSON helpers ---- */

/* Derive language.json path from VHDX path (same directory) */
static void get_language_json_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_len)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_len, L"%s\\language.json", dir);
}

void vm_save_language_json(const wchar_t *vhdx_path, const wchar_t *lang)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char narrow[64];
    get_language_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    WideCharToMultiByte(CP_UTF8, 0, lang, -1, narrow, sizeof(narrow), NULL, NULL);
    fprintf(f, "{\"language\":\"%s\"}\n", narrow);
    fclose(f);
}

BOOL vm_load_language_json(const wchar_t *vhdx_path, wchar_t *lang_out, int lang_out_max)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    get_language_json_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r") != 0 || !f) return FALSE;
    if (fgets(buf, sizeof(buf), f)) {
        /* Parse {"language":"xx-YY"} */
        char *p = strstr(buf, "\"language\":\"");
        if (p) {
            char *start, *end;
            p += 12;  /* skip past "language":" */
            start = p;
            end = strchr(start, '"');
            if (end) {
                *end = '\0';
                MultiByteToWideChar(CP_UTF8, 0, start, -1, lang_out, lang_out_max);
                fclose(f);
                return TRUE;
            }
        }
    }
    fclose(f);
    return FALSE;
}

/* ---- SHA-512 crypt (glibc $6$ format) ----
 *
 * Linux passwd/shadow stores hashed passwords in crypt(3) format. The
 * guest's firstboot plants the admin password via `usermod -p <hash>`,
 * which needs a string in this format — plain text isn't accepted. We
 * can't shell out to `mkpasswd` (not present on Windows), and OpenSSL
 * isn't guaranteed. So implement Ulrich Drepper's SHA-512 crypt
 * algorithm on top of Win32 CNG's SHA-512 primitive.
 *
 * Reference: https://www.akkadia.org/drepper/sha-crypt.html
 *            https://www.akkadia.org/drepper/SHA-crypt.txt
 */

#define SHA512_DIGEST_LEN 64

typedef struct {
    const void *data;
    ULONG       len;
} sha512_part_t;

/* One-shot SHA-512: hash a list of (data, len) parts and write the 64-byte
   digest to `out`. Returns TRUE on success. */
static BOOL sha512_oneshot(const sha512_part_t *parts,
                            int num_parts, BYTE out[SHA512_DIGEST_LEN])
{
    BCRYPT_ALG_HANDLE  hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    NTSTATUS status;
    BOOL ok = FALSE;
    int i;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0);
    if (status != 0) return FALSE;

    status = BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0);
    if (status != 0) goto done;

    for (i = 0; i < num_parts; i++) {
        if (parts[i].len == 0) continue;
        status = BCryptHashData(hHash, (PUCHAR)parts[i].data, parts[i].len, 0);
        if (status != 0) goto done;
    }

    status = BCryptFinishHash(hHash, out, SHA512_DIGEST_LEN, 0);
    if (status == 0) ok = TRUE;

done:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

/* GNU crypt b64 alphabet — note "./" prefix and the rest is digits, upper, lower. */
static const char crypt_b64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* Encode three bytes as four crypt-b64 chars, in the specific order Drepper
   defined. n = number of chars to emit (1..4). */
static void crypt_b64_3(unsigned b2, unsigned b1, unsigned b0, int n, char **out)
{
    unsigned w = (b2 << 16) | (b1 << 8) | b0;
    while (n-- > 0) {
        *(*out)++ = crypt_b64[w & 0x3f];
        w >>= 6;
    }
}

/* Drepper SHA-512 crypt. `key` is the password bytes; `salt` is up to 16
   bytes from the crypt_b64 alphabet. Writes the encoded result (including
   the "$6$<salt>$" prefix) into `out`. `out_size` must be >= 110.
   Returns the number of chars written (not including NUL), or 0 on error. */
static int sha512_crypt(const char *key, size_t key_len,
                         const char *salt, size_t salt_len,
                         char *out, size_t out_size)
{
    /* Algorithm uses two "rolling" SHA-512 inputs: digest A (the per-round
       hash) and digest B (a key-derived seed used to padding-extend A). */
    BYTE alt[SHA512_DIGEST_LEN];   /* digest B (intermediate) */
    BYTE digest[SHA512_DIGEST_LEN]; /* digest A / running hash C */
    BYTE p_seq[SHA512_DIGEST_LEN]; /* DP digest, expanded to P below */
    BYTE s_seq[SHA512_DIGEST_LEN]; /* DS digest, expanded to S below */
    BYTE *P = NULL, *S = NULL;
    int rc = 0;
    size_t cnt;
    int rounds = 5000;  /* default rounds for $6$ */
    char *cp;
    int i;

    sha512_part_t parts[64];
    int n;

    if (key_len > 256 || salt_len > 16 || out_size < 110)
        return 0;

    /* --- Step 4-8: compute digest B = SHA512(key || salt || key) --- */
    n = 0;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    parts[n].data = salt; parts[n].len = (ULONG)salt_len; n++;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    if (!sha512_oneshot(parts, n, alt)) return 0;

    /* --- Step 1-3, 9-12: digest A = SHA512(key, salt, then key-len bytes
       from alt (full 64-byte blocks at a time), then key-bit-pattern feeds.) */
    n = 0;
    parts[n].data = key;  parts[n].len = (ULONG)key_len; n++;
    parts[n].data = salt; parts[n].len = (ULONG)salt_len; n++;

    /* Feed full 64-byte blocks of alt for each 64-byte chunk of key length */
    cnt = key_len;
    while (cnt > SHA512_DIGEST_LEN) {
        parts[n].data = alt; parts[n].len = SHA512_DIGEST_LEN; n++;
        cnt -= SHA512_DIGEST_LEN;
    }
    if (cnt > 0) {
        parts[n].data = alt; parts[n].len = (ULONG)cnt; n++;
    }

    /* For each bit of key_len (low bit first), alternately feed alt or key */
    for (cnt = key_len; cnt > 0; cnt >>= 1) {
        if (cnt & 1) {
            parts[n].data = alt; parts[n].len = SHA512_DIGEST_LEN; n++;
        } else {
            parts[n].data = key; parts[n].len = (ULONG)key_len; n++;
        }
    }

    if (!sha512_oneshot(parts, n, digest)) return 0;

    /* --- Steps 13-15: DP = SHA512(key repeated key_len times). Number of
       feeds equals key length, which can be larger than parts[]. Use
       incremental hashing so we don't care about parts[] capacity here. */
    {
        BCRYPT_ALG_HANDLE  hAlg = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;
        NTSTATUS status;
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0) != 0) goto cleanup;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0); goto cleanup;
        }
        for (cnt = 0; cnt < key_len; cnt++) {
            status = BCryptHashData(hHash, (PUCHAR)key, (ULONG)key_len, 0);
            if (status != 0) {
                BCryptDestroyHash(hHash);
                BCryptCloseAlgorithmProvider(hAlg, 0);
                goto cleanup;
            }
        }
        status = BCryptFinishHash(hHash, p_seq, SHA512_DIGEST_LEN, 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (status != 0) goto cleanup;
    }

    /* Expand DP into P: floor(key_len / 64) full copies + (key_len % 64) bytes */
    P = (BYTE *)malloc(key_len > 0 ? key_len : 1);
    if (!P) return 0;
    {
        size_t left = key_len;
        BYTE *dst = P;
        while (left >= SHA512_DIGEST_LEN) {
            memcpy(dst, p_seq, SHA512_DIGEST_LEN);
            dst += SHA512_DIGEST_LEN;
            left -= SHA512_DIGEST_LEN;
        }
        if (left) memcpy(dst, p_seq, left);
    }

    /* --- Steps 16-18: DS = SHA512(salt repeated (16 + digest[0]) times).
       digest[0] is 0..255 so reps can be up to 271 — way past parts[]
       capacity, so incremental again. */
    {
        BCRYPT_ALG_HANDLE  hAlg = NULL;
        BCRYPT_HASH_HANDLE hHash = NULL;
        NTSTATUS status;
        int reps = 16 + digest[0];

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA512_ALGORITHM, NULL, 0) != 0) goto cleanup;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) {
            BCryptCloseAlgorithmProvider(hAlg, 0);
            goto cleanup;
        }
        for (i = 0; i < reps; i++) {
            status = BCryptHashData(hHash, (PUCHAR)salt, (ULONG)salt_len, 0);
            if (status != 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); goto cleanup; }
        }
        status = BCryptFinishHash(hHash, s_seq, SHA512_DIGEST_LEN, 0);
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        if (status != 0) goto cleanup;
    }

    /* Expand DS into S */
    S = (BYTE *)malloc(salt_len > 0 ? salt_len : 1);
    if (!S) goto cleanup;
    {
        size_t left = salt_len;
        BYTE *dst = S;
        while (left >= SHA512_DIGEST_LEN) {
            memcpy(dst, s_seq, SHA512_DIGEST_LEN);
            dst += SHA512_DIGEST_LEN;
            left -= SHA512_DIGEST_LEN;
        }
        if (left) memcpy(dst, s_seq, left);
    }

    /* --- Step 21: main loop, 5000 rounds. Each round hashes a sequence of
       (P, S, prev_digest) chosen by the round index parity vs 2/3/7. */
    for (i = 0; i < rounds; i++) {
        n = 0;
        if (i & 1) { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        else       { parts[n].data = digest; parts[n].len = SHA512_DIGEST_LEN; n++; }
        if (i % 3) { parts[n].data = S; parts[n].len = (ULONG)salt_len; n++; }
        if (i % 7) { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        if (i & 1) { parts[n].data = digest; parts[n].len = SHA512_DIGEST_LEN; n++; }
        else       { parts[n].data = P; parts[n].len = (ULONG)key_len; n++; }
        if (!sha512_oneshot(parts, n, digest)) goto cleanup;
    }

    /* --- Step 22: encode digest into crypt_b64 form, in a specific 21-group
       permutation defined by the spec. Final char encodes the leftover byte. */
    cp = out;
    cp += sprintf_s(cp, out_size, "$6$");
    if (salt_len > 0) {
        memcpy(cp, salt, salt_len);
        cp += salt_len;
    }
    *cp++ = '$';

    crypt_b64_3(digest[ 0], digest[21], digest[42], 4, &cp);
    crypt_b64_3(digest[22], digest[43], digest[ 1], 4, &cp);
    crypt_b64_3(digest[44], digest[ 2], digest[23], 4, &cp);
    crypt_b64_3(digest[ 3], digest[24], digest[45], 4, &cp);
    crypt_b64_3(digest[25], digest[46], digest[ 4], 4, &cp);
    crypt_b64_3(digest[47], digest[ 5], digest[26], 4, &cp);
    crypt_b64_3(digest[ 6], digest[27], digest[48], 4, &cp);
    crypt_b64_3(digest[28], digest[49], digest[ 7], 4, &cp);
    crypt_b64_3(digest[50], digest[ 8], digest[29], 4, &cp);
    crypt_b64_3(digest[ 9], digest[30], digest[51], 4, &cp);
    crypt_b64_3(digest[31], digest[52], digest[10], 4, &cp);
    crypt_b64_3(digest[53], digest[11], digest[32], 4, &cp);
    crypt_b64_3(digest[12], digest[33], digest[54], 4, &cp);
    crypt_b64_3(digest[34], digest[55], digest[13], 4, &cp);
    crypt_b64_3(digest[56], digest[14], digest[35], 4, &cp);
    crypt_b64_3(digest[15], digest[36], digest[57], 4, &cp);
    crypt_b64_3(digest[37], digest[58], digest[16], 4, &cp);
    crypt_b64_3(digest[59], digest[17], digest[38], 4, &cp);
    crypt_b64_3(digest[18], digest[39], digest[60], 4, &cp);
    crypt_b64_3(digest[40], digest[61], digest[19], 4, &cp);
    crypt_b64_3(digest[62], digest[20], digest[41], 4, &cp);
    crypt_b64_3(       0,         0, digest[63], 2, &cp);

    *cp = '\0';
    rc = (int)(cp - out);

cleanup:
    if (P) { SecureZeroMemory(P, key_len); free(P); }
    if (S) { SecureZeroMemory(S, salt_len); free(S); }
    SecureZeroMemory(digest, sizeof(digest));
    SecureZeroMemory(alt, sizeof(alt));
    SecureZeroMemory(p_seq, sizeof(p_seq));
    SecureZeroMemory(s_seq, sizeof(s_seq));
    return rc;
}

/* Public-ish wrapper: hash a wide-char password into glibc $6$ format.
   Generates a fresh random 16-char salt. Wipes the UTF-8 conversion of the
   password from memory after use. Returns TRUE on success. */
BOOL unix_password_hash(const wchar_t *plain, char *hash_out, size_t hash_out_size)
{
    char utf8[512];
    int  utf8_len;
    char salt[17];
    BYTE salt_raw[12];
    int  i, rc;

    if (!plain || !hash_out || hash_out_size < 110) return FALSE;

    utf8_len = WideCharToMultiByte(CP_UTF8, 0, plain, -1, utf8, sizeof(utf8) - 1, NULL, NULL);
    if (utf8_len <= 0) return FALSE;
    /* WideCharToMultiByte includes the NUL in the count when input is
       NUL-terminated. Trim it for the hash input. */
    if (utf8[utf8_len - 1] == '\0') utf8_len--;

    /* 16-char crypt_b64 salt from 12 random bytes (96 bits of entropy). */
    if (BCryptGenRandom(NULL, salt_raw, sizeof(salt_raw),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        SecureZeroMemory(utf8, sizeof(utf8));
        return FALSE;
    }
    for (i = 0; i < 16; i++) {
        /* Pack 3 random bytes -> 4 b64 chars, take 16 of them. */
        unsigned b = salt_raw[i % sizeof(salt_raw)];
        salt[i] = crypt_b64[b & 0x3f];
    }
    salt[16] = '\0';

    rc = sha512_crypt(utf8, (size_t)utf8_len, salt, 16, hash_out, hash_out_size);
    SecureZeroMemory(utf8, sizeof(utf8));
    SecureZeroMemory(salt_raw, sizeof(salt_raw));
    return rc > 0;
}


/* Lay out everything that setup.sh expects under staging\extras\.
 *
 * Path resolution per artifact kind:
 *   - Build outputs (agent ELFs, .ko modules, wsl-mesa tarball)
 *       1. <res_dir>\linux\... — primary path. <res_dir> is bin\<cfg>\resources\
 *          at runtime, populated by AppSandbox.vcxproj's PostBuildEvent which
 *          xcopies the committed release\resources\ tree. So files committed
 *          under release\resources\linux\ flow here automatically.
 *       2. <repo>\tools\linux\dist\... — dev fallback. Set by `make` on the
 *          Linux dev box; useful when running AppSandbox.exe straight out of
 *          the build tree before doing the release/resources/linux/ copy.
 *
 *   - Source-only artifacts (systemd units, DKMS source trees, modprobe.d
 *     configs): read straight from <repo>\tools\linux\<sub>\... — these
 *     are stable text files in the repo, never built, no point routing them
 *     through dist/.
 *
 *   - wsl-deps .so libs: prefetched into C:\ProgramData\AppSandbox\wsl-deps\
 *     by `iso-patch.exe --prefetch-wsl-deps` (called automatically from
 *     linux_create_thread when the cache is empty). C-only — no PowerShell.
 */
void stage_linux_agent_and_extras(const wchar_t *staging,
                                  const wchar_t *res_dir,
                                  BOOL ssh_enabled)
{
    wchar_t extras[MAX_PATH];

    (void)res_dir;       /* unused — no host-side resource lookups any more */
    (void)ssh_enabled;   /* SSH packaging handled separately in firstboot */

    /* Direct-stage model: linux_create_thread runs three iso-patch
     * prefetch modes that already wrote into <staging>\extras\ — agent
     * source from GitHub, build-dep .debs from archive.ubuntu.com, and
     * wsl-deps .so files from the Microsoft NuGet feed. This function
     * just sanity-checks that the directory exists and logs what's
     * present; nothing more to stage on the host side. */

    swprintf_s(extras, MAX_PATH, L"%s\\extras", staging);
    if (GetFileAttributesW(extras) == INVALID_FILE_ATTRIBUTES) {
        ui_log(L"WARN: %s\\extras not present — prefetches all failed?", staging);
        return;
    }
    ui_log(L"Staging Linux extras: prefetches populated %s\\extras", staging);
}
