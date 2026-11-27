/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-006 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-007 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-008 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-003::REQ-009 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-007::REQ-019 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
#include "vgd_controller.h"
#include <string.h>

void vgd_ctrl_init(VgdControllerMgr *mgr)
{
    if (!mgr) return;
    memset(mgr, 0, sizeof(*mgr));
}

int vgd_ctrl_handle_packet(VgdControllerMgr *mgr, const GamepadPacket *pkt, const VgdBackendOps *ops)
{
    uint8_t slot;
    if (!mgr || !pkt || !ops) return 0;
    if (!asb_gamepad_packet_validate(pkt)) return 0;
    slot = pkt->slot;

    switch (pkt->type) {
    case GAMEPAD_PKT_NAME: {
        uint8_t idx = pkt->large_motor;
        uint8_t total = pkt->small_motor;
        size_t off = (size_t)idx * GAMEPAD_NAME_CHUNK_BYTES;
        uint8_t expected_mask = (uint8_t)((1u << total) - 1u);

        if (idx == 0u || mgr->name_total_chunks[slot] != total) {
            memset(mgr->product_name[slot], 0, sizeof(mgr->product_name[slot]));
            mgr->name_chunks_mask[slot] = 0u;
            mgr->name_total_chunks[slot] = total;
        }
        if (off < GAMEPAD_NAME_MAX_LEN - 1u) {
            size_t avail = (GAMEPAD_NAME_MAX_LEN - 1u) - off;
            size_t copy_len = avail < GAMEPAD_NAME_CHUNK_BYTES ? avail : GAMEPAD_NAME_CHUNK_BYTES;
            memcpy(mgr->product_name[slot] + off, &pkt->state, copy_len);
            mgr->product_name[slot][GAMEPAD_NAME_MAX_LEN - 1u] = '\0';
        }
        mgr->name_chunks_mask[slot] |= (uint8_t)(1u << idx);
        if ((mgr->name_chunks_mask[slot] & expected_mask) == expected_mask) {
            if (ops->set_slot_name) {
                ops->set_slot_name(ops->ctx, slot, mgr->product_name[slot]);
            }
        }
        return 1;
    }

    case GAMEPAD_PKT_PLUG:
        if (mgr->plugged[slot]) return 1;
        if (!ops->plug_slot || !ops->plug_slot(ops->ctx, slot)) return 0;
        mgr->plugged[slot] = 1u;
        memset(&mgr->last_state[slot], 0, sizeof(mgr->last_state[slot]));
        return 1;

    case GAMEPAD_PKT_UNPLUG:
        if (!mgr->plugged[slot]) return 1;
        if (ops->unplug_slot) (void)ops->unplug_slot(ops->ctx, slot);
        mgr->plugged[slot] = 0u;
        memset(&mgr->last_state[slot], 0, sizeof(mgr->last_state[slot]));
        memset(mgr->product_name[slot], 0, sizeof(mgr->product_name[slot]));
        mgr->name_chunks_mask[slot] = 0u;
        mgr->name_total_chunks[slot] = 0u;
        return 1;

    case GAMEPAD_PKT_STATE:
        if (!mgr->plugged[slot]) {
            if (!ops->plug_slot || !ops->plug_slot(ops->ctx, slot)) return 0;
            mgr->plugged[slot] = 1u;
        }
        if (!ops->write_state || !ops->write_state(ops->ctx, slot, &pkt->state)) return 0;
        mgr->last_state[slot] = pkt->state;
        return 1;

    case GAMEPAD_PKT_HELLO:
    case GAMEPAD_PKT_RUMBLE:
        return 1;

    default:
        return 0;
    }
}

void vgd_ctrl_unplug_all(VgdControllerMgr *mgr, const VgdBackendOps *ops)
{
    uint8_t s;
    if (!mgr) return;
    for (s = 0; s < GAMEPAD_MAX_SLOTS; s++) {
        if (mgr->plugged[s]) {
            if (ops && ops->unplug_slot) (void)ops->unplug_slot(ops->ctx, s);
            mgr->plugged[s] = 0u;
            memset(&mgr->last_state[s], 0, sizeof(mgr->last_state[s]));
        }
        memset(mgr->product_name[s], 0, sizeof(mgr->product_name[s]));
        mgr->name_chunks_mask[s] = 0u;
        mgr->name_total_chunks[s] = 0u;
    }
}

int vgd_ctrl_encode_rumble(uint8_t slot, uint8_t large_motor, uint8_t small_motor,
                           uint32_t seq, GamepadPacket *out)
{
    if (!out || slot >= GAMEPAD_MAX_SLOTS) return 0;
    asb_gamepad_make_rumble(slot, large_motor, small_motor, seq, out);
    return 1;
}

#if defined(_WIN32) && !defined(VGD_CTRL_NO_WIN32)
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <sddl.h>
#include <stdio.h>

#ifndef MAX_CLASS_NAME_LEN
#define MAX_CLASS_NAME_LEN 32
#endif

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "advapi32.lib")

static void vgd_log(VgdWinBackend *b, const char *fmt, ...)
{
    if (!b || !b->log_fn) return;
    {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        buf[sizeof(buf) - 1] = '\0';
        b->log_fn("%s", buf);
    }
}

static void ascii_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (src) {
        while (src[i] != '\0' && i + 1u < cap) {
            dst[i] = (wchar_t)(unsigned char)src[i];
            i++;
        }
    }
    dst[i] = L'\0';
}

static size_t ascii_msz_to_wide(const char *src, size_t src_len, wchar_t *dst, size_t dst_cap)
{
    size_t i;
    if (!src || !dst || dst_cap < src_len) return 0;
    for (i = 0; i < src_len; i++) {
        dst[i] = (wchar_t)(unsigned char)src[i];
    }
    return src_len * sizeof(wchar_t);
}

/* Ensure the inbox Windows xinputhid filter driver service exists in SCM so that
 * UpperFilters="xinputhid" never fails PnP device start with Code 19/31 on a fresh VM. */
static void vgd_win_ensure_xinputhid_service(VgdWinBackend *b)
{
    SC_HANDLE scm, svc;
    wchar_t sys_dst[MAX_PATH];

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return;

    svc = OpenServiceW(scm, L"xinputhid", SERVICE_QUERY_STATUS);
    if (svc) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return;
    }

    GetSystemDirectoryW(sys_dst, ARRAYSIZE(sys_dst));
    wcscat_s(sys_dst, ARRAYSIZE(sys_dst), L"\\drivers\\xinputhid.sys");
    if (GetFileAttributesW(sys_dst) == INVALID_FILE_ATTRIBUTES) {
        wchar_t repo_pat[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE hf;
        GetSystemDirectoryW(repo_pat, ARRAYSIZE(repo_pat));
        wcscat_s(repo_pat, ARRAYSIZE(repo_pat), L"\\DriverStore\\FileRepository\\xinputhid.inf_*");
        hf = FindFirstFileW(repo_pat, &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    wchar_t src_sys[MAX_PATH];
                    GetSystemDirectoryW(src_sys, ARRAYSIZE(src_sys));
                    wcscat_s(src_sys, ARRAYSIZE(src_sys), L"\\DriverStore\\FileRepository\\");
                    wcscat_s(src_sys, ARRAYSIZE(src_sys), fd.cFileName);
                    wcscat_s(src_sys, ARRAYSIZE(src_sys), L"\\xinputhid.sys");
                    if (CopyFileW(src_sys, sys_dst, FALSE)) {
                        vgd_log(b, "VGD: staged inbox xinputhid.sys from DriverStore");
                        break;
                    }
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }

    if (GetFileAttributesW(sys_dst) != INVALID_FILE_ATTRIBUTES) {
        svc = CreateServiceW(
            scm, L"xinputhid", L"XINPUT HID Filter Driver",
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL, L"System32\\drivers\\xinputhid.sys",
            L"Base", NULL, NULL, NULL, NULL);
        if (svc) {
            vgd_log(b, "VGD: registered inbox xinputhid kernel service");
            CloseServiceHandle(svc);
        } else {
            vgd_log(b, "VGD: CreateServiceW(xinputhid) failed (err=%lu)", GetLastError());
        }
    } else {
        vgd_log(b, "VGD: warning - inbox xinputhid.sys not found on system");
    }
    CloseServiceHandle(scm);
}

static void vgd_win_remove_devnode_by_hwid(VgdWinBackend *b, const wchar_t *target_hwid)
{
    HDEVINFO devs = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA dev_info;
    DWORD idx;

    if (devs == INVALID_HANDLE_VALUE) return;

    dev_info.cbSize = sizeof(dev_info);
    for (idx = 0; SetupDiEnumDeviceInfo(devs, idx, &dev_info); idx++) {
        wchar_t hwids[512];
        DWORD req = 0, reg_type = 0;
        memset(hwids, 0, sizeof(hwids));
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_HARDWAREID,
                                               &reg_type, (PBYTE)hwids,
                                               sizeof(hwids) - sizeof(wchar_t), &req)) {
            continue;
        }
        {
            const wchar_t *p = hwids;
            while (*p != L'\0') {
                if (_wcsicmp(p, target_hwid) == 0) {
                    if (!SetupDiCallClassInstaller(DIF_REMOVE, devs, &dev_info)) {
                        vgd_log(b, "VGD: DIF_REMOVE failed for %ls (err=%lu)", target_hwid, GetLastError());
                    } else {
                        vgd_log(b, "VGD: removed devnode %ls", target_hwid);
                    }
                    break;
                }
                p += wcslen(p) + 1;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
}

static void vgd_win_write_reg_sz(HKEY root, const wchar_t *subkey, const wchar_t *val_name, const wchar_t *val)
{
    HKEY hkey = NULL;
    if (!subkey || !val_name || !val) return;
    if (RegCreateKeyExW(root, subkey, 0, NULL, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hkey, NULL) == ERROR_SUCCESS) {
        DWORD bytes = (DWORD)((wcslen(val) + 1u) * sizeof(wchar_t));
        (void)RegSetValueExW(hkey, val_name, 0, REG_SZ, (const BYTE *)val, bytes);
        RegCloseKey(hkey);
    }
}

static int wcs_ends_with_ci(const wchar_t *str, const wchar_t *suffix)
{
    size_t slen, suflen;
    if (!str || !suffix) return 0;
    slen = wcslen(str);
    suflen = wcslen(suffix);
    if (slen < suflen) return 0;
    return _wcsicmp(str + (slen - suflen), suffix) == 0;
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
static void vgd_win_apply_oem_name_registry(VgdWinBackend *b, const wchar_t *name_w)
{
    static const wchar_t k_di_subkey[] =
        L"SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\DirectInput\\VID_045E&PID_028E\\OEM";
    static const wchar_t k_joy_subkey[] =
        L"SYSTEM\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick\\OEM\\VID_045E&PID_028E";
    const wchar_t *eff = (name_w && name_w[0] != L'\0')
        ? name_w
        : L"Controller (XBOX 360 For Windows)";
    DWORD idx;

    vgd_win_write_reg_sz(HKEY_LOCAL_MACHINE, k_di_subkey, L"OEM Name", eff);
    vgd_win_write_reg_sz(HKEY_LOCAL_MACHINE, k_joy_subkey, L"OEMName", eff);

    /* Update loaded interactive user hives under HKEY_USERS since appsandbox-gamepad runs as SYSTEM */
    for (idx = 0; ; idx++) {
        wchar_t sid[128];
        DWORD sid_len = ARRAYSIZE(sid);
        LONG rc = RegEnumKeyExW(HKEY_USERS, idx, sid, &sid_len, NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;
        if (wcsncmp(sid, L"S-1-5-21-", 9) != 0) continue;
        if (wcs_ends_with_ci(sid, L"_Classes")) continue;
        {
            wchar_t user_joy[384], user_di[384];
            swprintf_s(user_joy, ARRAYSIZE(user_joy), L"%ls\\%ls", sid, k_joy_subkey);
            swprintf_s(user_di, ARRAYSIZE(user_di), L"%ls\\%ls", sid, k_di_subkey);
            vgd_win_write_reg_sz(HKEY_USERS, user_joy, L"OEMName", eff);
            vgd_win_write_reg_sz(HKEY_USERS, user_di, L"OEM Name", eff);
        }
    }
    vgd_log(b, "VGD: updated VID_045E&PID_028E OEMName/OEM Name to '%ls'", eff);
}

static void vgd_win_set_devnode_friendly_name(VgdWinBackend *b, const GUID *class_guid,
                                              const wchar_t *inst_w, const wchar_t *friendly_w)
{
    HDEVINFO devs;
    SP_DEVINFO_DATA dev_info;
    if (!class_guid || !inst_w || !friendly_w || friendly_w[0] == L'\0') return;
    devs = SetupDiCreateDeviceInfoList(class_guid, NULL);
    if (devs == INVALID_HANDLE_VALUE) return;
    memset(&dev_info, 0, sizeof(dev_info));
    dev_info.cbSize = sizeof(dev_info);
    if (SetupDiOpenDeviceInfoW(devs, inst_w, NULL, 0, &dev_info)) {
        DWORD bytes = (DWORD)((wcslen(friendly_w) + 1u) * sizeof(wchar_t));
        if (!SetupDiSetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_FRIENDLYNAME,
                                               (const BYTE *)friendly_w, bytes)) {
            vgd_log(b, "VGD: Set SPDRP_FRIENDLYNAME failed for %ls (err=%lu)", inst_w, GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-005::REQ-015 */
/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
static int vgd_win_create_devnode(VgdWinBackend *b, uint8_t slot, int is_xusb, const wchar_t *inf_path)
{
    GUID class_guid;
    wchar_t class_name[MAX_CLASS_NAME_LEN];
    HDEVINFO devs = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev_info;
    char inst_a[96], slot_hwid_a[64], hw_msz_a[160];
    wchar_t inst_w[96], slot_hwid_w[64], hw_msz_w[160], compat_msz_w[256];
    size_t hw_len = 0, compat_len = 0, hw_bytes = 0, compat_bytes = 0;
    const char *compat_a;
    const wchar_t *custom_name_w = (b->slots[slot].product_name_w[0] != L'\0')
        ? b->slots[slot].product_name_w
        : NULL;
    const wchar_t *desc_w = custom_name_w
        ? custom_name_w
        : (is_xusb ? L"App Sandbox Virtual Xbox 360 Controller (XUSB)"
                   : L"Controller (XBOX 360 For Windows)");
    BOOL reboot_req = FALSE;
    DEVINST dev_inst = 0;
    ULONG dev_status = 0, dev_problem = 0;

    if (GetFileAttributesW(inf_path) == INVALID_FILE_ATTRIBUTES) {
        vgd_log(b, "VGD: INF missing: %ls", inf_path);
        return 0;
    }

    if (is_xusb) {
        if (vgd_format_pnp_instance_ids(slot, inst_a, sizeof(inst_a), NULL, 0) != 0 ||
            vgd_format_hwids(slot, slot_hwid_a, sizeof(slot_hwid_a), NULL, 0) != 0)
            return 0;
    } else {
        if (vgd_format_pnp_instance_ids(slot, NULL, 0, inst_a, sizeof(inst_a)) != 0 ||
            vgd_format_hwids(slot, NULL, 0, slot_hwid_a, sizeof(slot_hwid_a)) != 0)
            return 0;
    }
    if (vgd_format_pnp_hwids_msz(slot, is_xusb, hw_msz_a, sizeof(hw_msz_a), &hw_len) != 0)
        return 0;
    compat_a = vgd_get_compat_ids_msz(is_xusb, &compat_len);

    ascii_to_wide(inst_a, inst_w, ARRAYSIZE(inst_w));
    ascii_to_wide(slot_hwid_a, slot_hwid_w, ARRAYSIZE(slot_hwid_w));
    hw_bytes = ascii_msz_to_wide(hw_msz_a, hw_len, hw_msz_w, ARRAYSIZE(hw_msz_w));
    compat_bytes = ascii_msz_to_wide(compat_a, compat_len, compat_msz_w, ARRAYSIZE(compat_msz_w));
    if (hw_bytes == 0 || compat_bytes == 0) return 0;

    /* Clean any stale devnode with the same slot Hardware ID before creating a fresh one */
    vgd_win_remove_devnode_by_hwid(b, slot_hwid_w);

    memset(&class_guid, 0, sizeof(class_guid));
    memset(class_name, 0, sizeof(class_name));
    if (!SetupDiGetINFClassW(inf_path, &class_guid, class_name, MAX_CLASS_NAME_LEN, NULL)) {
        vgd_log(b, "VGD: SetupDiGetINFClassW failed for %ls (err=%lu)", inf_path, GetLastError());
        return 0;
    }

    devs = SetupDiCreateDeviceInfoList(&class_guid, NULL);
    if (devs == INVALID_HANDLE_VALUE) {
        vgd_log(b, "VGD: SetupDiCreateDeviceInfoList failed (err=%lu)", GetLastError());
        return 0;
    }

    memset(&dev_info, 0, sizeof(dev_info));
    dev_info.cbSize = sizeof(dev_info);
    /* Explicit PnP Instance ID: ROOT\VID_045E&PID_028E&IG_00\ASB_SLOT_<N> (or XI_00) */
    if (!SetupDiCreateDeviceInfoW(devs, inst_w, &class_guid, desc_w, NULL, 0, &dev_info)) {
        DWORD err = GetLastError();
        if (err == ERROR_DEVINST_ALREADY_EXISTS) {
            if (SetupDiOpenDeviceInfoW(devs, inst_w, NULL, 0, &dev_info)) {
                SetupDiCallClassInstaller(DIF_REMOVE, devs, &dev_info);
            }
            memset(&dev_info, 0, sizeof(dev_info));
            dev_info.cbSize = sizeof(dev_info);
            if (!SetupDiCreateDeviceInfoW(devs, inst_w, &class_guid, desc_w, NULL, 0, &dev_info)) {
                vgd_log(b, "VGD: SetupDiCreateDeviceInfoW(%ls) retry failed (err=%lu)", inst_w, GetLastError());
                SetupDiDestroyDeviceInfoList(devs);
                return 0;
            }
        } else {
            vgd_log(b, "VGD: SetupDiCreateDeviceInfoW(%ls) failed (err=%lu)", inst_w, err);
            SetupDiDestroyDeviceInfoList(devs);
            return 0;
        }
    }

    if (!SetupDiSetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_HARDWAREID,
                                           (const BYTE *)hw_msz_w, (DWORD)hw_bytes)) {
        vgd_log(b, "VGD: Set SPDRP_HARDWAREID failed for %ls (err=%lu)", inst_w, GetLastError());
        SetupDiDestroyDeviceInfoList(devs);
        return 0;
    }

    if (!SetupDiSetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_COMPATIBLEIDS,
                                           (const BYTE *)compat_msz_w, (DWORD)compat_bytes)) {
        vgd_log(b, "VGD: Set SPDRP_COMPATIBLEIDS failed for %ls (err=%lu)", inst_w, GetLastError());
    }

    if (custom_name_w) {
        DWORD fname_bytes = (DWORD)((wcslen(custom_name_w) + 1u) * sizeof(wchar_t));
        (void)SetupDiSetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_FRIENDLYNAME,
                                                (const BYTE *)custom_name_w, fname_bytes);
    }

    /* Pre-registration xinputhid UpperFilter tripwire on HID parent so WGI routes the
     * controller exclusively via AppSandboxVGD_XUSB while joy.cpl still sees HID\VID_045E&PID_028E&IG_00. */
    if (!is_xusb) {
        static const wchar_t k_uf[] = L"xinputhid\0";
        if (!SetupDiSetDeviceRegistryPropertyW(devs, &dev_info, SPDRP_UPPERFILTERS,
                                               (const BYTE *)k_uf, (DWORD)sizeof(k_uf))) {
            vgd_log(b, "VGD: Set SPDRP_UPPERFILTERS(xinputhid) failed for %ls (err=%lu)", inst_w, GetLastError());
        }
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devs, &dev_info)) {
        vgd_log(b, "VGD: DIF_REGISTERDEVICE failed for %ls (err=%lu)", inst_w, GetLastError());
        SetupDiDestroyDeviceInfoList(devs);
        return 0;
    }

    SetupDiDestroyDeviceInfoList(devs);

    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, slot_hwid_w, inf_path, INSTALLFLAG_FORCE, &reboot_req)) {
        DWORD err = GetLastError();
        vgd_log(b, "VGD: UpdateDriverForPlugAndPlayDevicesW failed for %ls (err=0x%08lX)", slot_hwid_w, err);
        vgd_win_remove_devnode_by_hwid(b, slot_hwid_w);
        return 0;
    }

    if (custom_name_w) {
        vgd_win_set_devnode_friendly_name(b, &class_guid, inst_w, custom_name_w);
    }

    if (CM_Locate_DevNodeW(&dev_inst, inst_w, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
        CM_Get_DevNode_Status(&dev_status, &dev_problem, dev_inst, 0) == CR_SUCCESS) {
        vgd_log(b, "VGD: created & bound %ls (reboot=%d, status=0x%lX, problem=%lu)",
                inst_w, reboot_req ? 1 : 0, dev_status, dev_problem);
    } else {
        vgd_log(b, "VGD: created & bound %ls (reboot=%d)", inst_w, reboot_req ? 1 : 0);
    }
    return 1;
}

static int vgd_win_create_slot_ipc(VgdWinBackend *b, uint8_t slot)
{
    VgdWinSlot *s = &b->slots[slot];
    wchar_t shm_name[64], out_name[64], xusb_in_name[64], hid_in_name[64];
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;

    /* Grant SYSTEM, Admins, LOCAL SERVICE (WUDFHost), Network Service, and Everyone full access */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;WD)",
            SDDL_REVISION_1, &sd, NULL)) {
        vgd_log(b, "VGD: SDDL parse failed (err=%lu)", GetLastError());
        return 0;
    }

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    vgd_shm_format_names_w(slot, (uint16_t *)shm_name, 64, NULL, 0, (uint16_t *)out_name, 64);
    vgd_shm_format_doorbells_w(slot, (uint16_t *)xusb_in_name, 64, (uint16_t *)hid_in_name, 64);

    s->shm_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                       0, sizeof(VgdShmBlock), shm_name);
    if (!s->shm_handle) {
        vgd_log(b, "VGD: CreateFileMappingW(%ls) failed (err=%lu)", shm_name, GetLastError());
        LocalFree(sd);
        return 0;
    }

    s->shm = (VgdShmBlock *)MapViewOfFile(s->shm_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VgdShmBlock));
    if (!s->shm) {
        vgd_log(b, "VGD: MapViewOfFile(%ls) failed (err=%lu)", shm_name, GetLastError());
        CloseHandle(s->shm_handle);
        s->shm_handle = NULL;
        LocalFree(sd);
        return 0;
    }

    vgd_shm_init(s->shm, slot);
    if (s->product_name_utf8[0] != '\0') {
        (void)vgd_shm_set_product_name_utf8(s->shm, s->product_name_utf8);
    }

    s->xusb_in_evt = CreateEventW(&sa, FALSE, FALSE, xusb_in_name);
    s->hid_in_evt  = CreateEventW(&sa, FALSE, FALSE, hid_in_name);
    s->out_evt     = CreateEventW(&sa, FALSE, FALSE, out_name);
    LocalFree(sd);

    if (!s->xusb_in_evt || !s->hid_in_evt || !s->out_evt) {
        vgd_log(b, "VGD: CreateEventW failed for slot %u (err=%lu)", slot, GetLastError());
        if (s->xusb_in_evt) { CloseHandle(s->xusb_in_evt); s->xusb_in_evt = NULL; }
        if (s->hid_in_evt)  { CloseHandle(s->hid_in_evt);  s->hid_in_evt  = NULL; }
        if (s->out_evt)     { CloseHandle(s->out_evt);     s->out_evt     = NULL; }
        UnmapViewOfFile(s->shm); s->shm = NULL;
        CloseHandle(s->shm_handle); s->shm_handle = NULL;
        return 0;
    }

    s->last_rumble_gen = 0;
    s->active = 1;
    return 1;
}

static void vgd_win_close_slot_ipc(VgdWinBackend *b, uint8_t slot)
{
    VgdWinSlot *s = &b->slots[slot];
    s->active = 0;
    if (s->shm) {
        s->shm->connected = 0u;
        UnmapViewOfFile(s->shm);
        s->shm = NULL;
    }
    if (s->shm_handle) {
        CloseHandle(s->shm_handle);
        s->shm_handle = NULL;
    }
    if (s->xusb_in_evt) {
        CloseHandle(s->xusb_in_evt);
        s->xusb_in_evt = NULL;
    }
    if (s->hid_in_evt) {
        CloseHandle(s->hid_in_evt);
        s->hid_in_evt = NULL;
    }
    if (s->out_evt) {
        CloseHandle(s->out_evt);
        s->out_evt = NULL;
    }
}

int vgd_win_backend_init(VgdWinBackend *b, const wchar_t *drivers_dir, VgdLogFn log_fn)
{
    if (!b || !drivers_dir) return 0;
    memset(b, 0, sizeof(*b));
    b->log_fn = log_fn;
    swprintf_s(b->xusb_inf_path, MAX_PATH, L"%s\\AppSandboxVGD_XUSB.inf", drivers_dir);
    swprintf_s(b->hid_inf_path, MAX_PATH, L"%s\\AppSandboxVGD_HID.inf", drivers_dir);
    return 1;
}

void vgd_win_backend_cleanup_stale(VgdWinBackend *b)
{
    uint32_t slot;
    if (!b) return;
    for (slot = 0; slot < GAMEPAD_MAX_SLOTS; slot++) {
        char xa[64], ha[64];
        wchar_t xw[64], hw[64];
        if (vgd_format_hwids(slot, xa, sizeof(xa), ha, sizeof(ha)) == 0) {
            ascii_to_wide(xa, xw, ARRAYSIZE(xw));
            ascii_to_wide(ha, hw, ARRAYSIZE(hw));
            vgd_win_remove_devnode_by_hwid(b, xw);
            vgd_win_remove_devnode_by_hwid(b, hw);
        }
    }
}

/* @implements spec-ft-gamepad-umdf2-driver::TASK-GP-VGD-010::REQ-024 */
void vgd_win_set_slot_name(void *ctx, uint8_t slot, const char *utf8_name)
{
    VgdWinBackend *b = (VgdWinBackend *)ctx;
    VgdWinSlot *s;
    if (!b || slot >= GAMEPAD_MAX_SLOTS) return;
    s = &b->slots[slot];
    memset(s->product_name_utf8, 0, sizeof(s->product_name_utf8));
    memset(s->product_name_w, 0, sizeof(s->product_name_w));
    if (utf8_name && utf8_name[0] != '\0') {
        size_t len = strlen(utf8_name);
        if (len >= sizeof(s->product_name_utf8)) len = sizeof(s->product_name_utf8) - 1u;
        memcpy(s->product_name_utf8, utf8_name, len);
        s->product_name_utf8[len] = '\0';
        (void)MultiByteToWideChar(CP_UTF8, 0, s->product_name_utf8, -1,
                                  s->product_name_w, ARRAYSIZE(s->product_name_w));
        s->product_name_w[ARRAYSIZE(s->product_name_w) - 1u] = L'\0';
        vgd_log(b, "VGD: slot %u product name set to '%s'", slot, s->product_name_utf8);
    }
    if (s->shm) {
        (void)vgd_shm_set_product_name_utf8(s->shm, s->product_name_utf8);
    }
    if (s->active && s->product_name_w[0] != L'\0') {
        vgd_win_apply_oem_name_registry(b, s->product_name_w);
    }
}

int vgd_win_plug_slot(void *ctx, uint8_t slot)
{
    VgdWinBackend *b = (VgdWinBackend *)ctx;
    int xusb_ok, hid_ok;

    if (!b || slot >= GAMEPAD_MAX_SLOTS) return 0;
    vgd_win_ensure_xinputhid_service(b);
    vgd_win_apply_oem_name_registry(b, b->slots[slot].product_name_w);
    if (!vgd_win_create_slot_ipc(b, slot)) return 0;

    xusb_ok = vgd_win_create_devnode(b, slot, 1, b->xusb_inf_path);
    hid_ok  = vgd_win_create_devnode(b, slot, 0, b->hid_inf_path);
    if (!xusb_ok && !hid_ok) {
        vgd_log(b, "VGD: slot %u failed to create both XUSB and HID devnodes", slot);
        vgd_win_close_slot_ipc(b, slot);
        return 0;
    }
    vgd_log(b, "VGD: slot %u plugged (xusb=%d, hid=%d)", slot, xusb_ok, hid_ok);
    return 1;
}

int vgd_win_unplug_slot(void *ctx, uint8_t slot)
{
    VgdWinBackend *b = (VgdWinBackend *)ctx;
    char xa[64], ha[64];
    wchar_t xw[64], hw[64];

    if (!b || slot >= GAMEPAD_MAX_SLOTS) return 0;
    if (vgd_format_hwids(slot, xa, sizeof(xa), ha, sizeof(ha)) == 0) {
        ascii_to_wide(xa, xw, ARRAYSIZE(xw));
        ascii_to_wide(ha, hw, ARRAYSIZE(hw));
        vgd_win_remove_devnode_by_hwid(b, xw);
        vgd_win_remove_devnode_by_hwid(b, hw);
    }
    vgd_win_close_slot_ipc(b, slot);
    memset(b->slots[slot].product_name_utf8, 0, sizeof(b->slots[slot].product_name_utf8));
    memset(b->slots[slot].product_name_w, 0, sizeof(b->slots[slot].product_name_w));
    vgd_log(b, "VGD: slot %u unplugged", slot);
    return 1;
}

int vgd_win_write_state(void *ctx, uint8_t slot, const GamepadState *state)
{
    VgdWinBackend *b = (VgdWinBackend *)ctx;
    VgdWinSlot *s;
    if (!b || !state || slot >= GAMEPAD_MAX_SLOTS) return 0;
    s = &b->slots[slot];
    if (!s->active || !s->shm) return 0;
    if (vgd_shm_write_state(s->shm, state) != 0) return 0;
    if (s->xusb_in_evt) SetEvent(s->xusb_in_evt);
    if (s->hid_in_evt)  SetEvent(s->hid_in_evt);
    return 1;
}

int vgd_win_wait_rumble(VgdWinBackend *b, uint8_t slot, HANDLE stop_event,
                        uint8_t *out_large, uint8_t *out_small)
{
    VgdWinSlot *s;
    HANDLE handles[2];
    if (!b || !out_large || !out_small || slot >= GAMEPAD_MAX_SLOTS) return -1;
    s = &b->slots[slot];
    if (!s->active || !s->shm || !s->out_evt) return 0;

    handles[0] = stop_event;
    handles[1] = s->out_evt;

    for (;;) {
        DWORD wr = WaitForMultipleObjects(2, handles, FALSE, 500);
        if (wr == WAIT_OBJECT_0 || !s->active || !s->shm) {
            return 0;
        }
        if (wr == WAIT_OBJECT_0 + 1 || wr == WAIT_TIMEOUT) {
            uint8_t lm = 0, sm = 0;
            uint32_t gen = 0;
            if (vgd_shm_read_rumble(s->shm, &lm, &sm, &gen) == 0 && gen != s->last_rumble_gen) {
                s->last_rumble_gen = gen;
                *out_large = lm;
                *out_small = sm;
                return 1;
            }
        } else {
            return -1;
        }
    }
}
#endif
