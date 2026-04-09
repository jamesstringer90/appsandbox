#include "disk_util.h"
#include "ui.h"
#include <virtdisk.h>
#include <stdio.h>
#include <wincrypt.h>
#include <imapi2fs.h>

#pragma comment(lib, "virtdisk.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

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
    wchar_t comp_name[16];
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
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <SetupUILanguage><UILanguage>%s</UILanguage></SetupUILanguage>\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
        L"            </Disk></DiskConfiguration>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <RunSynchronous>\n"
        L"                <RunSynchronousCommand wcm:action=\"add\">\n"
        L"                    <Order>1</Order>\n"
        L"                    <Path>reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f</Path>\n"
        L"                </RunSynchronousCommand>\n",
        lang, lang_to_input_locale(lang), lang, lang, lang,
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
            L"                   processorArchitecture=\"amd64\"\n"
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
            L"                   processorArchitecture=\"amd64\"\n"
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
            L"                   processorArchitecture=\"amd64\"\n"
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
            L"                   processorArchitecture=\"amd64\"\n"
            L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
            L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
            L"            <InputLocale>%s</InputLocale>\n"
            L"            <SystemLocale>%s</SystemLocale>\n"
            L"            <UILanguage>%s</UILanguage>\n"
            L"            <UserLocale>%s</UserLocale>\n"
            L"        </component>\n"
            L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
            L"                   processorArchitecture=\"amd64\"\n"
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
            lang_to_input_locale(lang), lang, lang, lang,
            admin_user, b64_password,
            admin_user, b64_password);
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

    for (;;) {
        hr = stream->lpVtbl->Read(stream, buf, sizeof(buf), &bytes_read);
        if (FAILED(hr) || bytes_read == 0) break;
        WriteFile(hFile, buf, bytes_read, &bytes_written, NULL);
    }

    CloseHandle(hFile);
    return S_OK;
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

static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir);

HRESULT iso_create_resources(const wchar_t *iso_path,
                              const wchar_t *vm_name,
                              const wchar_t *admin_user,
                              wchar_t *admin_pass,
                              const wchar_t *res_dir,
                              BOOL is_template,
                              BOOL test_mode,
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

    /* DISABLED: p9client.exe packaging — GPU driver copy is now handled by the
       agent service via embedded 9P client (p9copy). To revert, uncomment this
       block and the gpudrv-copy.cmd block below, and restore setup.cmd lines. */
#if 0 /* >>> REVERT: uncomment to restore standalone p9client.exe on ISO <<< */
    /* p9client.exe for GPU driver copy via Plan9 shares.
       Check res_dir first, then fall back to parent directory (exe_dir). */
    {
        BOOL found_p9 = FALSE;
        if (res_dir) {
            swprintf_s(src_path, MAX_PATH, L"%s\\p9client.exe", res_dir);
            if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
                found_p9 = TRUE;
        }
        if (!found_p9) {
            /* Try parent of res_dir (the exe directory itself) */
            wcscpy_s(src_path, MAX_PATH, dir);
            /* dir was derived from iso_path; try exe directory instead */
            {
                wchar_t exe_dir[MAX_PATH];
                wchar_t *slash;
                GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
                slash = wcsrchr(exe_dir, L'\\');
                if (slash) *slash = L'\0';
                swprintf_s(src_path, MAX_PATH, L"%s\\p9client.exe", exe_dir);
                if (GetFileAttributesW(src_path) != INVALID_FILE_ATTRIBUTES)
                    found_p9 = TRUE;
            }
        }
        if (found_p9) {
            swprintf_s(file_path, MAX_PATH, L"%s\\p9client.exe", staging);
            if (!CopyFileW(src_path, file_path, FALSE))
                ui_log(L"Warning: failed to copy p9client.exe (error %lu)", GetLastError());
        } else {
            ui_log(L"Warning: p9client.exe not found (needed for GPU driver copy)");
        }
    }
#endif /* >>> END REVERT p9client.exe <<< */

    /* Agent + input helper + VDD driver files */
    stage_agent_and_setup(staging, res_dir);

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
       Installs VDD driver (cert + test signing + devcon). */
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
                ":done\r\n"
                "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
                sc);
            fclose(sc);
        }
    }

    /* DISABLED: gpudrv-copy.cmd — GPU driver copy is now handled by the agent
       service via embedded 9P client. To revert, uncomment this block and
       restore setup.cmd + p9client.exe packaging above. */
#if 0 /* >>> REVERT: uncomment to restore gpudrv-copy.cmd on ISO <<< */
    /* gpudrv-copy.cmd — boot-time script: reads manifest and copies driver shares.
       In a .cmd file, for variables use %%a. In C fputs, no escaping needed. */
    swprintf_s(file_path, MAX_PATH, L"%s\\gpudrv-copy.cmd", staging);
    {
        FILE *cmd;
        if (_wfopen_s(&cmd, file_path, L"w") == 0 && cmd) {
            fputs(
                "@echo off\r\n"
                "set P9=%SystemRoot%\\AppSandbox\\p9client.exe\r\n"
                "set CFG=%SystemRoot%\\AppSandbox\\config\r\n"
                "\r\n"
                "echo [AppSandbox] Fetching GPU driver manifest...\r\n"
                "\"%P9%\" AppSandbox.Manifest \"%CFG%\"\r\n"
                "if not exist \"%CFG%\\shares.txt\" (\r\n"
                "    echo [AppSandbox] No manifest found, skipping GPU driver copy.\r\n"
                "    exit /b 0\r\n"
                ")\r\n"
                "\r\n"
                "echo [AppSandbox] Copying GPU driver files...\r\n"
                "for /f \"usebackq tokens=1,2,*\" %%a in (\"%CFG%\\shares.txt\") do (\r\n"
                "    if \"%%c\"==\"\" (\r\n"
                "        echo [AppSandbox] Share: %%a -^> %%b\r\n"
                "        \"%P9%\" %%a \"%%b\"\r\n"
                "    ) else (\r\n"
                "        echo [AppSandbox] Share: %%a -^> %%b [filtered]\r\n"
                "        \"%P9%\" %%a \"%%b\" --filter %%c\r\n"
                "    )\r\n"
                ")\r\n"
                "echo [AppSandbox] GPU driver copy complete.\r\n"
                "\r\n"
                "echo [AppSandbox] Restarting GPU devices to load drivers...\r\n"
                "setlocal enabledelayedexpansion\r\n"
                "set \"DEVID=\"\r\n"
                "for /f \"tokens=1,* delims=:\" %%a in ('pnputil /enum-devices /class Display') do (\r\n"
                "    echo %%a | findstr /i \"Instance\" >nul && (\r\n"
                "        set \"DEVID=%%b\"\r\n"
                "        set \"DEVID=!DEVID:~1!\"\r\n"
                "    )\r\n"
                "    echo %%a | findstr /i \"Driver Name\" >nul && (\r\n"
                "        set \"DRVNAME=%%b\"\r\n"
                "        echo !DRVNAME! | findstr /i \"vrd.inf\" >nul && (\r\n"
                "            echo [AppSandbox] Restarting GPU-PV device !DEVID!\r\n"
                "            pnputil /disable-device \"!DEVID!\"\r\n"
                "            pnputil /enable-device \"!DEVID!\"\r\n"
                "        )\r\n"
                "    )\r\n"
                ")\r\n"
                "endlocal\r\n"
                "echo [AppSandbox] GPU device restart complete.\r\n",
                cmd);
            fclose(cmd);
        } else {
            ui_log(L"Warning: failed to write gpudrv-copy.cmd");
        }
    }
#endif /* >>> END REVERT gpudrv-copy.cmd <<< */

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
    wchar_t comp_name[16];
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
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
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
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n"
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
        lang_to_input_locale(lang), lang, lang, lang,
        admin_user, b64_password,
        admin_user, b64_password);

    fclose(f);
    return TRUE;
}

/* Helper: copy agent exe to staging and write setup.cmd */
static void stage_agent_and_setup(const wchar_t *staging, const wchar_t *res_dir)
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
                "    \"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
                ")\r\n"
                "echo === instance setup.cmd finished === >> \"%LOG%\"\r\n",
                cmd);
            fclose(cmd);
        }
    }

    /* SetupComplete.cmd — runs as SYSTEM after setup, before first logon.
       Copied to C:\Windows\Setup\Scripts\ during specialize pass.
       Installs VDD driver (cert + test signing + devcon). */
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
    if (!generate_unattend_instance(file_path, vm_name, admin_user, b64_pass, lang))
        ui_log(L"Warning: failed to write instance unattend.xml");

    /* Agent exe + setup.cmd */
    stage_agent_and_setup(staging, res_dir);

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
   Only specialize + oobeSystem for first-boot mini-setup. */
BOOL generate_unattend_vhdx(const wchar_t *output_path,
                             const wchar_t *vm_name,
                             const wchar_t *admin_user,
                             const wchar_t *admin_pass,
                             BOOL test_mode,
                             const wchar_t *lang)
{
    FILE *f;
    wchar_t comp_name[16];
    wchar_t b64_pass[512];

    if (!output_path || !vm_name || !admin_user || !admin_pass)
        return FALSE;

    wcsncpy_s(comp_name, 16, vm_name, 15);

    if (!encode_unattend_password(admin_pass, b64_pass, 512)) {
        return FALSE;
    }

    if (_wfopen_s(&f, output_path, L"w,ccs=UTF-8") != 0 || !f) {
        SecureZeroMemory(b64_pass, sizeof(b64_pass));
        return FALSE;
    }

    /* specialize pass: ComputerName + BypassNRO + optional testsigning */
    fwprintf(f,
        L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\n"
        L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
        L"\n"
        L"    <settings pass=\"specialize\">\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"\n");

    /* oobeSystem pass: locale, OOBE hiding, user account, FirstLogonCommand (on-disk path) */
    fwprintf(f,
        L"    <settings pass=\"oobeSystem\">\n"
        L"        <component name=\"Microsoft-Windows-International-Core\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <InputLocale>%s</InputLocale>\n"
        L"            <SystemLocale>%s</SystemLocale>\n"
        L"            <UILanguage>%s</UILanguage>\n"
        L"            <UserLocale>%s</UserLocale>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Shell-Setup\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
        L"                    <CommandLine>C:\\Windows\\AppSandbox\\setup.cmd</CommandLine>\n"
        L"                    <RequiresUserInput>false</RequiresUserInput>\n"
        L"                </SynchronousCommand>\n"
        L"            </FirstLogonCommands>\n"
        L"        </component>\n"
        L"    </settings>\n"
        L"</unattend>\n",
        lang_to_input_locale(lang), lang, lang, lang,
        admin_user, b64_pass,
        admin_user, b64_pass);

    fclose(f);
    SecureZeroMemory(b64_pass, sizeof(b64_pass));
    return TRUE;
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
        L"                   processorArchitecture=\"amd64\"\n"
        L"                   publicKeyToken=\"31bf3856ad364e35\"\n"
        L"                   language=\"neutral\" versionScope=\"nonSxS\">\n"
        L"            <ComputerName>%s</ComputerName>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-Deployment\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
    }

    fwprintf(f,
        L"            </RunSynchronous>\n"
        L"        </component>\n"
        L"        <component name=\"Microsoft-Windows-SecureStartup-FilterDriver\"\n"
        L"                   processorArchitecture=\"amd64\"\n"
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
        L"                   processorArchitecture=\"amd64\"\n"
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
        L"                   processorArchitecture=\"amd64\"\n"
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

/* Generate setup.cmd for VHDX-first boot (agent already on disk) */
BOOL generate_vhdx_setup_cmd(const wchar_t *output_path)
{
    FILE *cmd;
    if (_wfopen_s(&cmd, output_path, L"w") != 0 || !cmd)
        return FALSE;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "echo === setup.cmd started === >> \"%LOG%\"\r\n"
        "REM Agent already at C:\\Windows\\AppSandbox\\ from VHDX staging\r\n"
        "\"%SystemRoot%\\AppSandbox\\appsandbox-agent.exe\" --install >> \"%LOG%\" 2>&1\r\n"
        "echo === setup.cmd finished === >> \"%LOG%\"\r\n",
        cmd);
    fclose(cmd);
    return TRUE;
}

/* Generate SetupComplete.cmd for VHDX-first boot (VDD driver files on disk) */
BOOL generate_vhdx_setupcomplete(const wchar_t *output_path)
{
    FILE *sc;
    if (_wfopen_s(&sc, output_path, L"w") != 0 || !sc)
        return FALSE;
    fputs(
        "@echo off\r\n"
        "set LOG=%SystemRoot%\\AppSandbox\\setup.log\r\n"
        "mkdir \"%SystemRoot%\\AppSandbox\" 2>nul\r\n"
        "echo === SetupComplete.cmd started === >> \"%LOG%\"\r\n"
        "\r\n"
        "set DRVDIR=%SystemRoot%\\AppSandbox\\drivers\r\n"
        "if not exist \"%DRVDIR%\\AppSandboxVDD.inf\" goto :done\r\n"
        "\r\n"
        "echo [VDD] Enabling test signing... >> \"%LOG%\"\r\n"
        "bcdedit /set testsigning on >> \"%LOG%\" 2>&1\r\n"
        "\r\n"
        "if exist \"%DRVDIR%\\AppSandboxVDD.cer\" (\r\n"
        "    echo [VDD] Installing certificate... >> \"%LOG%\"\r\n"
        "    certutil -addstore Root \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        "    certutil -f -addstore TrustedPublisher \"%DRVDIR%\\AppSandboxVDD.cer\" >> \"%LOG%\" 2>&1\r\n"
        ")\r\n"
        "\r\n"
        "echo [VDD] Installing driver with devcon... >> \"%LOG%\"\r\n"
        "\"%DRVDIR%\\devcon.exe\" install \"%DRVDIR%\\AppSandboxVDD.inf\" Root\\AppSandboxVDD >> \"%LOG%\" 2>&1\r\n"
        "echo [VDD] devcon exit code: %errorlevel% >> \"%LOG%\"\r\n"
        "\r\n"
        "REM Disable display sleep so IDD swap chain stays alive\r\n"
        "powercfg /change monitor-timeout-ac 0\r\n"
        "powercfg /change monitor-timeout-dc 0\r\n"
        "powercfg /change standby-timeout-ac 0\r\n"
        "powercfg /change standby-timeout-dc 0\r\n"
        "echo [PWR] Display sleep disabled >> \"%LOG%\"\r\n"
        "\r\n"
        ":done\r\n"
        "echo === SetupComplete.cmd finished === >> \"%LOG%\"\r\n",
        sc);
    fclose(sc);
    return TRUE;
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
                            const void *gpu_shares_ptr)
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
        const wchar_t *bins[] = { L"appsandbox-agent.exe", L"appsandbox-input.exe", L"appsandbox-displays.exe", L"appsandbox-clipboard.exe", L"appsandbox-clipboard-reader.exe" };
        int bi;
        for (bi = 0; bi < 5; bi++) {
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
