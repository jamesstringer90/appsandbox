/*
 * asb_shares.c -- host folders shared into a Windows guest as SMB drive letters.
 *
 * Publishing a share does three things on the host:
 *   1. create/refresh an SMB share for the folder (net share)
 *   2. open inbound TCP 445 for the VM's NAT subnet only (netsh advfirewall)
 *   3. keep the password for the share account DPAPI-protected in the config
 *
 * The guest agent does the mapping (cmdkey + net use) in the interactive user
 * session; asb_share_guest_line() builds the line it receives over the agent
 * control channel.
 */

#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <lm.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>
#include <ctype.h>

#include "asb_shares.h"
#include "hcn_network.h"
#include "ui.h"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "advapi32.lib")

/* ---- Small helpers ---- */

/* Run a command line hidden and wait for it. stdout/stderr are captured into
   out (newlines collapsed to spaces) so a failure carries its reason - "System
   error 5 has occurred. Access is denied." instead of a bare exit code.
   Returns the exit code, or -1 if the process could not be started. */
static int run_hidden(const wchar_t *cmdline, char *out, size_t out_chars)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sec;
    HANDLE rd = NULL, wr = NULL;
    wchar_t *mut;
    DWORD code = 0;
    size_t len = wcslen(cmdline) + 1;

    if (out && out_chars) out[0] = '\0';

    mut = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!mut) return -1;
    wcscpy_s(mut, len, cmdline);

    if (out && out_chars) {
        sec.nLength = sizeof(sec);
        sec.bInheritHandle = TRUE;
        sec.lpSecurityDescriptor = NULL;
        if (CreatePipe(&rd, &wr, &sec, 0))
            SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        else
            rd = wr = NULL;
    }

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (wr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = wr;
        si.hStdInput = NULL;
    }
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, mut, NULL, NULL, wr ? TRUE : FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        if (wr) CloseHandle(wr);
        if (rd) CloseHandle(rd);
        HeapFree(GetProcessHeap(), 0, mut);
        return -1;
    }
    if (wr) CloseHandle(wr);          /* our copy; the child holds the other end */

    if (rd) {
        DWORD got = 0, total = 0;
        while (total + 1 < (DWORD)out_chars &&
               ReadFile(rd, out + total, (DWORD)(out_chars - 1 - total), &got, NULL) && got > 0)
            total += got;
        out[total] = '\0';
        CloseHandle(rd);
        {
            char *c;
            for (c = out; *c; c++)
                if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        }
    }

    WaitForSingleObject(pi.hProcess, 30000);
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = (DWORD)-1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    HeapFree(GetProcessHeap(), 0, mut);
    return (int)code;
}

static BOOL dir_exists(const wchar_t *path)
{
    DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* TRUE when path is inside root (case-insensitive, path-boundary aware). */
static BOOL path_under(const wchar_t *path, const wchar_t *root)
{
    size_t n;
    if (!root || !root[0]) return FALSE;
    n = wcslen(root);
    if (_wcsnicmp(path, root, n) != 0) return FALSE;
    return path[n] == L'\0' || path[n] == L'\\';
}

static void env_dir(const wchar_t *name, wchar_t *out, size_t out_chars)
{
    out[0] = L'\0';
    GetEnvironmentVariableW(name, out, (DWORD)out_chars);
}

/* A folder may not be a drive root, one of the system directories, or the app's
   own data directory. */
static BOOL path_is_allowed(const wchar_t *path)
{
    static const wchar_t *const env_blocks[] = {
        L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"
    };
    wchar_t blocked[MAX_PATH];
    int i;

    if (wcslen(path) <= 3) return FALSE;              /* "C:\" or shorter */
    if (path[1] != L':' || path[2] != L'\\') return FALSE;

    /* SystemRoot, Program Files and ProgramData cover the Windows directory and
       the VM disk tree (which lives under ProgramData). */
    for (i = 0; i < (int)(sizeof(env_blocks) / sizeof(env_blocks[0])); i++) {
        env_dir(env_blocks[i], blocked, ARRAYSIZE(blocked));
        if (blocked[0] && path_under(path, blocked)) return FALSE;
    }

    return TRUE;
}

/* ---- Password protection (DPAPI, hex-encoded for the config file) ---- */

static void bytes_to_hex(const BYTE *in, DWORD n, wchar_t *out, size_t out_chars)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    DWORD i;

    if (out_chars < (size_t)n * 2 + 1) {
        if (out_chars) out[0] = L'\0';
        return;
    }
    for (i = 0; i < n; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = L'\0';
}

static BOOL hex_to_bytes(const wchar_t *in, BYTE *out, DWORD out_cap, DWORD *out_len)
{
    DWORD n, i;

    n = (DWORD)wcslen(in);
    if (n == 0 || (n % 2) != 0 || n / 2 > out_cap) return FALSE;
    for (i = 0; i < n; i++) {
        wchar_t c = in[i];
        int v;
        if (c >= L'0' && c <= L'9') v = c - L'0';
        else if (c >= L'a' && c <= L'f') v = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v = c - L'A' + 10;
        else return FALSE;
        if ((i & 1) == 0) out[i / 2] = (BYTE)(v << 4);
        else out[i / 2] |= (BYTE)v;
    }
    *out_len = n / 2;
    return TRUE;
}

BOOL asb_share_protect_password(const wchar_t *plain, wchar_t *out, size_t out_chars)
{
    DATA_BLOB in, encrypted;
    char utf8[1024];
    int n;

    if (out_chars == 0) return FALSE;
    out[0] = L'\0';
    if (!plain || !plain[0]) return TRUE;   /* no password to protect */

    n = WideCharToMultiByte(CP_UTF8, 0, plain, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0) return FALSE;

    in.pbData = (BYTE *)utf8;
    in.cbData = (DWORD)n - 1;               /* exclude the NUL */
    encrypted.pbData = NULL;
    encrypted.cbData = 0;

    if (!CryptProtectData(&in, L"AppSandbox shared folder", NULL, NULL, NULL,
                          CRYPTPROTECT_UI_FORBIDDEN, &encrypted))
        return FALSE;

    bytes_to_hex(encrypted.pbData, encrypted.cbData, out, out_chars);
    LocalFree(encrypted.pbData);
    SecureZeroMemory(utf8, sizeof(utf8));
    return out[0] != L'\0';
}

BOOL asb_share_unprotect_password(const wchar_t *enc, wchar_t *out, size_t out_chars)
{
    DATA_BLOB in, plain;
    BYTE blob[1024];
    DWORD blob_len = 0;
    int n;

    if (out_chars == 0) return FALSE;
    out[0] = L'\0';
    if (!enc || !enc[0]) return TRUE;

    if (!hex_to_bytes(enc, blob, sizeof(blob), &blob_len)) return FALSE;
    in.pbData = blob;
    in.cbData = blob_len;
    plain.pbData = NULL;
    plain.cbData = 0;

    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain))
        return FALSE;

    /* The protected bytes carry no terminator, and MultiByteToWideChar only
       writes one for a -1 length: terminate the string ourselves, or the
       password runs on into whatever follows in the buffer. */
    n = MultiByteToWideChar(CP_UTF8, 0, (const char *)plain.pbData, (int)plain.cbData,
                            out, (int)out_chars - 1);
    if (n > 0) out[n] = L'\0';
    LocalFree(plain.pbData);
    SecureZeroMemory(blob, sizeof(blob));
    return n > 0;
}

/* ---- Naming ---- */

void asb_shares_name_for_vm(AsbHostShareList *list, const wchar_t *vm_name)
{
    wchar_t safe[48];
    int i, n = 0;

    if (!list) return;

    for (i = 0; vm_name && vm_name[i] && n < (int)ARRAYSIZE(safe) - 1; i++) {
        wchar_t c = vm_name[i];
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')
            safe[n++] = c;
    }
    safe[n] = L'\0';
    if (n == 0) wcscpy_s(safe, ARRAYSIZE(safe), L"VM");

    for (i = 0; i < list->count; i++) {
        if (list->items[i].share_name[0]) continue;
        swprintf_s(list->items[i].share_name, ASB_SHARE_NAME_MAX,
                   L"AppSandbox.%s.%d", safe, i);
    }
}

/* The guest writes through an SMB session. A LogonUser NETWORK token is only
   identification-level, so impersonating it and calling CreateFile fails with
   error 1346 before the ACL is consulted, and a local administrator's SMB
   session does not keep the Administrators group. Evaluate the directory DACL
   against the network token's groups with those elevated groups deny-only.
   That is the access the mapped drive actually has. */

/* icacls "(OI)(CI)M": read, write, execute and delete, on this folder and on
   files and subfolders created in it. */
#define SHARE_MODIFY_MASK (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)

#define SMB_EXTRA_SIDS 4

typedef struct {
    PSID user;
    PTOKEN_GROUPS groups;
    PSID extra[SMB_EXTRA_SIDS];
    int extra_count;
    BOOL filter_admins;
    BYTE extra_buf[SMB_EXTRA_SIDS][SECURITY_MAX_SID_SIZE];
} SmbView;

static DWORD reg_dword(const wchar_t *name, DWORD fallback)
{
    HKEY key;
    DWORD val = fallback, cb = sizeof(val), type = 0;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return fallback;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&val, &cb) != ERROR_SUCCESS ||
        type != REG_DWORD)
        val = fallback;
    RegCloseKey(key);
    return val;
}

static BOOL is_nt_authority(PSID sid)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    return IsValidSid(sid) &&
           memcmp(GetSidIdentifierAuthority(sid), &nt, sizeof(nt)) == 0;
}

/* Groups UAC removes from a local administrator's network token. An allow ACE
   for one of these does not let the guest write; a deny ACE still does. */
static BOOL is_elevated_group(PSID sid)
{
    PUCHAR count;
    DWORD domain, rid;

    if (!is_nt_authority(sid)) return FALSE;
    count = GetSidSubAuthorityCount(sid);
    if (*count < 2) return FALSE;
    domain = *GetSidSubAuthority(sid, 0);
    rid = *GetSidSubAuthority(sid, *count - 1);
    if (domain == SECURITY_BUILTIN_DOMAIN_RID) {
        return rid == DOMAIN_ALIAS_RID_ADMINS ||
               rid == DOMAIN_ALIAS_RID_POWER_USERS ||
               rid == DOMAIN_ALIAS_RID_ACCOUNT_OPS ||
               rid == DOMAIN_ALIAS_RID_SYSTEM_OPS ||
               rid == DOMAIN_ALIAS_RID_PRINT_OPS ||
               rid == DOMAIN_ALIAS_RID_BACKUP_OPS ||
               rid == DOMAIN_ALIAS_RID_NETWORK_CONFIGURATION_OPS ||
               rid == DOMAIN_ALIAS_RID_CRYPTO_OPERATORS;
    }
    if (domain == SECURITY_NT_NON_UNIQUE) {
        return rid == DOMAIN_GROUP_RID_ADMINS ||
               rid == DOMAIN_GROUP_RID_SCHEMA_ADMINS ||
               rid == DOMAIN_GROUP_RID_ENTERPRISE_ADMINS;
    }
    return FALSE;
}

static BOOL is_local_builtin_admin(PSID user)
{
    wchar_t name[256], domain[256], computer[256];
    DWORD name_len = ARRAYSIZE(name), domain_len = ARRAYSIZE(domain);
    DWORD computer_len = ARRAYSIZE(computer);
    SID_NAME_USE use;
    PUCHAR count;

    count = GetSidSubAuthorityCount(user);
    if (!count || *count < 1) return FALSE;
    if (*GetSidSubAuthority(user, *count - 1) != DOMAIN_USER_RID_ADMIN) return FALSE;
    if (!LookupAccountSidW(NULL, user, name, &name_len, domain, &domain_len, &use))
        return FALSE;
    if (!GetComputerNameW(computer, &computer_len)) return FALSE;
    return _wcsicmp(domain, computer) == 0;
}

static BOOL network_filters_admins(PSID user, const PTOKEN_GROUPS groups)
{
    DWORD i;

    if (reg_dword(L"EnableLUA", 1) == 0) return FALSE;
    if (reg_dword(L"LocalAccountTokenFilterPolicy", 0) == 1) return FALSE;
    if (is_local_builtin_admin(user) && reg_dword(L"FilterAdministratorToken", 0) == 0)
        return FALSE;
    if (!groups) return FALSE;
    for (i = 0; i < groups->GroupCount; i++) {
        PSID sid = groups->Groups[i].Sid;
        if (sid && (groups->Groups[i].Attributes & SE_GROUP_ENABLED) && is_elevated_group(sid))
            return TRUE;
    }
    return FALSE;
}

static void smb_view_add_well_known(SmbView *view, WELL_KNOWN_SID_TYPE type)
{
    DWORD n;

    if (view->extra_count >= SMB_EXTRA_SIDS) return;
    n = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(type, NULL, view->extra_buf[view->extra_count], &n)) return;
    view->extra[view->extra_count] = (PSID)view->extra_buf[view->extra_count];
    view->extra_count++;
}

static BOOL sid_in_view(const SmbView *view, PSID sid, BOOL deny)
{
    DWORD i;
    int e;

    if (!sid || !IsValidSid(sid)) return FALSE;
    if (view->user && EqualSid(sid, view->user)) return TRUE;
    if (view->groups) {
        for (i = 0; i < view->groups->GroupCount; i++) {
            SID_AND_ATTRIBUTES *g = &view->groups->Groups[i];
            if (!g->Sid || !IsValidSid(g->Sid) || !EqualSid(g->Sid, sid)) continue;
            /* Elevated groups are deny-only on the guest's SMB session. */
            if (view->filter_admins && is_elevated_group(sid)) return deny;
            if (g->Attributes & SE_GROUP_ENABLED) return TRUE;
            if (deny && (g->Attributes & SE_GROUP_USE_FOR_DENY_ONLY)) return TRUE;
            return FALSE;
        }
    }
    for (e = 0; e < view->extra_count; e++)
        if (EqualSid(view->extra[e], sid)) return TRUE;
    return FALSE;
}

static DWORD map_generic_file(DWORD mask)
{
    DWORD out = mask;

    if (mask & GENERIC_ALL) {
        out &= ~GENERIC_ALL;
        out |= FILE_ALL_ACCESS;
    }
    if (mask & GENERIC_READ) {
        out &= ~GENERIC_READ;
        out |= FILE_GENERIC_READ;
    }
    if (mask & GENERIC_WRITE) {
        out &= ~GENERIC_WRITE;
        out |= FILE_GENERIC_WRITE;
    }
    if (mask & GENERIC_EXECUTE) {
        out &= ~GENERIC_EXECUTE;
        out |= FILE_GENERIC_EXECUTE;
    }
    return out;
}

/* 1 = granted, 0 = not granted, -1 = an explicit deny blocks it.
   for_file evaluates the ACL a new file in the folder would inherit. The
   guest probe creates a file, reads it back and deletes it. */
static int acl_result(const SmbView *view, PACL acl, DWORD desired, BOOL for_file,
                      PSID creator_owner)
{
    DWORD i, still = desired;

    if (!acl) return 1;   /* NULL DACL grants everyone full control */
    for (i = 0; i < acl->AceCount; i++) {
        ACE_HEADER *header;
        ACCESS_ALLOWED_ACE *ace;
        PSID sid, match;
        BOOL denied;
        DWORD mask;

        if (!GetAce(acl, i, (LPVOID *)&header)) continue;
        denied = header->AceType == ACCESS_DENIED_ACE_TYPE;
        if (!denied && header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if (for_file) {
            if ((header->AceFlags & OBJECT_INHERIT_ACE) == 0) continue;
        } else if (header->AceFlags & INHERIT_ONLY_ACE) {
            continue;
        }

        ace = (ACCESS_ALLOWED_ACE *)header;
        sid = (PSID)&ace->SidStart;
        if (!IsValidSid(sid)) continue;
        match = sid;
        /* A new file is owned by the account that creates it. */
        if (for_file && creator_owner && EqualSid(sid, creator_owner))
            match = view->user;
        if (!sid_in_view(view, match, denied)) continue;

        mask = map_generic_file(ace->Mask);
        if (denied) {
            if (mask & still) return -1;
            continue;
        }
        still &= ~mask;
        if (still == 0) return 1;
    }
    return 0;
}

static int smb_folder_access(const SmbView *view, PACL dacl)
{
    BYTE creator[SECURITY_MAX_SID_SIZE];
    DWORD creator_len = sizeof(creator);
    PSID creator_owner = NULL;
    int dir, file;

    if (CreateWellKnownSid(WinCreatorOwnerSid, NULL, creator, &creator_len))
        creator_owner = (PSID)creator;
    dir = acl_result(view, dacl, FILE_LIST_DIRECTORY | FILE_ADD_FILE, FALSE, NULL);
    file = acl_result(view, dacl,
                      FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                      DELETE | SYNCHRONIZE,
                      TRUE, creator_owner);
    if (dir < 0 || file < 0) return -1;
    if (dir == 0 || file == 0) return 0;
    return 1;
}

/* Add an explicit inheritable Modify ACE for the share account. This is the
   same grant that makes an administrator-only folder writable from the VM. */
static DWORD grant_share_modify(const wchar_t *path, PSID user)
{
    EXPLICIT_ACCESSW access;
    PACL old_dacl = NULL, new_dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    DWORD rc;

    ZeroMemory(&access, sizeof(access));
    access.grfAccessPermissions = SHARE_MODIFY_MASK;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = (LPWSTR)user;

    rc = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               NULL, NULL, &old_dacl, NULL, &sd);
    if (rc == ERROR_SUCCESS)
        rc = SetEntriesInAclW(1, &access, old_dacl, &new_dacl);
    if (sd) LocalFree(sd);
    if (rc == ERROR_SUCCESS)
        rc = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                   NULL, NULL, new_dacl, NULL);
    if (new_dacl) LocalFree(new_dacl);
    return rc;
}

/* ---- Validation ---- */

/* Prove the share's account exists, the stored password is its password, and the
   guest's SMB session can create, read and delete a file in the folder. When
   the password is right but the ACL would deny that session, grant the account
   an inheritable Modify entry (the guest drops administrator rights). Returns
   NULL when the share is usable, otherwise a message. */
static const wchar_t *check_share_account(const AsbHostShare *s)
{
    static wchar_t msg[1024];
    wchar_t pass[256], account[192], domain[128], name[160], *slash;
    HANDLE token = NULL;
    DWORD err, need = 0;
    BYTE user_buf[SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER)];
    TOKEN_USER *token_user;
    PSECURITY_DESCRIPTOR sd = NULL;
    PACL dacl = NULL;
    PTOKEN_GROUPS groups = NULL;
    SmbView view;
    int access;

    /* An inherited password was checked when it was stored; re-checking it on
       every save would burn sign-in attempts against the account's lockout
       policy without telling us anything new. Runtime status covers the case
       where the account's password changed behind our back. */
    if (s->skip_account_check) return NULL;

    if (!asb_share_unprotect_password(s->pass_enc, pass, ARRAYSIZE(pass)))
        return L"The stored password for this shared folder could not be read; enter it again.";

    /* An explicit domain ("MACHINE\user" or ".\user") is used as given; a bare
       name is a local account on this host. */
    wcscpy_s(account, ARRAYSIZE(account), s->user);
    slash = wcschr(account, L'\\');
    if (slash) {
        *slash = L'\0';
        wcscpy_s(domain, ARRAYSIZE(domain), account);
        wcscpy_s(name, ARRAYSIZE(name), slash + 1);
    } else {
        domain[0] = L'\0';
        wcscpy_s(name, ARRAYSIZE(name), account);
    }

    if (!LogonUserW(name, domain[0] ? domain : L".", pass, LOGON32_LOGON_NETWORK,
                    LOGON32_PROVIDER_DEFAULT, &token)) {
        err = GetLastError();
        SecureZeroMemory(pass, sizeof(pass));
        switch (err) {
        case ERROR_LOGON_FAILURE:            /* 1326 */
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account or password for \"%s\" is incorrect.", s->user);
            break;
        case ERROR_ACCOUNT_DISABLED:
            swprintf_s(msg, ARRAYSIZE(msg), L"The host account \"%s\" is disabled.", s->user);
            break;
        case ERROR_ACCOUNT_RESTRICTION:      /* 1327 - often a blank password */
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account \"%s\" cannot sign in with this password (Windows policy).",
                s->user);
            break;
        case ERROR_ACCOUNT_LOCKED_OUT:       /* 1909 */
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account \"%s\" is locked out after failed sign-in attempts. "
                L"Unlock it, or wait for the lockout window to pass, then try again.", s->user);
            break;
        case ERROR_PASSWORD_EXPIRED:         /* 1330 */
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The password for the host account \"%s\" has expired.", s->user);
            break;
        case ERROR_PASSWORD_MUST_CHANGE:     /* 1907 */
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account \"%s\" must change its password before it can sign in.", s->user);
            break;
        case ERROR_ACCOUNT_EXPIRED:
            swprintf_s(msg, ARRAYSIZE(msg), L"The host account \"%s\" has expired.", s->user);
            break;
        default:
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account \"%s\" could not be used (error %lu).", s->user, err);
            break;
        }
        return msg;
    }
    SecureZeroMemory(pass, sizeof(pass));

    ZeroMemory(&view, sizeof(view));
    if (!GetTokenInformation(token, TokenUser, user_buf, sizeof(user_buf), &need)) {
        err = GetLastError();
        CloseHandle(token);
        swprintf_s(msg, ARRAYSIZE(msg),
            L"The host account \"%s\" could not be used (error %lu).", s->user, err);
        return msg;
    }
    token_user = (TOKEN_USER *)user_buf;
    view.user = token_user->User.Sid;

    GetTokenInformation(token, TokenGroups, NULL, 0, &need);
    if (need) {
        groups = (PTOKEN_GROUPS)HeapAlloc(GetProcessHeap(), 0, need);
        if (groups && GetTokenInformation(token, TokenGroups, groups, need, &need))
            view.groups = groups;
    }
    view.filter_admins = network_filters_admins(view.user, view.groups);
    /* A network logon is also Everyone, Authenticated Users, Users and NETWORK,
       even when the token's group list does not spell each of those out. */
    smb_view_add_well_known(&view, WinWorldSid);
    smb_view_add_well_known(&view, WinAuthenticatedUserSid);
    smb_view_add_well_known(&view, WinBuiltinUsersSid);
    smb_view_add_well_known(&view, WinNetworkSid);
    CloseHandle(token);
    token = NULL;

    err = GetNamedSecurityInfoW((LPWSTR)s->host_path, SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
    if (err != ERROR_SUCCESS) {
        if (groups) HeapFree(GetProcessHeap(), 0, groups);
        swprintf_s(msg, ARRAYSIZE(msg),
            L"Could not read the permissions on %s (error %lu).", s->host_path, err);
        return msg;
    }
    access = smb_folder_access(&view, dacl);
    LocalFree(sd);
    sd = NULL;

    if (access == 0) {
        /* No allow entry covers the guest. Add one, then check again so a
           protected or deny-only ACL is not reported as shared. */
        err = grant_share_modify(s->host_path, view.user);
        if (err != ERROR_SUCCESS) {
            if (groups) HeapFree(GetProcessHeap(), 0, groups);
            swprintf_s(msg, ARRAYSIZE(msg),
                L"The host account \"%s\" cannot write %s from the VM, and AppSandbox "
                L"could not grant it access (error %lu). Grant it yourself: "
                L"icacls \"%s\" /grant \"%s:(OI)(CI)M\".",
                s->user, s->host_path, err, s->host_path, s->user);
            return msg;
        }
        err = GetNamedSecurityInfoW((LPWSTR)s->host_path, SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd);
        access = (err == ERROR_SUCCESS) ? smb_folder_access(&view, dacl) : 0;
        if (sd) LocalFree(sd);
        if (access == 1)
            ui_log(L"Granted %s Modify on %s so the VM can write the shared folder.",
                   s->user, s->host_path);
    }

    if (groups) HeapFree(GetProcessHeap(), 0, groups);

    if (access == 1) return NULL;
    if (access < 0) {
        swprintf_s(msg, ARRAYSIZE(msg),
            L"The host account \"%s\" is denied access to %s. Remove that deny entry, "
            L"or choose a different folder.", s->user, s->host_path);
        return msg;
    }
    swprintf_s(msg, ARRAYSIZE(msg),
        L"The host account \"%s\" still cannot write %s from the VM. Grant it yourself: "
        L"icacls \"%s\" /grant \"%s:(OI)(CI)M\".",
        s->user, s->host_path, s->host_path, s->user);
    return msg;
}

const wchar_t *asb_shares_validate(const AsbHostShareList *list, int network_mode)
{
    int i, j;

    if (!list || list->count == 0) return NULL;
    if (list->count > ASB_MAX_HOST_SHARES)
        return L"Too many shared folders (maximum 8).";

    if (network_mode != 1 /* NET_NAT */)
        return L"Shared folders need NAT networking: the guest reaches the host at the NAT gateway.";

    for (i = 0; i < list->count; i++) {
        const AsbHostShare *s = &list->items[i];

        if (!s->host_path[0]) return L"Every shared folder needs a host path.";
        if (!dir_exists(s->host_path))
            return L"A shared folder does not exist on the host (or is not a folder).";
        if (!path_is_allowed(s->host_path))
            return L"Cannot share a drive root or a Windows system folder.";
        if (!s->share_name[0])
            return L"Shared folder has no share name.";
        if (!s->user[0])
            return L"Shared folders need a host user name: the guest signs in to the share as that account.";
        if (!s->pass_enc[0])
            return L"Shared folders need the host account's password.";

        if (s->drive_letter) {
            wchar_t c = (wchar_t)towupper(s->drive_letter);
            if (c < L'D' || c > L'Z')
                return L"Drive letter must be an unused letter from D: to Z:.";
        }

        for (j = 0; j < i; j++) {
            const AsbHostShare *p = &list->items[j];
            if (_wcsicmp(p->host_path, s->host_path) == 0)
                return L"The same host folder is shared twice.";
            if (_wcsicmp(p->share_name, s->share_name) == 0)
                return L"Two shared folders have the same share name.";
            if (p->drive_letter && s->drive_letter &&
                towupper(p->drive_letter) == towupper(s->drive_letter))
                return L"Two shared folders claim the same drive letter.";
        }

        /* Last, because it talks to the account database: the account must
           exist, match the stored password, and be able to open the folder. */
        {
            const wchar_t *account_error = check_share_account(s);
            if (account_error) return account_error;
        }
    }
    return NULL;
}

/* ---- Publish / withdraw ---- */

/* Firewall rule name derives from the share name alone, so withdrawing a share
   never needs the VM name again. */
static void rule_name_for(const wchar_t *share_name, wchar_t *out, size_t out_chars)
{
    swprintf_s(out, out_chars, L"AppSandbox share %s", share_name);
}

static void firewall_delete(const wchar_t *share_name)
{
    wchar_t rule[256], cmd[600];
    rule_name_for(share_name, rule, ARRAYSIZE(rule));
    swprintf_s(cmd, ARRAYSIZE(cmd),
        L"netsh.exe advfirewall firewall delete rule name=\"%s\"", rule);
    (void)run_hidden(cmd, NULL, 0);
}

static BOOL firewall_allow_share(const wchar_t *share_name)
{
    wchar_t rule[256], cmd[800];
    char why[512];
    const char *base = hcn_nat_subnet_base();

    rule_name_for(share_name, rule, ARRAYSIZE(rule));
    firewall_delete(share_name);   /* idempotent: replace the old rule */

    swprintf_s(cmd, ARRAYSIZE(cmd),
        L"netsh.exe advfirewall firewall add rule name=\"%s\" dir=in action=allow "
        L"protocol=TCP localport=445 remoteip=%S.0/24 profile=any",
        rule, base);
    if (run_hidden(cmd, why, sizeof(why)) != 0) {
        ui_log(L"Firewall rule for share %s failed: %S", share_name, why);
        return FALSE;
    }
    return TRUE;
}

HRESULT asb_shares_publish(const wchar_t *vm_name, const AsbHostShareList *list)
{
    int i, published = 0, failed = 0;
    wchar_t cmd[1600];
    char why[512];
    int rc;

    (void)vm_name;
    if (!list || list->count == 0) return S_OK;

    for (i = 0; i < list->count; i++) {
        const AsbHostShare *s = &list->items[i];

        if (!dir_exists(s->host_path)) {
            ui_log(L"Shared folder missing on the host: %s (share %s skipped)",
                   s->host_path, s->share_name);
            failed++;
            continue;
        }

        /* Share names are namespaced ("AppSandbox.<vm>.<n>"), so replacing an
           entry of ours is safe; delete first so a stale grant cannot linger. */
        swprintf_s(cmd, ARRAYSIZE(cmd), L"net.exe share \"%s\" /delete /y", s->share_name);
        (void)run_hidden(cmd, NULL, 0);

        if (swprintf_s(cmd, ARRAYSIZE(cmd),
                L"net.exe share \"%s\"=\"%s\" /grant:\"%s\",FULL /cache:none "
                L"/remark:\"AppSandbox shared folder\"",
                s->share_name, s->host_path, s->user) < 0) {
            ui_log(L"Failed to publish share %s: the command line does not fit.", s->share_name);
            failed++;
            continue;
        }
        rc = run_hidden(cmd, why, sizeof(why));
        if (rc != 0) {
            ui_log(L"Failed to publish share %s for %s: %S", s->share_name, s->host_path, why);
            failed++;
            continue;
        }

        if (!firewall_allow_share(s->share_name)) {
            /* Leave nothing half-done: the guest could not reach it anyway. */
            swprintf_s(cmd, ARRAYSIZE(cmd), L"net.exe share \"%s\" /delete /y", s->share_name);
            (void)run_hidden(cmd, NULL, 0);
            failed++;
            continue;
        }

        ui_log(L"Shared folder published: %s -> \\\\%S.1\\%s",
               s->host_path, hcn_nat_subnet_base(), s->share_name);
        published++;
    }

    if (failed) return E_FAIL;
    return published ? S_OK : S_FALSE;
}

void asb_shares_withdraw(const AsbHostShareList *list)
{
    int i;
    wchar_t cmd[600];

    if (!list) return;

    for (i = 0; i < list->count; i++) {
        const AsbHostShare *s = &list->items[i];
        if (!s->share_name[0]) continue;

        swprintf_s(cmd, ARRAYSIZE(cmd), L"net.exe share \"%s\" /delete /y", s->share_name);
        (void)run_hidden(cmd, NULL, 0);
        firewall_delete(s->share_name);
        ui_log(L"Shared folder withdrawn: %s", s->share_name);
    }
}

/* ---- Orphan cleanup (unclean exit) ---- */

int asb_shares_enum_published(wchar_t (*names)[ASB_SHARE_NAME_MAX], int max)
{
    PSHARE_INFO_2 info = NULL;
    DWORD read = 0, total = 0, resume = 0;
    NET_API_STATUS status;
    int found = 0, i;

    if (!names || max <= 0) return 0;

    status = NetShareEnum(NULL, 2, (LPBYTE *)&info, MAX_PREFERRED_LENGTH,
                          &read, &total, &resume);
    if (status != NERR_Success && status != ERROR_MORE_DATA) {
        ui_log(L"Could not list host shares (error %lu); leftover shares are not cleaned up.",
               (unsigned long)status);
        return 0;
    }

    for (i = 0; i < (int)read && found < max; i++) {
        if (_wcsnicmp(info[i].shi2_netname, L"AppSandbox.", 11) != 0) continue;
        wcsncpy_s(names[found], ASB_SHARE_NAME_MAX, info[i].shi2_netname, _TRUNCATE);
        found++;
    }

    if (info) NetApiBufferFree(info);
    return found;
}

void asb_shares_remove_by_name(const wchar_t *share_name)
{
    wchar_t cmd[600];

    if (!share_name || !share_name[0]) return;

    swprintf_s(cmd, ARRAYSIZE(cmd), L"net.exe share \"%s\" /delete /y", share_name);
    (void)run_hidden(cmd, NULL, 0);
    firewall_delete(share_name);
    ui_log(L"Removed leftover share %s (no running VM uses it).", share_name);
}

/* ---- Guest-side mapping line ---- */

BOOL asb_share_guest_line(const AsbHostShare *share, wchar_t *out, size_t out_chars)
{
    wchar_t pass[256];
    wchar_t letter[8];
    wchar_t qualified[192];
    const wchar_t *user;

    if (!share || !out || out_chars == 0) return FALSE;
    out[0] = L'\0';

    if (!asb_share_unprotect_password(share->pass_enc, pass, ARRAYSIZE(pass)))
        return FALSE;

    /* A bare account name would be offered to the server with the guest's own
       domain. Local accounts on this host need the machine name. */
    user = share->user;
    if (share->user[0] && wcschr(share->user, L'\\') == NULL) {
        wchar_t computer[80];
        DWORD computer_len = ARRAYSIZE(computer);
        qualified[0] = L'\0';
        if (GetComputerNameW(computer, &computer_len))
            swprintf_s(qualified, ARRAYSIZE(qualified), L"%s\\%s", computer, share->user);
        if (qualified[0]) user = qualified;
    }

    if (share->drive_letter)
        swprintf_s(letter, ARRAYSIZE(letter), L"%c", towupper(share->drive_letter));
    else
        wcscpy_s(letter, ARRAYSIZE(letter), L"auto");

    if (swprintf_s(out, out_chars, L"%s|\\\\%S.1\\%s|%s|%s",
                   letter, hcn_nat_subnet_base(), share->share_name,
                   user, pass) < 0) {
        SecureZeroMemory(pass, sizeof(pass));
        return FALSE;
    }

    SecureZeroMemory(pass, sizeof(pass));
    return TRUE;
}
