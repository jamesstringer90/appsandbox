/*
 * backend_win.c -- Windows implementation of the core BackendVtbl.
 *
 * Each vtable function converts its UTF-8 string arguments to UTF-16
 * and delegates to the existing wchar_t-based asb_vm_* API in asb_core.h.
 * This keeps the existing Windows code completely unchanged while still
 * exposing a platform-neutral surface to the core.
 */

#include "backend.h"
#include "asb_core.h"
#include "hcs_vm.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

/* ---- String conversion helpers ---- */

static int utf8_to_wide(const char *utf8, wchar_t *out, int out_chars)
{
    if (!utf8 || !out || out_chars <= 0) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars);
    return n > 0 ? 1 : 0;
}

static void wide_to_utf8(const wchar_t *w, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)out_sz, NULL, NULL);
    out[out_sz - 1] = '\0';
}

/* ---- Event callback plumbing ---- */

static CoreVmEventCallback g_event_cb;
static void           *g_event_user_data;

static void post_event_vm(int type, AsbVm vm, int int_value, const wchar_t *str_value)
{
    if (!g_event_cb) return;
    CoreVmEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    evt.int_value = int_value;
    if (vm) {
        const wchar_t *n = asb_vm_name(vm);
        if (n) wide_to_utf8(n, evt.vm_name, sizeof(evt.vm_name));
    }
    char str_buf[512] = { 0 };
    if (str_value) {
        wide_to_utf8(str_value, str_buf, sizeof(str_buf));
        evt.str_value = str_buf;
    }
    g_event_cb(&evt, g_event_user_data);
}

static void on_state_change(AsbVm vm, BOOL running, void *user_data)
{
    (void)user_data;
    post_event_vm(CORE_VM_EVENT_STATE_CHANGED, vm, running ? 1 : 0, NULL);
}

static void on_progress(AsbVm vm, int pct, BOOL staging, void *user_data)
{
    (void)user_data;
    post_event_vm(CORE_VM_EVENT_PROGRESS, vm, pct, staging ? L"Staging files" : L"Creating disk");
}

static void on_alert(const wchar_t *message, void *user_data)
{
    (void)user_data;
    post_event_vm(CORE_VM_EVENT_ALERT, NULL, 0, message);
}

static void on_log(const wchar_t *message, void *user_data)
{
    (void)user_data;
    post_event_vm(CORE_VM_EVENT_LOG, NULL, 0, message);
}

static void on_vm_removed(int index, void *user_data)
{
    (void)index; (void)user_data;
    post_event_vm(CORE_VM_EVENT_LIST_CHANGED, NULL, 0, NULL);
}

/* ---- Vtable entries ---- */

static int backend_win_init(void)
{
    HRESULT hr = asb_init();
    if (FAILED(hr)) return BACKEND_ERR_FAILED;
    asb_set_state_callback(on_state_change, NULL);
    asb_set_progress_callback(on_progress, NULL);
    asb_set_alert_callback(on_alert, NULL);
    asb_set_log_callback(on_log, NULL);
    asb_set_vm_removed_callback(on_vm_removed, NULL);
    return BACKEND_OK;
}

static void backend_win_cleanup(void)
{
    asb_cleanup();
    g_event_cb = NULL;
    g_event_user_data = NULL;
}

static int backend_win_create_vm(const CoreVmConfig *cfg, char *err, size_t err_sz)
{
    if (!cfg || !cfg->name || !cfg->os_type) {
        if (err && err_sz) strncpy_s(err, err_sz, "missing name/os_type", _TRUNCATE);
        return BACKEND_ERR_INVALID_ARG;
    }

    wchar_t wname[256], wos[64], wimg[MAX_PATH], wtpl[128];
    wchar_t wnet[128], wuser[128], wpass[128];

    if (!utf8_to_wide(cfg->name, wname, 256)) return BACKEND_ERR_INVALID_ARG;
    utf8_to_wide(cfg->os_type, wos, 64);

    AsbVmConfig ac;
    memset(&ac, 0, sizeof(ac));
    ac.name = wname;
    ac.os_type = wos;

    if (cfg->image_path && utf8_to_wide(cfg->image_path, wimg, MAX_PATH))
        ac.image_path = wimg;
    if (cfg->template_name && utf8_to_wide(cfg->template_name, wtpl, 128))
        ac.template_name = wtpl;
    if (cfg->net_adapter && utf8_to_wide(cfg->net_adapter, wnet, 128))
        ac.net_adapter = wnet;
    if (cfg->username && utf8_to_wide(cfg->username, wuser, 128))
        ac.username = wuser;
    if (cfg->password && utf8_to_wide(cfg->password, wpass, 128))
        ac.password = wpass;

    ac.ram_mb = (DWORD)cfg->ram_mb;
    ac.hdd_gb = (DWORD)cfg->hdd_gb;
    ac.cpu_cores = (DWORD)cfg->cpu_cores;
    ac.gpu_mode = cfg->gpu_mode;
    ac.network_mode = cfg->network_mode;
    ac.is_template = cfg->is_template ? TRUE : FALSE;

    HRESULT hr = asb_vm_create(&ac);
    if (FAILED(hr)) {
        if (err && err_sz) _snprintf_s(err, err_sz, _TRUNCATE, "asb_vm_create failed: 0x%08lx", (long)hr);
        return BACKEND_ERR_FAILED;
    }
    return BACKEND_OK;
}

static int backend_win_start_vm(const char *name)
{
    wchar_t wname[256];
    if (!name || !utf8_to_wide(name, wname, 256)) return BACKEND_ERR_INVALID_ARG;
    AsbVm vm = asb_vm_find(wname);
    if (!vm) return BACKEND_ERR_NOT_FOUND;
    HRESULT hr = asb_vm_start(vm, -1, -1, NULL);
    return SUCCEEDED(hr) ? BACKEND_OK : BACKEND_ERR_FAILED;
}

static int backend_win_stop_vm(const char *name, int force)
{
    wchar_t wname[256];
    if (!name || !utf8_to_wide(name, wname, 256)) return BACKEND_ERR_INVALID_ARG;
    AsbVm vm = asb_vm_find(wname);
    if (!vm) return BACKEND_ERR_NOT_FOUND;
    HRESULT hr = force ? asb_vm_stop(vm) : asb_vm_shutdown(vm);
    return SUCCEEDED(hr) ? BACKEND_OK : BACKEND_ERR_FAILED;
}

static int backend_win_delete_vm(const char *name)
{
    wchar_t wname[256];
    if (!name || !utf8_to_wide(name, wname, 256)) return BACKEND_ERR_INVALID_ARG;
    AsbVm vm = asb_vm_find(wname);
    if (!vm) return BACKEND_ERR_NOT_FOUND;
    HRESULT hr = asb_vm_delete(vm);
    return SUCCEEDED(hr) ? BACKEND_OK : BACKEND_ERR_FAILED;
}

static int backend_win_list_vms(CoreVmInfo *out, int max_count, int *out_count)
{
    if (!out || !out_count) return BACKEND_ERR_INVALID_ARG;
    int count = asb_vm_count();
    if (count > max_count) count = max_count;

    for (int i = 0; i < count; i++) {
        AsbVm vm = asb_vm_get(i);
        CoreVmInfo *info = &out[i];
        memset(info, 0, sizeof(*info));
        wide_to_utf8(asb_vm_name(vm), info->name, sizeof(info->name));
        wide_to_utf8(asb_vm_os_type(vm), info->os_type, sizeof(info->os_type));
        info->running = asb_vm_is_running(vm) ? 1 : 0;
        info->ram_mb = (int)asb_vm_ram_mb(vm);
        info->hdd_gb = (int)asb_vm_hdd_gb(vm);
        info->cpu_cores = (int)asb_vm_cpu_cores(vm);

        VmInstance *inst = asb_vm_instance(vm);
        if (inst) {
            info->install_complete = inst->install_complete ? 1 : 0;
            info->install_progress = inst->install_complete ? 100 : inst->vhdx_progress;
            wide_to_utf8(inst->vhdx_step, info->install_status, sizeof(info->install_status));
        } else {
            info->install_complete = 0;
            info->install_progress = -1;
        }
    }
    *out_count = count;
    return BACKEND_OK;
}

static int backend_win_open_display(const char *name, void *host_window)
{
    (void)name; (void)host_window;
    /* The existing Windows UI opens display windows directly via the ui.c
     * layer (vm_display_idd) rather than through the core. Wire this up
     * later if the core ever needs to drive display creation. */
    return BACKEND_ERR_NOT_IMPLEMENTED;
}

static void backend_win_set_event_cb(CoreVmEventCallback cb, void *user_data)
{
    g_event_cb = cb;
    g_event_user_data = user_data;
}

static const BackendVtbl g_backend_win = {
    backend_win_init,
    backend_win_cleanup,
    backend_win_create_vm,
    backend_win_start_vm,
    backend_win_stop_vm,
    backend_win_delete_vm,
    backend_win_list_vms,
    backend_win_open_display,
    backend_win_set_event_cb,
};

const BackendVtbl *backend_get(void)
{
    return &g_backend_win;
}
