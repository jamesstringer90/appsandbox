#ifndef ASB_SHARES_H
#define ASB_SHARES_H

/*
 * asb_shares.h -- host folders shared into a Windows guest as SMB drive letters.
 *
 * A shared folder is published as an SMB share on the host, reachable from the
 * guest at the VM's NAT gateway (\\<subnet>.1\<share>). The guest agent maps the
 * drive letter in the interactive user session (cmdkey + net use). Windows
 * guests only: the transport is the SMB client both OSes already ship.
 */

#include <windows.h>

/* Shared with asb_core.h: these entries are exported from the core DLL because
   the UI and the headless API build share lists with them. */
#ifndef ASB_API
#ifdef ASB_BUILDING_DLL
#define ASB_API __declspec(dllexport)
#else
#define ASB_API __declspec(dllimport)
#endif
#endif

#define ASB_MAX_HOST_SHARES 8
#define ASB_SHARE_NAME_MAX  80

typedef struct {
    wchar_t host_path[MAX_PATH];          /* host folder to share */
    wchar_t share_name[ASB_SHARE_NAME_MAX]; /* SMB share name on the host */
    wchar_t drive_letter;                 /* L'Z'.. or 0 = guest picks a free letter */
    wchar_t user[128];                    /* host account the guest authenticates as (may be empty) */
    wchar_t pass_enc[4096];               /* DPAPI blob of the password, hex-encoded (empty = none) */
    BOOL    read_only;                    /* reserved: shares are currently read-write */
    /* Runtime only (never written to the config): the password was inherited
       from the stored list, so it has already been checked against the account
       and must not spend another sign-in attempt. */
    BOOL    skip_account_check;
} AsbHostShare;

typedef struct {
    AsbHostShare items[ASB_MAX_HOST_SHARES];
    int count;
} AsbHostShareList;

/* Validate a share list against a VM. Returns NULL when valid, otherwise a
   user-facing message (static storage). network_mode is the VM's mode: a
   shared drive needs NAT, because the guest reaches the host at the NAT
   gateway. When the account's password is correct but the folder's ACL would
   deny the guest's network logon (a local administrator loses the
   Administrators group over SMB), this grants that account an inheritable
   Modify entry so the mapped drive can be written. */
ASB_API const wchar_t *asb_shares_validate(const AsbHostShareList *list, int network_mode);

/* Fill in share names for entries that don't have one yet. vm_name is used to
   keep names unique across VMs. Safe to call repeatedly. */
ASB_API void asb_shares_name_for_vm(AsbHostShareList *list, const wchar_t *vm_name);

/* Publish every share in the list: create/refresh the SMB share and open
   inbound 445 from the VM's NAT subnet only. Idempotent. Returns S_OK when
   every share is published (failures are logged with ui_log). */
HRESULT asb_shares_publish(const wchar_t *vm_name, const AsbHostShareList *list);

/* Withdraw every share in the list: delete the SMB shares and their firewall
   rules. Idempotent; safe to call for a list whose shares were never created. */
void asb_shares_withdraw(const AsbHostShareList *list);

/* Cleanup after an unclean exit: list the host's published AppSandbox shares so
   the caller can drop the ones no running VM uses, and remove one by name
   (share plus firewall rule). Names are copied into names[][], up to max. */
int  asb_shares_enum_published(wchar_t (*names)[ASB_SHARE_NAME_MAX], int max);
void asb_shares_remove_by_name(const wchar_t *share_name);

/* Encrypt/decrypt a share password with DPAPI, hex-encoded for the config
   file. Both return FALSE on failure (out is then empty). */
ASB_API BOOL asb_share_protect_password(const wchar_t *plain, wchar_t *out, size_t out_chars);
ASB_API BOOL asb_share_unprotect_password(const wchar_t *enc, wchar_t *out, size_t out_chars);

/* Build the guest-side mapping lines for a share:
   "<letter>|<unc>|<user>|<password>", with the password in plain text (the
   channel is the local Hyper-V socket). letter is 0 when the guest should pick
   a free drive letter. Returns TRUE when the line fits. */
BOOL asb_share_guest_line(const AsbHostShare *share, wchar_t *out, size_t out_chars);

#endif /* ASB_SHARES_H */
