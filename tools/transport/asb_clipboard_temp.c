#include "asb_clipboard_temp.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#define ASB_CLIP_TEMP_MAGIC 0x41544350u
#define ASB_CLIP_TEMP_VERSION 2u
#define ASB_CLIP_TEMP_RETENTION 3600u

typedef struct AsbClipboardTempRecord {
    uint32_t magic;
    uint32_t version;
    uint64_t deadline;
    uint64_t deadline_check;
    uint64_t device;
    uint64_t inode;
    char session[32];
} AsbClipboardTempRecord;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>
#include <sddl.h>
#include <aclapi.h>
#include <wtsapi32.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#define ASB_CLIP_TEMP_PATH MAX_PATH
#define TEMP_PATH_LENGTH wcslen
#define TEMP_PATH_DUP _wcsdup
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#define ASB_CLIP_TEMP_PATH PATH_MAX
#define TEMP_PATH_LENGTH strlen
#define TEMP_PATH_DUP strdup
#endif

typedef struct AsbClipboardSession AsbClipboardSession;

struct AsbClipboardTemp {
    AsbClipboardPathChar *root;
    AsbClipboardPathChar *path;
    struct AsbClipboardTemp *next;
    const void *owner;
    uint64_t token;
    AsbClipboardTempRecord record;
    AsbClipboardSession *session;
#ifdef _WIN32
    HANDLE root_handle;
    HANDLE lease;
#else
    int root_fd;
    int data_fd;
    int lease;
#endif
};

static AsbClipboardTemp *published;
static AsbClipboardTemp *retired;
static uint64_t clipboard_token;

struct AsbClipboardSession {
    AsbClipboardPathChar *parent;
    AsbClipboardTemp *keeper;
    char name[32];
    struct AsbClipboardSession *next;
};

static AsbClipboardSession *sessions;
static int stopping;
static void wake_collector(void);
static void queue_retired(AsbClipboardTemp *temp, uint64_t deadline);

static int session_name_safe(const char *name)
{
    size_t length = strnlen(name, 32);
    if (!length || length >= 32) return 0;
    for (size_t i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    return strcmp(name, ".") && strcmp(name, "..");
}

#ifdef _WIN32
static SRWLOCK temp_lock = SRWLOCK_INIT;
#define TEMP_LOCK() AcquireSRWLockExclusive(&temp_lock)
#define TEMP_UNLOCK() ReleaseSRWLockExclusive(&temp_lock)

static PSECURITY_DESCRIPTOR temp_security(int console, int *handoff)
{
    HANDLE token = NULL;
    TOKEN_USER *user = NULL;
    wchar_t *sid = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD size = 0;
    wchar_t sddl[512];
    if (handoff) *handoff = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    user = malloc(size);
    if (!user || !GetTokenInformation(token, TokenUser, user, size, &size)) goto done;
    if (console && IsWellKnownSid(user->User.Sid, WinLocalSystemSid)) {
        if (handoff) *handoff = 1;
        CloseHandle(token);
        token = NULL;
        free(user);
        user = NULL;
        if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) goto done;
        size = 0;
        GetTokenInformation(token, TokenUser, NULL, 0, &size);
        user = malloc(size);
        if (!user || !GetTokenInformation(token, TokenUser, user, size, &size)) goto done;
    }
    if (!ConvertSidToStringSidW(user->User.Sid, &sid)) goto done;
    if (swprintf_s(sddl, ARRAYSIZE(sddl), L"O:%sD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;%s)", sid, sid) < 0) goto done;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, NULL);
done:
    if (sid) LocalFree(sid);
    free(user);
    if (token) CloseHandle(token);
    return descriptor;
}

static int record_write(HANDLE lease, const AsbClipboardTempRecord *record, int flush)
{
    AsbClipboardTempRecord committed = *record;
    committed.deadline_check = ~committed.deadline;
    LARGE_INTEGER zero = {0};
    DWORD count = 0;
    return SetFilePointerEx(lease, zero, NULL, FILE_BEGIN) &&
           WriteFile(lease, &committed, sizeof(committed), &count, NULL) && count == sizeof(committed) &&
           (!flush || FlushFileBuffers(lease));
}

static HANDLE open_child(HANDLE parent, const wchar_t *name, ACCESS_MASK access)
{
    typedef NTSTATUS (NTAPI *CreateFileFn)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
        PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    typedef ULONG (NTAPI *StatusErrorFn)(NTSTATUS);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    CreateFileFn create_file = (CreateFileFn)GetProcAddress(module, "NtCreateFile");
    StatusErrorFn status_error = (StatusErrorFn)GetProcAddress(module, "RtlNtStatusToDosError");
    if (!create_file || !status_error || wcslen(name) >= MAX_PATH) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return INVALID_HANDLE_VALUE;
    }
    UNICODE_STRING path = {(USHORT)(wcslen(name) * sizeof(wchar_t)), (USHORT)(wcslen(name) * sizeof(wchar_t)), (PWSTR)name};
    OBJECT_ATTRIBUTES attributes = {sizeof(attributes), parent, &path, 0x40, NULL, NULL};
    IO_STATUS_BLOCK status;
    HANDLE result = INVALID_HANDLE_VALUE;
    NTSTATUS error = create_file(&result, access | SYNCHRONIZE, &attributes, &status,
        NULL, 0, FILE_SHARE_READ, 1, 0x00200020, NULL, 0);
    if (error < 0) { SetLastError(status_error(error)); return INVALID_HANDLE_VALUE; }
    return result;
}

static int delete_handle(HANDLE handle)
{
    FILE_DISPOSITION_INFO disposition = {TRUE};
    return SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

static int remove_tree(HANDLE entry)
{
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(entry, &info)) return 0;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        BYTE *buffer = malloc(32768);
        if (!buffer) return 0;
        FILE_INFO_BY_HANDLE_CLASS type = FileIdBothDirectoryRestartInfo;
        int ok = 1;
        while (GetFileInformationByHandleEx(entry, type, buffer, 32768)) {
            type = FileIdBothDirectoryInfo;
            FILE_ID_BOTH_DIR_INFO *item = (FILE_ID_BOTH_DIR_INFO *)buffer;
            for (;;) {
                wchar_t name[MAX_PATH];
                size_t count = item->FileNameLength / sizeof(wchar_t);
                if (count >= ARRAYSIZE(name) || (item->FileNameLength & 1)) { ok = 0; break; }
                memcpy(name, item->FileName, item->FileNameLength);
                name[count] = 0;
                if (wcscmp(name, L".") && wcscmp(name, L"..")) {
                    HANDLE child = open_child(entry, name, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY);
                    if (child == INVALID_HANDLE_VALUE) ok = 0;
                    else {
                        if (!remove_tree(child)) ok = 0;
                        CloseHandle(child);
                    }
                }
                if (!item->NextEntryOffset) break;
                item = (FILE_ID_BOTH_DIR_INFO *)((BYTE *)item + item->NextEntryOffset);
            }
            if (!ok) break;
        }
        if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = 0;
        free(buffer);
        if (!ok) return 0;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) && info.nNumberOfLinks == 1) {
        FILE_BASIC_INFO basic;
        if (!GetFileInformationByHandleEx(entry, FileBasicInfo, &basic, sizeof(basic))) return 0;
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!basic.FileAttributes) basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(entry, FileBasicInfo, &basic, sizeof(basic))) return 0;
    }
    return delete_handle(entry);
}

static int remove_data(HANDLE root)
{
    HANDLE data = open_child(root, L"d", DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | FILE_LIST_DIRECTORY);
    if (data == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    int removed = remove_tree(data);
    CloseHandle(data);
    return removed;
}

static void release_handles(AsbClipboardTemp *temp, int keep_lease)
{
    if (temp->root_handle != INVALID_HANDLE_VALUE) CloseHandle(temp->root_handle);
    temp->root_handle = INVALID_HANDLE_VALUE;
    if (!keep_lease && temp->lease != INVALID_HANDLE_VALUE) CloseHandle(temp->lease);
    if (!keep_lease) temp->lease = INVALID_HANDLE_VALUE;
}

static void temp_close(AsbClipboardTemp *temp)
{
    release_handles(temp, 0);
    free(temp->root);
    free(temp->path);
    free(temp);
}

static AsbClipboardTemp *temp_create(const wchar_t *parent, const char *session)
{
    if (!parent || !*parent) return NULL;
    AsbClipboardTemp *temp = calloc(1, sizeof(*temp));
    PSECURITY_DESCRIPTOR descriptor = temp_security(0, NULL);
    if (!temp || !descriptor) { free(temp); if (descriptor) LocalFree(descriptor); return NULL; }
    temp->root_handle = temp->lease = INVALID_HANDLE_VALUE;
    temp->root = calloc(ASB_CLIP_TEMP_PATH, sizeof(*temp->root));
    temp->path = calloc(ASB_CLIP_TEMP_PATH, sizeof(*temp->path));
    if (!temp->root || !temp->path) { LocalFree(descriptor); temp_close(temp); return NULL; }
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    int created = 0;
    for (int attempt = 0; attempt < 64; attempt++) {
        if (!GetTempFileNameW(parent, L"asc", 0, temp->root)) break;
        if (!DeleteFileW(temp->root)) break;
        if (CreateDirectoryW(temp->root, &security)) { created = 1; break; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (!created) { LocalFree(descriptor); temp_close(temp); return NULL; }
    temp->root_handle = CreateFileW(temp->root, DELETE | READ_CONTROL | WRITE_DAC | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION root_info = {0};
    PSECURITY_DESCRIPTOR owner_descriptor = NULL;
    PSID owner = NULL, expected_owner = NULL;
    BOOL defaulted = FALSE;
    int owned = temp->root_handle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(temp->root_handle, &root_info) &&
        (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        GetSecurityDescriptorOwner(descriptor, &expected_owner, &defaulted) &&
        GetSecurityInfo(temp->root_handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            &owner, NULL, NULL, NULL, &owner_descriptor) == ERROR_SUCCESS && EqualSid(owner, expected_owner);
    if (owner_descriptor) LocalFree(owner_descriptor);
    LocalFree(descriptor);
    if (!owned) { temp_close(temp); return NULL; }
    wchar_t lease_path[MAX_PATH];
    if (temp->root_handle == INVALID_HANDLE_VALUE || wcslen(temp->root) + 3 > MAX_PATH) goto fail;
    swprintf_s(lease_path, ARRAYSIZE(lease_path), L"%s\\s", temp->root);
    temp->lease = CreateFileW(lease_path, DELETE | GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, NULL);
    AsbClipboardTempRecord record = {0};
    record.magic = ASB_CLIP_TEMP_MAGIC;
    record.version = ASB_CLIP_TEMP_VERSION;
    record.device = root_info.dwVolumeSerialNumber;
    record.inode = ((uint64_t)root_info.nFileIndexHigh << 32) | root_info.nFileIndexLow;
    if (session) strncpy_s(record.session, sizeof(record.session), session, _TRUNCATE);
    temp->record = record;
    if (temp->lease == INVALID_HANDLE_VALUE || !record_write(temp->lease, &record, 1)) goto fail;
    swprintf_s(temp->path, ASB_CLIP_TEMP_PATH, L"%s\\d", temp->root);
    if (!CreateDirectoryW(temp->path, NULL)) goto fail;
    return temp;
fail:
    if (temp->lease == INVALID_HANDLE_VALUE) {
        delete_handle(temp->root_handle);
        temp_close(temp);
        return NULL;
    }
    asb_clipboard_temp_discard(temp);
    return NULL;
}

int asb_clipboard_temp_prepare(AsbClipboardTemp *temp)
{
    int handoff = 0;
    PSECURITY_DESCRIPTOR descriptor = temp_security(1, &handoff);
    if (descriptor && !handoff) { LocalFree(descriptor); return 1; }
    PACL acl = NULL;
    BOOL present = FALSE, defaulted = FALSE;
    if (descriptor && temp->session) {
        PSID owner = NULL;
        wchar_t *sid = NULL;
        wchar_t sddl[512];
        PSECURITY_DESCRIPTOR keeper_descriptor = NULL;
        PACL keeper_acl = NULL;
        AsbClipboardTemp *keeper = temp->session->keeper;
        HANDLE root = CreateFileW(keeper->root, WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION info;
        int ok = root != INVALID_HANDLE_VALUE && GetFileInformationByHandle(root, &info) &&
            info.dwVolumeSerialNumber == keeper->record.device &&
            (((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow) == keeper->record.inode &&
            GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) && ConvertSidToStringSidW(owner, &sid) &&
            swprintf_s(sddl, ARRAYSIZE(sddl), L"D:P(A;OICI;FA;;;SY)(A;OICI;GRGX;;;%s)", sid) > 0 &&
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &keeper_descriptor, NULL) &&
            GetSecurityDescriptorDacl(keeper_descriptor, &present, &keeper_acl, &defaulted) && present &&
            SetSecurityInfo(root, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                NULL, NULL, keeper_acl, NULL) == ERROR_SUCCESS;
        if (sid) LocalFree(sid);
        if (keeper_descriptor) LocalFree(keeper_descriptor);
        if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
        if (!ok) { LocalFree(descriptor); return 0; }
    }
    int ok = descriptor && GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) && present &&
        SetSecurityInfo(temp->root_handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        NULL, NULL, acl, NULL) == ERROR_SUCCESS;
    if (descriptor) LocalFree(descriptor);
    return ok;
}

void asb_clipboard_temp_discard(AsbClipboardTemp *temp)
{
    if (!temp) return;
    release_handles(temp, 0);
    if (temp->record.magic == ASB_CLIP_TEMP_MAGIC) queue_retired(temp, (uint64_t)time(NULL));
    else temp_close(temp);
}

static int write_retired(AsbClipboardTemp *temp)
{
    HANDLE root = CreateFileW(temp->root, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (root == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    BY_HANDLE_FILE_INFORMATION root_info, info;
    int owned = GetFileInformationByHandle(root, &root_info) &&
        (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        root_info.dwVolumeSerialNumber == temp->record.device &&
        (((uint64_t)root_info.nFileIndexHigh << 32) | root_info.nFileIndexLow) == temp->record.inode;
    int done = !owned;
    if (owned) {
        HANDLE lease = open_child(root, L"s", GENERIC_READ | GENERIC_WRITE);
        if (lease != INVALID_HANDLE_VALUE) {
            int valid = GetFileInformationByHandle(lease, &info) && info.nNumberOfLinks == 1 &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
            done = !valid || record_write(lease, &temp->record, 0);
            CloseHandle(lease);
        } else done = GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    CloseHandle(root);
    return done;
}

static int session_active(const wchar_t *parent, const char *name)
{
    if (!session_name_safe(name) || wcslen(parent) + strlen(name) + 2 > MAX_PATH) return 0;
    wchar_t path[MAX_PATH], wide[32];
    for (size_t i = 0; i <= strlen(name); ++i) wide[i] = (wchar_t)name[i];
    swprintf_s(path, ARRAYSIZE(path), L"%s\\%s", parent, wide);
    HANDLE root = CreateFileW(path, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (root == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_SHARING_VIOLATION;
    BY_HANDLE_FILE_INFORMATION info;
    int active = 0;
    if (GetFileInformationByHandle(root, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        HANDLE lease = open_child(root, L"s", GENERIC_READ);
        if (lease == INVALID_HANDLE_VALUE) active = GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_ACCESS_DENIED;
        else CloseHandle(lease);
    }
    CloseHandle(root);
    return active;
}

static void collect_parent(const wchar_t *parent)
{
    wchar_t pattern[MAX_PATH], root[MAX_PATH];
    WIN32_FIND_DATAW item;
    if (!parent || !*parent || wcslen(parent) + 13 >= MAX_PATH ||
        swprintf_s(pattern, ARRAYSIZE(pattern), L"%s\\asc????.tmp", parent) < 0) return;
    HANDLE find = FindFirstFileW(pattern, &item);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (item.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        swprintf_s(root, ARRAYSIZE(root), L"%s\\%s", parent, item.cFileName);
        HANDLE root_handle = CreateFileW(root, DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        BY_HANDLE_FILE_INFORMATION root_info;
        if (root_handle == INVALID_HANDLE_VALUE) continue;
        if (!GetFileInformationByHandle(root_handle, &root_info) ||
            !(root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(root_handle);
            continue;
        }
        HANDLE lease = open_child(root_handle, L"s", DELETE | GENERIC_READ | GENERIC_WRITE);
        if (lease == INVALID_HANDLE_VALUE) { CloseHandle(root_handle); continue; }
        BY_HANDLE_FILE_INFORMATION info;
        AsbClipboardTempRecord record = {0};
        DWORD count = 0;
        int valid = GetFileInformationByHandle(lease, &info) && info.nNumberOfLinks == 1 &&
                    !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    ReadFile(lease, &record, sizeof(record), &count, NULL) && count == sizeof(record) &&
                    record.magic == ASB_CLIP_TEMP_MAGIC && record.version == ASB_CLIP_TEMP_VERSION &&
                    record.device == root_info.dwVolumeSerialNumber &&
                    record.inode == (((uint64_t)root_info.nFileIndexHigh << 32) | root_info.nFileIndexLow);
        uint64_t now = (uint64_t)time(NULL);
        int remove = 0;
        int intact = record.deadline_check == ~record.deadline;
        if (valid && (!intact || !record.deadline) && !session_active(parent, record.session)) {
            record.deadline = now + ASB_CLIP_TEMP_RETENTION;
            record_write(lease, &record, 1);
        } else if (valid && intact && record.deadline && record.deadline <= now) {
            remove = remove_data(root_handle);
        }
        if (remove && !delete_handle(lease)) remove = 0;
        CloseHandle(lease);
        if (remove) delete_handle(root_handle);
        CloseHandle(root_handle);
    } while (FindNextFileW(find, &item));
    FindClose(find);
}

#else
static pthread_mutex_t temp_lock = PTHREAD_MUTEX_INITIALIZER;
#define TEMP_LOCK() pthread_mutex_lock(&temp_lock)
#define TEMP_UNLOCK() pthread_mutex_unlock(&temp_lock)

static int record_write(int lease, const AsbClipboardTempRecord *record, int flush)
{
    AsbClipboardTempRecord committed = *record;
    committed.deadline_check = ~committed.deadline;
    ssize_t count;
    do { count = pwrite(lease, &committed, sizeof(committed), 0); } while (count < 0 && errno == EINTR);
    if (count != sizeof(committed)) return 0;
    if (!flush) return 1;
    int rc;
    do { rc = fsync(lease); } while (rc < 0 && errno == EINTR);
    return rc == 0;
}

static int remove_contents(int fd)
{
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return 0;
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return 0; }
    struct dirent *item;
    int ok = 1;
    while ((item = readdir(dir)) != NULL) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) { if (errno != ENOENT) ok = 0; continue; }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) { ok = 0; continue; }
            int removed = remove_contents(child);
            close(child);
            if (!removed || unlinkat(fd, item->d_name, AT_REMOVEDIR) < 0) ok = 0;
        } else if (unlinkat(fd, item->d_name, 0) < 0 && errno != ENOENT) ok = 0;
    }
    closedir(dir);
    return ok;
}

static void release_handles(AsbClipboardTemp *temp, int keep_lease)
{
    if (temp->data_fd >= 0) close(temp->data_fd);
    if (temp->root_fd >= 0) close(temp->root_fd);
    temp->data_fd = temp->root_fd = -1;
    if (!keep_lease && temp->lease >= 0) close(temp->lease);
    if (!keep_lease) temp->lease = -1;
}

static void temp_close(AsbClipboardTemp *temp)
{
    release_handles(temp, 0);
    free(temp->root);
    free(temp->path);
    free(temp);
}

static AsbClipboardTemp *temp_create(const char *parent, const char *session)
{
    if (!parent || !*parent) return NULL;
    AsbClipboardTemp *temp = calloc(1, sizeof(*temp));
    if (!temp) return NULL;
    temp->lease = temp->root_fd = temp->data_fd = -1;
    temp->root = calloc(ASB_CLIP_TEMP_PATH, sizeof(*temp->root));
    temp->path = calloc(ASB_CLIP_TEMP_PATH, sizeof(*temp->path));
    if (!temp->root || !temp->path) { temp_close(temp); return NULL; }
    int length = snprintf(temp->root, ASB_CLIP_TEMP_PATH, "%s/.asbclip-XXXXXX", parent);
    if (length < 0 || (size_t)length >= ASB_CLIP_TEMP_PATH - 2 || !mkdtemp(temp->root)) { temp_close(temp); return NULL; }
    temp->root_fd = open(temp->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (temp->root_fd < 0) goto fail;
    struct stat root_info;
    if (fstat(temp->root_fd, &root_info) < 0 || root_info.st_uid != geteuid()) goto fail;
    temp->lease = openat(temp->root_fd, "s", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    AsbClipboardTempRecord record = {0};
    record.magic = ASB_CLIP_TEMP_MAGIC;
    record.version = ASB_CLIP_TEMP_VERSION;
    record.device = (uint64_t)root_info.st_dev;
    record.inode = (uint64_t)root_info.st_ino;
    if (session) snprintf(record.session, sizeof(record.session), "%s", session);
    temp->record = record;
    if (temp->lease < 0 || flock(temp->lease, LOCK_EX | LOCK_NB) < 0 || !record_write(temp->lease, &record, 1) ||
        mkdirat(temp->root_fd, "d", 0700) < 0) goto fail;
    temp->data_fd = openat(temp->root_fd, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (temp->data_fd < 0) goto fail;
    snprintf(temp->path, ASB_CLIP_TEMP_PATH, "%s/d", temp->root);
    return temp;
fail:
    if (temp->lease < 0) {
        rmdir(temp->root);
        temp_close(temp);
        return NULL;
    }
    asb_clipboard_temp_discard(temp);
    return NULL;
}

static int open_parent(AsbClipboardTemp *temp, const char *relative, char *leaf)
{
    char path[PATH_MAX];
    if (!relative || !*relative || *relative == '/' || strlen(relative) >= sizeof(path)) { errno = EINVAL; return -1; }
    strcpy(path, relative);
    int fd = openat(temp->data_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *part = path;
    for (;;) {
        char *slash = strchr(part, '/');
        if (slash) *slash = 0;
        if (!*part || !strcmp(part, ".") || !strcmp(part, "..")) { close(fd); errno = EINVAL; return -1; }
        if (!slash) { strcpy(leaf, part); return fd; }
        if (mkdirat(fd, part, 0700) < 0 && errno != EEXIST) { close(fd); return -1; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        if (next < 0) return -1;
        fd = next;
        part = slash + 1;
    }
}

int asb_clipboard_temp_directory(AsbClipboardTemp *temp, const char *relative)
{
    char leaf[PATH_MAX];
    int parent = open_parent(temp, relative, leaf);
    if (parent < 0) return 0;
    int ok = mkdirat(parent, leaf, 0700) == 0 || errno == EEXIST;
    int child = ok ? openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    close(parent);
    if (child < 0) return 0;
    close(child);
    return 1;
}

int asb_clipboard_temp_file(AsbClipboardTemp *temp, const char *relative)
{
    char leaf[PATH_MAX];
    int parent = open_parent(temp, relative, leaf);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(parent);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || ftruncate(fd, 0) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int asb_clipboard_temp_symlink(AsbClipboardTemp *temp, const char *relative, const char *target)
{
    char leaf[PATH_MAX];
    int parent = open_parent(temp, relative, leaf);
    if (parent < 0) return 0;
    int rc = symlinkat(target, parent, leaf);
    close(parent);
    return rc == 0;
}

static int change_owner(int fd, uid_t uid, gid_t gid)
{
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scan < 0) return 0;
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return 0; }
    struct dirent *item;
    int ok = 1;
    while ((item = readdir(dir)) != NULL) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, item->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) { ok = 0; break; }
        if (S_ISDIR(st.st_mode)) {
            int child = openat(fd, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0 || !change_owner(child, uid, gid)) ok = 0;
            if (child >= 0) close(child);
        } else if (fchownat(fd, item->d_name, uid, gid, AT_SYMLINK_NOFOLLOW) < 0) ok = 0;
        if (!ok) break;
    }
    closedir(dir);
    return ok && fchown(fd, uid, gid) == 0 && fchmod(fd, 0700) == 0;
}

int asb_clipboard_temp_set_owner(AsbClipboardTemp *temp, uid_t uid, gid_t gid)
{
    struct stat st;
    if (fstat(temp->root_fd, &st) < 0) return 0;
    if (st.st_uid == uid && st.st_gid == gid) return 1;
    if (temp->session) {
        AsbClipboardTemp *keeper = temp->session->keeper;
        int root = open(keeper->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int ok = root >= 0 && fstat(root, &st) == 0 &&
            (uint64_t)st.st_dev == keeper->record.device && (uint64_t)st.st_ino == keeper->record.inode &&
            fchmod(keeper->lease, 0644) == 0 && fchmod(root, 0755) == 0;
        if (root >= 0) close(root);
        if (!ok) return 0;
    }
    return change_owner(temp->root_fd, uid, gid);
}

void asb_clipboard_temp_discard(AsbClipboardTemp *temp)
{
    if (!temp) return;
    release_handles(temp, 0);
    if (temp->record.magic == ASB_CLIP_TEMP_MAGIC) queue_retired(temp, (uint64_t)time(NULL));
    else temp_close(temp);
}

static int write_retired(AsbClipboardTemp *temp)
{
    int root = open(temp->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return errno == ENOENT;
    struct stat st;
    int owned = fstat(root, &st) == 0 && (uint64_t)st.st_dev == temp->record.device && (uint64_t)st.st_ino == temp->record.inode;
    int done = !owned;
    if (owned) {
        int lease = openat(root, "s", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (lease >= 0) {
            if (flock(lease, LOCK_EX | LOCK_NB) == 0) {
                int valid = fstat(lease, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1;
                done = !valid || record_write(lease, &temp->record, 0);
            }
            close(lease);
        } else done = errno == ENOENT;
    }
    close(root);
    return done;
}

static int session_active(const char *parent, const char *name)
{
    if (!session_name_safe(name)) return 0;
    int base = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) return 1;
    int root = openat(base, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(base);
    if (root < 0) return errno == EACCES;
    int lease = openat(root, "s", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    close(root);
    if (lease < 0) return errno == EACCES;
    struct stat st;
    int active = fstat(lease, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
        flock(lease, LOCK_EX | LOCK_NB) < 0;
    close(lease);
    return active;
}

static void collect_parent(const char *parent)
{
    if (!parent || !*parent) return;
    DIR *dir = opendir(parent);
    if (!dir) return;
    int base = dirfd(dir);
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (strncmp(item->d_name, ".asbclip-", 9)) continue;
        int root = openat(base, item->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (root < 0) continue;
        struct stat st, root_info;
        if (fstat(root, &root_info) < 0 || (geteuid() != 0 && root_info.st_uid != geteuid())) { close(root); continue; }
        int lease = openat(root, "s", O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (lease < 0 || flock(lease, LOCK_EX | LOCK_NB) < 0) { if (lease >= 0) close(lease); close(root); continue; }
        AsbClipboardTempRecord record = {0};
        int valid = fstat(lease, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
                    pread(lease, &record, sizeof(record), 0) == sizeof(record) &&
                    record.magic == ASB_CLIP_TEMP_MAGIC && record.version == ASB_CLIP_TEMP_VERSION &&
                    record.device == (uint64_t)root_info.st_dev && record.inode == (uint64_t)root_info.st_ino;
        uint64_t now = (uint64_t)time(NULL);
        int remove = 0;
        int intact = record.deadline_check == ~record.deadline;
        if (valid && (!intact || !record.deadline) && !session_active(parent, record.session)) {
            record.deadline = now + ASB_CLIP_TEMP_RETENTION;
            record_write(lease, &record, 1);
        } else if (valid && intact && record.deadline && record.deadline <= now) {
            int data = openat(root, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (data >= 0) { remove = remove_contents(data); close(data); }
            else if (errno == ENOENT) remove = 1;
            if (remove && unlinkat(root, "d", AT_REMOVEDIR) < 0 && errno != ENOENT) remove = 0;
            if (remove && unlinkat(root, "s", 0) < 0) remove = 0;
        }
        close(lease);
        close(root);
        if (remove) unlinkat(base, item->d_name, AT_REMOVEDIR);
    }
    closedir(dir);
}
#endif

const AsbClipboardPathChar *asb_clipboard_temp_path(const AsbClipboardTemp *temp)
{
    return temp ? temp->path : NULL;
}

static void retire_locked(AsbClipboardTemp *temp, uint64_t deadline)
{
    temp->record.deadline = deadline;
    temp->next = retired;
    retired = temp;
}

static void queue_retired(AsbClipboardTemp *temp, uint64_t deadline)
{
    TEMP_LOCK();
    retire_locked(temp, deadline);
    TEMP_UNLOCK();
    wake_collector();
}

static void compact_temp(AsbClipboardTemp *temp)
{
    temp->session = NULL;
    free(temp->path);
    temp->path = NULL;
    void *root = realloc(temp->root, (TEMP_PATH_LENGTH(temp->root) + 1) * sizeof(*temp->root));
    if (root) temp->root = root;
}

static void collect_work(void)
{
    TEMP_LOCK();
    AsbClipboardTemp *batch = retired;
    AsbClipboardSession *parents = sessions;
    retired = NULL;
    TEMP_UNLOCK();
    AsbClipboardTemp *failed = NULL;
    while (batch) {
        AsbClipboardTemp *temp = batch;
        batch = temp->next;
        if (write_retired(temp)) temp_close(temp);
        else { temp->next = failed; failed = temp; }
    }
    TEMP_LOCK();
    while (failed) {
        AsbClipboardTemp *temp = failed;
        failed = temp->next;
        temp->next = retired;
        retired = temp;
    }
    TEMP_UNLOCK();
    for (AsbClipboardSession *session = parents; session; session = session->next)
        collect_parent(session->parent);
}

#ifdef _WIN32
static HANDLE collector_event;
static HANDLE collector_thread;

static DWORD WINAPI collector_main(LPVOID unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(collector_event, 60000);
        TEMP_LOCK();
        int done = stopping;
        TEMP_UNLOCK();
        if (done) break;
        collect_work();
    }
    return 0;
}

static int start_collector_locked(void)
{
    if (collector_thread) return 1;
    collector_event = CreateEventW(NULL, FALSE, TRUE, NULL);
    if (!collector_event) return 0;
    collector_thread = CreateThread(NULL, 0, collector_main, NULL, 0, NULL);
    if (collector_thread) return 1;
    CloseHandle(collector_event);
    collector_event = NULL;
    return 0;
}

static void wake_collector(void)
{
    TEMP_LOCK();
    if (collector_event) SetEvent(collector_event);
    TEMP_UNLOCK();
}
#else
static pthread_cond_t collector_cond = PTHREAD_COND_INITIALIZER;
static pthread_t collector_thread;
static int collector_started;
static int collector_requested = 1;

static void *collector_main(void *unused)
{
    (void)unused;
    for (;;) {
        TEMP_LOCK();
        if (!collector_requested && !stopping) {
            struct timespec until = {(time_t)time(NULL) + 60, 0};
            pthread_cond_timedwait(&collector_cond, &temp_lock, &until);
        }
        collector_requested = 0;
        int done = stopping;
        TEMP_UNLOCK();
        if (done) break;
        collect_work();
    }
    return NULL;
}

static int start_collector_locked(void)
{
    if (collector_started) return 1;
    collector_requested = 1;
    if (pthread_create(&collector_thread, NULL, collector_main, NULL)) return 0;
    collector_started = 1;
    return 1;
}

static void wake_collector(void)
{
    TEMP_LOCK();
    collector_requested = 1;
    pthread_cond_signal(&collector_cond);
    TEMP_UNLOCK();
}
#endif

static AsbClipboardSession *find_session(const AsbClipboardPathChar *parent)
{
    for (AsbClipboardSession *session = sessions; session; session = session->next)
#ifdef _WIN32
        if (!_wcsicmp(parent, session->parent)) return session;
#else
        if (!strcmp(parent, session->parent)) return session;
#endif
    return NULL;
}

static AsbClipboardSession *ensure_session(const AsbClipboardPathChar *parent)
{
    if (!parent || !*parent) return NULL;
    TEMP_LOCK();
    AsbClipboardSession *existing = stopping ? NULL : find_session(parent);
    int done = stopping || existing != NULL;
    TEMP_UNLOCK();
    if (done) return existing;

    AsbClipboardSession *created = calloc(1, sizeof(*created));
    if (!created) return NULL;
    created->parent = TEMP_PATH_DUP(parent);
    if (created->parent) created->keeper = temp_create(parent, NULL);
    if (!created->keeper) { free(created->parent); free(created); return NULL; }
#ifdef _WIN32
    const wchar_t *name = wcsrchr(created->keeper->root, L'\\');
    name = name ? name + 1 : created->keeper->root;
    for (size_t i = 0; i < wcslen(name) && i + 1 < sizeof(created->name); ++i) created->name[i] = (char)name[i];
#else
    const char *name = strrchr(created->keeper->root, '/');
    snprintf(created->name, sizeof(created->name), "%s", name ? name + 1 : created->keeper->root);
#endif
    release_handles(created->keeper, 1);
    compact_temp(created->keeper);
    TEMP_LOCK();
    existing = find_session(parent);
    if (!existing && !stopping && start_collector_locked()) {
        created->next = sessions;
        sessions = created;
        existing = created;
        created = NULL;
    }
    TEMP_UNLOCK();
    if (created) { temp_close(created->keeper); free(created->parent); free(created); }
    return existing;
}

AsbClipboardTemp *asb_clipboard_temp_create(const AsbClipboardPathChar *parent)
{
    AsbClipboardSession *session = ensure_session(parent);
    AsbClipboardTemp *temp = session ? temp_create(parent, session->name) : NULL;
    if (temp) temp->session = session;
    return temp;
}

void asb_clipboard_temp_collect(const AsbClipboardPathChar *parent)
{
    if (ensure_session(parent)) wake_collector();
}

void asb_clipboard_temp_publish(AsbClipboardTemp *temp, const void *owner, uint64_t token)
{
    if (!temp) return;
    temp->owner = owner;
    temp->token = token;
    release_handles(temp, 0);
    compact_temp(temp);
    TEMP_LOCK();
    if (clipboard_token == token) {
        temp->next = published;
        published = temp;
    } else retire_locked(temp, (uint64_t)time(NULL) + ASB_CLIP_TEMP_RETENTION);
    TEMP_UNLOCK();
    wake_collector();
}

void asb_clipboard_temp_changed(uint64_t token)
{
    TEMP_LOCK();
    clipboard_token = token;
    AsbClipboardTemp **cursor = &published;
    while (*cursor) {
        AsbClipboardTemp *temp = *cursor;
        if (temp->token != token) { *cursor = temp->next; retire_locked(temp, (uint64_t)time(NULL) + ASB_CLIP_TEMP_RETENTION); }
        else cursor = &temp->next;
    }
    TEMP_UNLOCK();
    wake_collector();
}

void asb_clipboard_temp_retire_owner(const void *owner)
{
    TEMP_LOCK();
    AsbClipboardTemp **cursor = &published;
    while (*cursor) {
        AsbClipboardTemp *temp = *cursor;
        if (temp->owner == owner) { *cursor = temp->next; retire_locked(temp, (uint64_t)time(NULL) + ASB_CLIP_TEMP_RETENTION); }
        else cursor = &temp->next;
    }
    TEMP_UNLOCK();
    wake_collector();
}

void asb_clipboard_temp_shutdown(void)
{
    TEMP_LOCK();
    stopping = 1;
    TEMP_UNLOCK();
    wake_collector();
#ifdef _WIN32
    if (collector_thread) { WaitForSingleObject(collector_thread, INFINITE); CloseHandle(collector_thread); }
    TEMP_LOCK();
    collector_thread = NULL;
    if (collector_event) CloseHandle(collector_event);
    collector_event = NULL;
    TEMP_UNLOCK();
#else
    if (collector_started) pthread_join(collector_thread, NULL);
    collector_started = 0;
#endif
    TEMP_LOCK();
    while (published) {
        AsbClipboardTemp *temp = published;
        published = temp->next;
        retire_locked(temp, (uint64_t)time(NULL) + ASB_CLIP_TEMP_RETENTION);
    }
    TEMP_UNLOCK();
    collect_work();
    TEMP_LOCK();
    for (AsbClipboardSession *session = sessions; session; session = session->next) {
        release_handles(session->keeper, 0);
        retire_locked(session->keeper, (uint64_t)time(NULL));
        session->keeper = NULL;
    }
    TEMP_UNLOCK();
    collect_work();
    TEMP_LOCK();
    while (retired) {
        AsbClipboardTemp *temp = retired;
        retired = temp->next;
        temp_close(temp);
    }
    while (sessions) {
        AsbClipboardSession *session = sessions;
        sessions = session->next;
        free(session->parent);
        free(session);
    }
    clipboard_token = 0;
    stopping = 0;
    TEMP_UNLOCK();
}
