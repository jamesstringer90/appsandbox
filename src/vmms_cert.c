/*
 * vmms_cert.c — Generate a self-signed VMMS certificate for HCS VMs.
 *
 * HCS VMs bypass VMMS (Virtual Machine Management Service), so there's
 * no VMMS certificate on the host.  Without it, vmwp.exe can't establish
 * VMBus IC channels and the guest-side integration services (shutdown,
 * heartbeat, time sync, …) start and immediately stop.
 *
 * This creates a machine-level self-signed cert with the Microsoft VMMS
 * OID (1.3.6.1.4.1.311.62.1.1.1) and grants the VIRTUAL_MACHINE SID
 * (S-1-5-83-0) read access to the private key — matching what Hyper-V's
 * VMMS role normally provides.
 */

#include "vmms_cert.h"
#include "ui.h"
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <sddl.h>
#include <stdio.h>

#pragma comment(lib, "crypt32.lib")

/* Microsoft VMMS identification OID */
#define VMMS_OID "1.3.6.1.4.1.311.62.1.1.1"

/* Key container name (machine-level) */
#define VMMS_CONTAINER L"AppSandbox-VMMS"

/* ASN.1 DER encoding: INTEGER(4) → 02 01 04 */
static const BYTE g_vmms_ext_value[] = { 0x02, 0x01, 0x04 };

/* Well-known SID for NT VIRTUAL MACHINE\Virtual Machines */
#define VM_SID_STR L"S-1-5-83-0"

/* ------------------------------------------------------------------ */

BOOL vmms_cert_exists(void)
{
    HCERTSTORE store;
    PCCERT_CONTEXT ctx = NULL;
    BOOL found = FALSE;

    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, L"My");
    if (!store)
        return FALSE;

    while ((ctx = CertEnumCertificatesInStore(store, ctx)) != NULL) {
        if (CertFindExtension(VMMS_OID,
                ctx->pCertInfo->cExtension,
                ctx->pCertInfo->rgExtension)) {
            found = TRUE;
            CertFreeCertificateContext(ctx);
            break;
        }
    }

    CertCloseStore(store, 0);
    return found;
}

/* Grant S-1-5-83-0 (VIRTUAL_MACHINE) read access to the private key file. */
static BOOL grant_vm_key_access(HCRYPTPROV hProv)
{
    char unique_a[256];
    wchar_t unique_w[256];
    wchar_t prog_data[MAX_PATH];
    wchar_t pk_path[MAX_PATH];
    DWORD unique_size = sizeof(unique_a);
    PSID vm_sid = NULL;
    PACL old_dacl = NULL;
    PACL new_dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    EXPLICIT_ACCESS_W ea;
    BOOL ok = FALSE;

    if (!CryptGetProvParam(hProv, PP_UNIQUE_CONTAINER,
            (BYTE *)unique_a, &unique_size, 0))
        return FALSE;

    MultiByteToWideChar(CP_ACP, 0, unique_a, -1, unique_w, 256);

    if (!GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH))
        wcscpy_s(prog_data, MAX_PATH, L"C:\\ProgramData");

    swprintf_s(pk_path, MAX_PATH,
        L"%s\\Microsoft\\Crypto\\RSA\\MachineKeys\\%s",
        prog_data, unique_w);

    if (!ConvertStringSidToSidW(VM_SID_STR, &vm_sid))
        return FALSE;

    if (GetNamedSecurityInfoW(pk_path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            NULL, NULL, &old_dacl, NULL, &sd) != ERROR_SUCCESS)
        goto done;

    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)vm_sid;

    if (SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) != ERROR_SUCCESS)
        goto done;

    if (SetNamedSecurityInfoW(pk_path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            NULL, NULL, new_dacl, NULL) == ERROR_SUCCESS) {
        ui_log(L"Granted VM access to private key: %s", pk_path);
        ok = TRUE;
    }

done:
    if (new_dacl) LocalFree(new_dacl);
    if (sd) LocalFree(sd);
    if (vm_sid) LocalFree(vm_sid);
    return ok;
}

BOOL vmms_cert_ensure(void)
{
    HCRYPTPROV hProv = 0;
    HCRYPTKEY hKey = 0;
    PCCERT_CONTEXT cert_ctx = NULL;
    HCERTSTORE store = NULL;
    BOOL result = FALSE;

    /* Subject name */
    wchar_t computer[256];
    wchar_t subject_str[512];
    BYTE subject_enc[512];
    DWORD subject_size;
    CERT_NAME_BLOB subject_blob;
    DWORD cn_size = 256;

    /* EKU extension: Server Authentication */
    LPSTR eku_oids[1];
    CERT_ENHKEY_USAGE eku_usage;
    BYTE eku_enc[256];
    DWORD eku_size;

    /* Key Usage extension: Key Encipherment | Data Encipherment */
    BYTE ku_byte;
    CRYPT_BIT_BLOB ku_bits;
    BYTE ku_enc[32];
    DWORD ku_size;

    /* Extensions array */
    CERT_EXTENSION exts[3];
    CERT_EXTENSIONS cert_exts;

    /* Validity period */
    SYSTEMTIME not_before, not_after;
    FILETIME ft;
    ULARGE_INTEGER uli;

    /* Key provider info */
    CRYPT_KEY_PROV_INFO kpi;

    /* Already have one? */
    if (vmms_cert_exists()) {
        ui_log(L"VMMS certificate found.");
        return TRUE;
    }

    ui_log(L"Creating VMMS certificate for integration services...");

    /* ---- Key container ---- */

    /* Delete stale container if it exists */
    CryptAcquireContextW(&hProv, VMMS_CONTAINER,
        L"Microsoft RSA SChannel Cryptographic Provider",
        PROV_RSA_SCHANNEL,
        CRYPT_MACHINE_KEYSET | CRYPT_DELETEKEYSET);
    hProv = 0;

    if (!CryptAcquireContextW(&hProv, VMMS_CONTAINER,
            L"Microsoft RSA SChannel Cryptographic Provider",
            PROV_RSA_SCHANNEL,
            CRYPT_MACHINE_KEYSET | CRYPT_NEWKEYSET)) {
        ui_log(L"Failed to create key container (0x%08X)", GetLastError());
        goto cleanup;
    }

    /* 2048-bit RSA, exportable */
    if (!CryptGenKey(hProv, AT_KEYEXCHANGE,
            (2048 << 16) | CRYPT_EXPORTABLE, &hKey)) {
        ui_log(L"Failed to generate RSA key (0x%08X)", GetLastError());
        goto cleanup;
    }

    /* ---- Subject ---- */

    GetComputerNameW(computer, &cn_size);
    swprintf_s(subject_str, 512, L"CN=AppSandbox-%s", computer);

    subject_size = sizeof(subject_enc);
    if (!CertStrToNameW(X509_ASN_ENCODING, subject_str,
            CERT_X500_NAME_STR, NULL,
            subject_enc, &subject_size, NULL)) {
        ui_log(L"Failed to encode subject (0x%08X)", GetLastError());
        goto cleanup;
    }
    subject_blob.cbData = subject_size;
    subject_blob.pbData = subject_enc;

    /* ---- Extensions ---- */

    /* 1. EKU: Server Authentication (1.3.6.1.5.5.7.3.1) */
    eku_oids[0] = szOID_PKIX_KP_SERVER_AUTH;
    eku_usage.cUsageIdentifier = 1;
    eku_usage.rgpszUsageIdentifier = eku_oids;
    eku_size = sizeof(eku_enc);
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE,
            &eku_usage, eku_enc, &eku_size)) {
        ui_log(L"Failed to encode EKU (0x%08X)", GetLastError());
        goto cleanup;
    }

    exts[0].pszObjId = szOID_ENHANCED_KEY_USAGE;
    exts[0].fCritical = FALSE;
    exts[0].Value.cbData = eku_size;
    exts[0].Value.pbData = eku_enc;

    /* 2. Key Usage: Key Encipherment (0x20) | Data Encipherment (0x10) */
    ku_byte = CERT_KEY_ENCIPHERMENT_KEY_USAGE | CERT_DATA_ENCIPHERMENT_KEY_USAGE;
    ku_bits.cbData = 1;
    ku_bits.pbData = &ku_byte;
    ku_bits.cUnusedBits = 0;
    ku_size = sizeof(ku_enc);
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_KEY_USAGE,
            &ku_bits, ku_enc, &ku_size)) {
        ui_log(L"Failed to encode KU (0x%08X)", GetLastError());
        goto cleanup;
    }

    exts[1].pszObjId = szOID_KEY_USAGE;
    exts[1].fCritical = FALSE;
    exts[1].Value.cbData = ku_size;
    exts[1].Value.pbData = ku_enc;

    /* 3. Microsoft VMMS OID (1.3.6.1.4.1.311.62.1.1.1) = {02, 01, 04} */
    exts[2].pszObjId = VMMS_OID;
    exts[2].fCritical = FALSE;
    exts[2].Value.cbData = sizeof(g_vmms_ext_value);
    exts[2].Value.pbData = (BYTE *)g_vmms_ext_value;

    cert_exts.cExtension = 3;
    cert_exts.rgExtension = exts;

    /* ---- Validity: yesterday → +100 years ---- */

    GetSystemTime(&not_before);
    SystemTimeToFileTime(&not_before, &ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart -= 864000000000ULL; /* subtract 1 day */
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    FileTimeToSystemTime(&ft, &not_before);

    not_after = not_before;
    not_after.wYear += 100;

    /* ---- Key provider info (links cert → key container) ---- */

    ZeroMemory(&kpi, sizeof(kpi));
    kpi.pwszContainerName = VMMS_CONTAINER;
    kpi.pwszProvName = L"Microsoft RSA SChannel Cryptographic Provider";
    kpi.dwProvType = PROV_RSA_SCHANNEL;
    kpi.dwFlags = CRYPT_MACHINE_KEYSET;
    kpi.dwKeySpec = AT_KEYEXCHANGE;

    /* ---- Create self-signed certificate ---- */

    cert_ctx = CertCreateSelfSignCertificate(
        hProv, &subject_blob, 0, &kpi,
        NULL, &not_before, &not_after, &cert_exts);

    if (!cert_ctx) {
        ui_log(L"CertCreateSelfSignCertificate failed (0x%08X)", GetLastError());
        goto cleanup;
    }

    /* ---- Install to local machine "My" store ---- */

    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_LOCAL_MACHINE, L"My");
    if (!store) {
        ui_log(L"Failed to open machine cert store (0x%08X)", GetLastError());
        goto cleanup;
    }

    if (!CertAddCertificateContextToStore(store, cert_ctx,
            CERT_STORE_ADD_REPLACE_EXISTING, NULL)) {
        ui_log(L"Failed to add cert to store (0x%08X)", GetLastError());
        goto cleanup;
    }

    /* ---- Grant VIRTUAL_MACHINE SID access to private key ---- */

    if (!grant_vm_key_access(hProv))
        ui_log(L"Warning: could not ACL private key for VM access.");

    ui_log(L"VMMS certificate created successfully (CN=AppSandbox-%s).", computer);
    result = TRUE;

cleanup:
    if (hKey) CryptDestroyKey(hKey);
    if (cert_ctx) CertFreeCertificateContext(cert_ctx);
    if (store) CertCloseStore(store, 0);
    if (hProv) CryptReleaseContext(hProv, 0);
    return result;
}
