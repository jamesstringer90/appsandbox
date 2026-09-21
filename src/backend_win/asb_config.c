/* asb_config.c - the vms.cfg parse and save loops.
 *
 * Extracted from asb_core.c with the baseline's reader and writer
 * intact: the parse loop and the save loop are the verbatim moves
 * (the fwprintf calls themselves are byte-identical), with the
 * declared deviations being the explicit caps, the IN/OUT counts, the
 * id-free interface (the core wrapper assigns unique_id), and ONE
 * ferror() probe per loop folding the whole-stream BOOL. The new
 * InternalSwitch key's dispatch arm and save branch live here too -
 * the same TU that owns every other key's parsing.
 *
 * A TU that never includes an HCN header: it compiles into both
 * AppSandboxCore.dll and the offline test binary. No logging, no HCS,
 * no VMMS - the wrapper owns every side effect beyond the stream. */

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>
#include "asb_config.h"

/* Per-block InternalSwitch parse state. The LAST occurrence in the
   block wins, matching the parser's sequential assignment for every
   other key: a later valid line recovers a block an earlier invalid
   line marked, and a later invalid line poisons a valid one. An
   explicit empty value ("InternalSwitch=") is VALID and selects Auto -
   it is the recovery path for a stale InternalSwitchInvalid marker. */
typedef enum {
    BLK_SW_ABSENT = 0,   /* no InternalSwitch line seen in this block */
    BLK_SW_VALID,
    BLK_SW_INVALID
} BlockSwState;

typedef struct {
    BlockSwState state;
    wchar_t value[INTERNAL_SWITCH_CAP];  /* live only when state == BLK_SW_VALID */
} BlockSwParse;

/* Merge the block's final InternalSwitch parse state into the row:
   VALID stores the value and clears the flag; INVALID empties the
   selector and sets the flag; ABSENT empties the selector and leaves
   the persisted InternalSwitchInvalid marker standing (a stale marker
   with no value means the row stays INVALID - start is refused until
   re-selected). Invariant: invalid == 1 implies selector == "". */
static void finish_vm_block(VmInstance *vm, BlockSwParse *sw)
{
    if (!vm) {
        sw->state = BLK_SW_ABSENT;
        sw->value[0] = L'\0';
        return;
    }
    switch (sw->state) {
    case BLK_SW_VALID:
        wcscpy_s(vm->internal_switch, INTERNAL_SWITCH_CAP, sw->value);
        vm->internal_switch_invalid = FALSE;
        break;
    case BLK_SW_INVALID:
        vm->internal_switch[0] = L'\0';
        vm->internal_switch_invalid = TRUE;
        break;
    case BLK_SW_ABSENT:
    default:
        vm->internal_switch[0] = L'\0';
        break;
    }
    sw->state = BLK_SW_ABSENT;
    sw->value[0] = L'\0';
}

FILE *asb_config_open_read(const wchar_t *path)
{
    FILE *f;
    unsigned char bom[3] = { 0 };
    BOOL unicode_config;

    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return NULL;
    (void)fread(bom, 1, sizeof(bom), f);
    fclose(f);
    unicode_config = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) ||
                     (bom[0] == 0xFF && bom[1] == 0xFE);
    if (_wfopen_s(&f, path, unicode_config ? L"r,ccs=UTF-8" : L"r") != 0 || !f) return NULL;
    return f;
}

FILE *asb_config_open_write(const wchar_t *path)
{
    FILE *f;

    if (_wfopen_s(&f, path, L"w,ccs=UTF-8") != 0 || !f) return NULL;
    return f;
}

BOOL asb_load_vm_list_stream(FILE *f, VmInstance *vms, int vm_cap,
                             int *vm_count, TemplateInfo *templates,
                             int template_cap, int *template_count,
                             wchar_t *last_iso_path, size_t last_iso_cap,
                             BOOL *suppress_tray_warn)
{
    wchar_t line[1024];
    VmInstance *vm = NULL;
    TemplateInfo *tpl = NULL;
    BOOL in_settings = FALSE;
    BlockSwParse sw = { 0 };
    BOOL ok;

    while (fgetws(line, 1024, f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len-1] == L'\n' || line[len-1] == L'\r'))
            line[--len] = L'\0';

        if (wcscmp(line, L"[Settings]") == 0) {
            finish_vm_block(vm, &sw);
            in_settings = TRUE;
            vm = NULL;
            tpl = NULL;
            continue;
        }

        if (wcscmp(line, L"[VM]") == 0) {
            finish_vm_block(vm, &sw);
            in_settings = FALSE;
            tpl = NULL;
            if (*vm_count >= vm_cap) {
                /* The current VM was just finalized above. Do not let the
                   common EOF path finalize it a second time: ABSENT would
                   clear a valid InternalSwitch on the last accepted row. */
                vm = NULL;
                break;
            }
            vm = &vms[*vm_count];
            ZeroMemory(vm, sizeof(VmInstance));
            (*vm_count)++;
            continue;
        }

        if (wcscmp(line, L"[Template]") == 0) {
            finish_vm_block(vm, &sw);
            in_settings = FALSE;
            vm = NULL;
            tpl = NULL;
            if (*template_count < template_cap) {
                tpl = &templates[(*template_count)++];
                ZeroMemory(tpl, sizeof(*tpl));
            }
            continue;
        }

        if (tpl) {
            if (wcsncmp(line, L"Name=", 5) == 0)
                wcsncpy_s(tpl->name, 256, line + 5, _TRUNCATE);
            else if (wcsncmp(line, L"OsType=", 7) == 0)
                wcsncpy_s(tpl->os_type, 32, line + 7, _TRUNCATE);
            else if (wcsncmp(line, L"ImagePath=", 10) == 0)
                wcsncpy_s(tpl->image_path, MAX_PATH, line + 10, _TRUNCATE);
            else if (wcsncmp(line, L"VhdxPath=", 9) == 0)
                wcsncpy_s(tpl->vhdx_path, MAX_PATH, line + 9, _TRUNCATE);
            continue;
        }

        if (in_settings) {
            if (wcsncmp(line, L"LastIsoPath=", 12) == 0)
                wcscpy_s(last_iso_path, last_iso_cap, line + 12);
            else if (wcsncmp(line, L"SuppressTrayWarn=", 17) == 0)
                *suppress_tray_warn = (_wtoi(line + 17) != 0);
            continue;
        }

        if (!vm) continue;

        if (wcsncmp(line, L"Name=", 5) == 0)
            wcscpy_s(vm->name, 256, line + 5);
        else if (wcsncmp(line, L"OsType=", 7) == 0)
            wcscpy_s(vm->os_type, 32, line + 7);
        else if (wcsncmp(line, L"ImagePath=", 10) == 0)
            wcscpy_s(vm->image_path, MAX_PATH, line + 10);
        else if (wcsncmp(line, L"VhdxPath=", 9) == 0)
            wcscpy_s(vm->vhdx_path, MAX_PATH, line + 9);
        else if (wcsncmp(line, L"RamMB=", 6) == 0)
            vm->ram_mb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"HddGB=", 6) == 0)
            vm->hdd_gb = (DWORD)_wtoi(line + 6);
        else if (wcsncmp(line, L"CpuCores=", 9) == 0)
            vm->cpu_cores = (DWORD)_wtoi(line + 9);
        else if (wcsncmp(line, L"GpuMode=", 8) == 0)
            vm->gpu_mode = _wtoi(line + 8);
        else if (wcsncmp(line, L"GpuName=", 8) == 0)
            wcscpy_s(vm->gpu_name, 256, line + 8);
        else if (wcsncmp(line, L"GpuId=", 6) == 0)
            wcsncpy_s(vm->gpu_id, ARRAYSIZE(vm->gpu_id), line + 6, _TRUNCATE);
        else if (wcsncmp(line, L"GpuDevicePath=", 14) == 0)
            { /* ignored - backwards compat */ }
        else if (wcsncmp(line, L"NetworkMode=", 12) == 0)
            vm->network_mode = _wtoi(line + 12);
        else if (wcsncmp(line, L"InternalSwitch=", 15) == 0) {
            /* Decided on the wcslen view the baseline reader delivers.
               An over-long value marks the row INVALID - the value is
               never copied into the row (wcscpy_s aborts the process on
               overflow, and a hand-edited over-long value must not kill
               the loader); a rejected value stores selector="" with the
               flag set, which start refuses until re-selected. */
            const wchar_t *val = line + 15;
            size_t vlen = wcslen(val);
            if (asb_internal_switch_value_valid(val, vlen)) {
                sw.state = BLK_SW_VALID;
                wcsncpy_s(sw.value, INTERNAL_SWITCH_CAP, val, _TRUNCATE);
            } else {
                sw.state = BLK_SW_INVALID;
            }
        }
        else if (wcsncmp(line, L"InternalSwitchInvalid=", 22) == 0)
            vm->internal_switch_invalid = (_wtoi(line + 22) != 0);
        else if (wcsncmp(line, L"NetAdapter=", 11) == 0)
            wcscpy_s(vm->net_adapter, 256, line + 11);
        else if (wcsncmp(line, L"MacAddress=", 11) == 0)
            wcsncpy_s(vm->mac_address, ARRAYSIZE(vm->mac_address), line + 11, _TRUNCATE);
        else if (wcsncmp(line, L"ResourcesIso=", 13) == 0)
            wcscpy_s(vm->resources_iso_path, MAX_PATH, line + 13);
        else if (wcsncmp(line, L"NatIp=", 6) == 0)
            WideCharToMultiByte(CP_UTF8, 0, line + 6, -1, vm->nat_ip, sizeof(vm->nat_ip), NULL, NULL);
        else if (wcsncmp(line, L"IsTemplate=", 11) == 0)
            vm->is_template = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"TestMode=", 9) == 0)
            vm->test_mode = (_wtoi(line + 9) != 0);
        else if (wcsncmp(line, L"AdminUser=", 10) == 0)
            wcscpy_s(vm->admin_user, 128, line + 10);
        else if (wcsncmp(line, L"SshEnabled=", 11) == 0)
            vm->ssh_enabled = (_wtoi(line + 11) != 0);
        else if (wcsncmp(line, L"SshPort=", 8) == 0)
            vm->ssh_port = (DWORD)_wtoi(line + 8);
        else if (wcsncmp(line, L"SshDeployKey=", 13) == 0)
            vm->ssh_deploy_key = (_wtoi(line + 13) != 0);
        else if (wcsncmp(line, L"SshPubKey=", 10) == 0)
            /* Truncating copy: wcscpy_s ABORTS the process on overflow, and this
               line comes from an editable config file. Generated ed25519 lines
               are ~100 chars; anything longer is corrupt -- truncate, don't die. */
            wcsncpy_s(vm->ssh_pubkey, 512, line + 10, _TRUNCATE);
        else if (wcsncmp(line, L"InstallComplete=", 16) == 0)
            vm->install_complete = (_wtoi(line + 16) != 0);
    }

    finish_vm_block(vm, &sw);
    ok = !ferror(f);
    return ok;
}

BOOL asb_save_vm_list_stream(FILE *f, const VmInstance *vms, int vm_count,
                             const TemplateInfo *templates, int template_count,
                             const wchar_t *last_iso_path,
                             BOOL suppress_tray_warn)
{
    int i;
    BOOL ok;

    if (last_iso_path[0] != L'\0' || suppress_tray_warn) {
        fwprintf(f, L"[Settings]\n");
        if (last_iso_path[0] != L'\0')
            fwprintf(f, L"LastIsoPath=%s\n", last_iso_path);
        if (suppress_tray_warn)
            fwprintf(f, L"SuppressTrayWarn=1\n");
        fwprintf(f, L"\n");
    }

    for (i = 0; i < template_count; i++) {
        fwprintf(f, L"[Template]\n");
        fwprintf(f, L"Name=%s\n", templates[i].name);
        fwprintf(f, L"OsType=%s\n", templates[i].os_type);
        fwprintf(f, L"ImagePath=%s\n", templates[i].image_path);
        fwprintf(f, L"VhdxPath=%s\n\n", templates[i].vhdx_path);
    }

    for (i = 0; i < vm_count; i++) {
        if (vms[i].building_vhdx) continue;
        fwprintf(f, L"[VM]\n");
        fwprintf(f, L"Name=%s\n", vms[i].name);
        fwprintf(f, L"OsType=%s\n", vms[i].os_type);
        fwprintf(f, L"ImagePath=%s\n", vms[i].image_path);
        fwprintf(f, L"VhdxPath=%s\n", vms[i].vhdx_path);
        fwprintf(f, L"RamMB=%lu\n", vms[i].ram_mb);
        fwprintf(f, L"HddGB=%lu\n", vms[i].hdd_gb);
        fwprintf(f, L"CpuCores=%lu\n", vms[i].cpu_cores);
        fwprintf(f, L"GpuMode=%d\n", vms[i].gpu_mode);
        fwprintf(f, L"GpuName=%s\n", vms[i].gpu_name);
        if (vms[i].gpu_id[0])
            fwprintf(f, L"GpuId=%s\n", vms[i].gpu_id);
        fwprintf(f, L"NetworkMode=%d\n", vms[i].network_mode);
        /* The InternalSwitch pair sits immediately after NetworkMode=:
           an INTEGER key is unconditional and corruption-proof, so the
           new string key never rides behind an unvalidated string key
           (a NetAdapter value carrying U+FFFF truncates its own line
           and everything after it on save - measured). */
        if (vms[i].internal_switch[0] != L'\0')
            fwprintf(f, L"InternalSwitch=%s\n", vms[i].internal_switch);
        if (vms[i].internal_switch_invalid)
            fwprintf(f, L"InternalSwitchInvalid=1\n");
        if (vms[i].net_adapter[0] != L'\0')
            fwprintf(f, L"NetAdapter=%s\n", vms[i].net_adapter);
        if (vms[i].mac_address[0])
            fwprintf(f, L"MacAddress=%s\n", vms[i].mac_address);
        if (vms[i].resources_iso_path[0] != L'\0')
            fwprintf(f, L"ResourcesIso=%s\n", vms[i].resources_iso_path);
        if (vms[i].nat_ip[0] != '\0')
            fwprintf(f, L"NatIp=%S\n", vms[i].nat_ip);
        if (vms[i].is_template)
            fwprintf(f, L"IsTemplate=1\n");
        if (vms[i].test_mode)
            fwprintf(f, L"TestMode=1\n");
        if (vms[i].admin_user[0])
            fwprintf(f, L"AdminUser=%s\n", vms[i].admin_user);
        if (vms[i].ssh_enabled)
            fwprintf(f, L"SshEnabled=1\n");
        if (vms[i].ssh_port)
            fwprintf(f, L"SshPort=%lu\n", vms[i].ssh_port);
        if (vms[i].ssh_deploy_key)
            fwprintf(f, L"SshDeployKey=1\n");
        if (vms[i].ssh_pubkey[0])
            fwprintf(f, L"SshPubKey=%s\n", vms[i].ssh_pubkey);
        if (vms[i].install_complete)
            fwprintf(f, L"InstallComplete=1\n");
        fwprintf(f, L"\n");
    }

    ok = !ferror(f);
    return ok;
}

BOOL asb_internal_switch_value_valid(const wchar_t *name, size_t len)
{
    return (len < INTERNAL_SWITCH_CAP &&
            asb_internal_switch_value_chars_ok(name, len)) ? TRUE : FALSE;
}
