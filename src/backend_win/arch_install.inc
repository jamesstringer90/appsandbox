/* arch_install.inc -- Arch Linux guest install, host side.
 *
 * Textually included by asb_core.c (after LinuxCreateArgs) so it can use that
 * file's private helpers (asb_log, notify_progress, spawn_iso_patch_prefetch,
 * remove_dir_recursive, g_dll_module). Kept out of asb_core.c to keep the
 * diff against upstream small. Not a separate translation unit. */

/* ================================================================
 * Arch Linux guest support
 *
 * Unlike Ubuntu (host-side squashfs -> ext4), Arch is installed *inside*
 * the guest: the VM boots the user's Arch live ISO with a generated
 * "seed" ISO at SCSI slot 2 (hcs_vm.c attaches image_path at slot 1 and
 * resources_iso_path at slot 2). The live environment auto-runs
 * install-arch.sh from the seed (see tools/linux/arch/), which
 * pacstraps onto the blank VHDX, builds dxgkrnl/asb_drm via DKMS, installs
 * Mesa d3d12 + the AppSandbox agents, reports progress over COM1, and powers
 * off. The host then boots the VHDX alone.
 * ================================================================ */

typedef enum { ASB_DISTRO_UNKNOWN = 0, ASB_DISTRO_UBUNTU, ASB_DISTRO_ARCH } AsbDistro;

static BOOL path_exists_w(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* Mount the ISO read-only and look at its layout. Arch/archiso/Arch ISOs
 * carry <iso>:\arch\ (airootfs.sfs, boot\); Ubuntu carries <iso>:\casper\. */
/* Files copied off an ISO keep its read-only attribute, which makes both
 * DeleteFileW and the VM-folder removal fail (ERROR_DIR_NOT_EMPTY) and would
 * leave the VM undeletable. Clear it on copy, and before deleting. */
static BOOL copy_from_iso(const wchar_t *src, const wchar_t *dst)
{
    if (!CopyFileW(src, dst, FALSE)) return FALSE;
    SetFileAttributesW(dst, FILE_ATTRIBUTE_NORMAL);
    return TRUE;
}

static void delete_file_force(const wchar_t *path)
{
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(path);
}

typedef struct {
    wchar_t kernel[MAX_PATH];   /* extracted vmlinuz */
    wchar_t initrd[MAX_PATH];   /* extracted initramfs */
    wchar_t label[64];          /* ISO volume label (archisolabel=) */
} ArchBootInfo;

/* Copy <iso>:\arch\boot\x86_64\vmlinuz-* + matching initramfs-*.img into
 * dest_dir. Prefers the stable Arch kernel for the installer environment
 * (the installed system's kernel is chosen separately by the script). */
static BOOL extract_arch_boot(wchar_t iso_drive, const wchar_t *dest_dir, ArchBootInfo *out)
{
    wchar_t spec[MAX_PATH], src[MAX_PATH], suffix[128] = L"";
    WIN32_FIND_DATAW fd;
    HANDLE h;
    static const wchar_t *dirs[] = { L"arch\\boot\\x86_64", L"arch\\boot" };

    for (int d = 0; d < 2; d++) {
        swprintf_s(spec, MAX_PATH, L"%c:\\%s\\vmlinuz-*", iso_drive, dirs[d]);
        h = FindFirstFileW(spec, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t *sfx = fd.cFileName + wcslen(L"vmlinuz-");
            /* prefer a non-LTS kernel; otherwise take what we find */
            if (!suffix[0] || (wcsstr(suffix, L"lts") && !wcsstr(sfx, L"lts")))
                wcsncpy_s(suffix, 128, sfx, _TRUNCATE);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (!suffix[0]) continue;

        swprintf_s(out->kernel, MAX_PATH, L"%s\\installer-vmlinuz", dest_dir);
        swprintf_s(out->initrd, MAX_PATH, L"%s\\installer-initramfs.img", dest_dir);
        swprintf_s(src, MAX_PATH, L"%c:\\%s\\vmlinuz-%s", iso_drive, dirs[d], suffix);
        if (!copy_from_iso(src, out->kernel)) {
            asb_log(L"extract_arch_boot: copy %s failed (%lu)", src, GetLastError());
            return FALSE;
        }
        swprintf_s(src, MAX_PATH, L"%c:\\%s\\initramfs-%s.img", iso_drive, dirs[d], suffix);
        if (!copy_from_iso(src, out->initrd)) {
            asb_log(L"extract_arch_boot: copy %s failed (%lu)", src, GetLastError());
            return FALSE;
        }
        asb_log(L"extract_arch_boot: using kernel '%s' from %s", suffix, dirs[d]);
        return TRUE;
    }
    asb_log(L"extract_arch_boot: no vmlinuz-* under arch\\boot");
    return FALSE;
}

static AsbDistro detect_iso_distro(const wchar_t *iso_path,
                                   const wchar_t *extract_dir, ArchBootInfo *boot)
{
    HANDLE iso_handle = INVALID_HANDLE_VALUE;
    DWORD cdrom_before = 0, cdrom_after, newly;
    wchar_t iso_drive = 0;
    AsbDistro result = ASB_DISTRO_UNKNOWN;
    VIRTUAL_STORAGE_TYPE st;
    OPEN_VIRTUAL_DISK_PARAMETERS op;

    for (int i = 0; i < 26; i++) {
        wchar_t root[4]; swprintf_s(root, 4, L"%c:\\", (wchar_t)(L'A' + i));
        if (GetDriveTypeW(root) == DRIVE_CDROM) cdrom_before |= (1u << i);
    }

    ZeroMemory(&st, sizeof(st));
    st.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
    ZeroMemory(&op, sizeof(op));
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    if (OpenVirtualDisk(&st, iso_path, VIRTUAL_DISK_ACCESS_READ,
                        OPEN_VIRTUAL_DISK_FLAG_NONE, &op, &iso_handle) != ERROR_SUCCESS) {
        asb_log(L"detect_iso_distro: OpenVirtualDisk failed (%lu)", GetLastError());
        return ASB_DISTRO_UNKNOWN;
    }
    if (AttachVirtualDisk(iso_handle, NULL, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                          0, NULL, NULL) != ERROR_SUCCESS) {
        asb_log(L"detect_iso_distro: AttachVirtualDisk failed (%lu)", GetLastError());
        CloseHandle(iso_handle);
        return ASB_DISTRO_UNKNOWN;
    }

    for (int i = 0; i < 20 && !iso_drive; i++) {
        cdrom_after = 0;
        for (int b = 0; b < 26; b++) {
            wchar_t root[4]; swprintf_s(root, 4, L"%c:\\", (wchar_t)(L'A' + b));
            if (GetDriveTypeW(root) == DRIVE_CDROM) cdrom_after |= (1u << b);
        }
        newly = cdrom_after & ~cdrom_before;
        for (int b = 0; b < 26 && newly; b++)
            if (newly & (1u << b)) { iso_drive = (wchar_t)(L'A' + b); break; }
        if (!iso_drive) Sleep(500);
    }
    if (iso_drive) {
        wchar_t root[4], vol[64], p[MAX_PATH];
        int ready = 0;
        swprintf_s(root, 4, L"%c:\\", iso_drive);
        for (int i = 0; i < 30 && !ready; i++) {
            if (GetVolumeInformationW(root, vol, 64, NULL, NULL, NULL, NULL, 0)) ready = 1;
            else Sleep(500);
        }
        if (ready) {
            swprintf_s(p, MAX_PATH, L"%c:\\casper", iso_drive);
            if (path_exists_w(p)) {
                result = ASB_DISTRO_UBUNTU;
            } else {
                swprintf_s(p, MAX_PATH, L"%c:\\arch", iso_drive);
                if (path_exists_w(p)) {
                    result = ASB_DISTRO_ARCH;
                    if (boot && extract_dir) {
                        wcsncpy_s(boot->label, 64, vol, _TRUNCATE);
                        if (!extract_arch_boot(iso_drive, extract_dir, boot))
                            boot->kernel[0] = L'\0';
                    }
                }
            }
        } else {
            asb_log(L"detect_iso_distro: %c:\\ never became accessible", iso_drive);
        }
    } else {
        asb_log(L"detect_iso_distro: ISO mounted but no drive letter");
    }

    DetachVirtualDisk(iso_handle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    CloseHandle(iso_handle);
    asb_log(L"detect_iso_distro: %s -> %s", iso_path,
            result == ASB_DISTRO_ARCH ? L"arch" :
            result == ASB_DISTRO_UBUNTU ? L"ubuntu" : L"unknown (defaulting to ubuntu)");
    return result;
}

/* Keep only [A-Za-z0-9._-] so values are safe inside a sh single-quoted
 * KEY='value' assignment; returns FALSE if anything else is present. */
static BOOL seed_safe_token(const char *s)
{
    if (!*s) return FALSE;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return FALSE;
    }
    return TRUE;
}

static void seed_w2u(const wchar_t *w, char *out, size_t cap)
{
    out[0] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = 0;
}

/* Write <staging>\seed.env: shell-sourceable per-VM settings consumed by
 * install-arch.sh. Values never contain a single quote (hash / tokens are
 * validated; the SSH key is rejected if it has one). */
static BOOL arch_write_seed_env(const wchar_t *path, LinuxCreateArgs *args,
                                   const char *pw_hash, const char *nat_ip,
                                   const char *ssh_pubkey)
{
    char user[128], host[256], gw[64] = "";
    FILE *f = NULL;

    seed_w2u(args->config.admin_user, user, sizeof(user));
    seed_w2u(args->config.name, host, sizeof(host));
    /* Hostname: lowercase alnum/hyphen, max 63. */
    {
        char h2[64]; size_t k = 0;
        for (const char *p = host; *p && k < 63; p++) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') h2[k++] = c;
            else if (c == ' ' || c == '_' || c == '.') h2[k++] = '-';
        }
        h2[k] = 0;
        if (!k) strcpy_s(h2, sizeof(h2), "arch");
        strcpy_s(host, sizeof(host), h2);
    }
    if (!seed_safe_token(user)) {
        swprintf_s(args->error_msg, 512,
                   L"Username contains characters not supported for Linux guests.");
        return FALSE;
    }
    if (strchr(pw_hash, '\'') || strchr(ssh_pubkey, '\'')) return FALSE;
    /* Unsafe locale/keyboard/timezone values fall back to defaults below. */
    if (nat_ip && nat_ip[0]) {
        const char *base = hcn_nat_subnet_base();
        snprintf(gw, sizeof(gw), "%s.1", base);
    }

    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return FALSE;
    fprintf(f, "ASB_USER='%s'\n", user);
    fprintf(f, "ASB_PWHASH='%s'\n", pw_hash);
    fprintf(f, "ASB_HOSTNAME='%s'\n", host);
    fprintf(f, "ASB_LOCALE='%s'\n",
            seed_safe_token(args->host_locale) ? args->host_locale : "en_US.UTF-8");
    fprintf(f, "ASB_XKB='%s'\n",
            seed_safe_token(args->host_xkb) ? args->host_xkb : "us");
    fprintf(f, "ASB_XKB_VARIANT='%s'\n",
            (args->host_variant[0] && seed_safe_token(args->host_variant)) ? args->host_variant : "");
    fprintf(f, "ASB_TZ='%s'\n",
            (args->host_tz[0] && seed_safe_token(args->host_tz)) ? args->host_tz : "UTC");
    fprintf(f, "ASB_SSH=%d\n", args->config.ssh_enabled ? 1 : 0);
    fprintf(f, "ASB_SSH_PUBKEY='%s'\n", ssh_pubkey);
    fprintf(f, "ASB_NET_IP='%s'\n", nat_ip ? nat_ip : "");
    fprintf(f, "ASB_NET_GW='%s'\n", gw);
    fprintf(f, "ASB_NET_PREFIX=24\n");
    fprintf(f, "ASB_GPU=%d\n", args->config.gpu_mode != GPU_NONE ? 1 : 0);
    fprintf(f, "ASB_MESA_TAG='%ls'\n", ASB_LINUX_MESA_TAG);
    fclose(f);
    return TRUE;
}

/* ---- Serial console session (COM1 named pipe) ----
 *
 * The installer's output comes back over COM1 (\\.\pipe\<vm>.com1). The pipe
 * is duplex, and the host also uses it to *start* the installer: the live
 * ISO runs a getty on ttyS0 where root has an empty password, so we log in
 * and type the start command once the "login:" prompt appears. */
typedef struct {
    HANDLE         pipe;              /* INVALID_HANDLE_VALUE until connected */
    const wchar_t *vm_name;
    char           linebuf[1024];
    size_t         linelen;
    BOOL           at_login;          /* saw a "login:" prompt */
    BOOL           script_started;    /* saw a line printed by install-arch.sh */
    BOOL           done_ok, done_fail;
    int            progress;          /* 0-100 from PROGRESS: lines */
    int            trigger_stage;     /* 0 = waiting for prompt, 1 = logged in, 2 = command typed */
    int            trigger_n;         /* start commands typed so far */
    ULONGLONG      last_trigger;
} ArchSerial;

/* Drain whatever the guest wrote, log it, and track the markers the host
 * cares about (PROGRESS:n, DONE:OK, DONE:FAIL, the installer's first lines). */
static void arch_serial_pump(ArchSerial *s)
{
    wchar_t name[300];
    DWORD avail = 0, got = 0;
    char buf[1024];

    if (s->pipe == INVALID_HANDLE_VALUE) {
        swprintf_s(name, 300, L"\\\\.\\pipe\\%s.com1", s->vm_name);
        s->pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        if (s->pipe == INVALID_HANDLE_VALUE) return;
    }
    if (!(PeekNamedPipe(s->pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) && s->linelen > 0) {
        /* No newline and the guest went quiet: a prompt ("login:") or a
         * fragment. Surface it so the log shows what the console is doing. */
        s->linebuf[s->linelen] = 0;
        asb_log(L"[arch] (partial) %S", s->linebuf);
        if (strstr(s->linebuf, "login:")) s->at_login = TRUE;
        s->linelen = 0;
    }
    while (PeekNamedPipe(s->pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD want = avail < sizeof(buf) ? avail : (DWORD)sizeof(buf);
        if (!ReadFile(s->pipe, buf, want, &got, NULL) || got == 0) break;
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (s->linelen + 1 < sizeof(s->linebuf)) s->linebuf[s->linelen++] = c;
                continue;
            }
            s->linebuf[s->linelen] = 0;
            s->linelen = 0;
            if (!s->linebuf[0]) continue;
            asb_log(L"[arch] %S", s->linebuf);
            if (strstr(s->linebuf, "login:")) s->at_login = TRUE;
            if (strncmp(s->linebuf, "==== ", 5) == 0 || strncmp(s->linebuf, "PROGRESS:", 9) == 0)
                s->script_started = TRUE;
            if (strncmp(s->linebuf, "DONE:OK", 7) == 0) s->done_ok = TRUE;
            else if (strncmp(s->linebuf, "DONE:FAIL", 9) == 0) s->done_fail = TRUE;
            else if (strncmp(s->linebuf, "PROGRESS:", 9) == 0) s->progress = atoi(s->linebuf + 9);
        }
    }
}

/* Start install-arch.sh from the serial console. Two steps, because the
 * login flushes the tty input queue: log in first, then type the command.
 * The script runs detached (setsid) so it outlives the shell, and the pgrep
 * guard makes a retyped command harmless. */
static void arch_serial_trigger(ArchSerial *s, ULONGLONG elapsed_ms)
{
    static const char login_seq[] = "\r\nroot\r\n\r\n";
    static const char start_cmd[] =
        "\r\nmkdir -p /seed; mountpoint -q /seed || mount -o ro -L ASBSEED /seed; "
        "pgrep -f install-arch >/dev/null || "
        "{ setsid bash /seed/payload/arch/install-arch.sh </dev/null >/dev/null 2>/dev/ttyS0 & }\r\n";
    ULONGLONG now = GetTickCount64();
    DWORD w = 0;

    if (s->script_started || s->pipe == INVALID_HANDLE_VALUE || s->trigger_n >= 3) return;

    if (s->trigger_stage == 0) {
        /* Log in as soon as the prompt shows (fallback: after 2 minutes). */
        if (!s->at_login && elapsed_ms < 120000) return;
        asb_log(L"Arch: logging in on the serial console...");
        WriteFile(s->pipe, login_seq, (DWORD)(sizeof(login_seq) - 1), &w, NULL);
        s->trigger_stage = 1;
        s->last_trigger = now;
    } else if (now - s->last_trigger > (s->trigger_stage == 1 ? 8000u : 30000u)) {
        asb_log(L"Arch: starting the installer from the serial console (%d)...",
                s->trigger_n + 1);
        WriteFile(s->pipe, start_cmd, (DWORD)(sizeof(start_cmd) - 1), &w, NULL);
        s->trigger_stage = 2;
        s->trigger_n++;
        s->last_trigger = now;
    }
}

/* Dev builds (bin\Release inside a checkout) read the guest sources from the
 * local tools\linux tree instead of GitHub, so unpushed guest changes are
 * what gets installed. */
static void arch_use_local_tree(void)
{
    wchar_t root[MAX_PATH], probe[MAX_PATH], *sl;

    GetModuleFileNameW(g_dll_module, root, MAX_PATH);
    for (int i = 0; i < 3; i++) {      /* dll -> Release -> bin -> repo root */
        sl = wcsrchr(root, L'\\');
        if (sl) *sl = L'\0';
    }
    swprintf_s(probe, MAX_PATH, L"%s\\tools\\linux\\arch\\install-arch.sh", root);
    if (path_exists_w(probe)) {
        SetEnvironmentVariableW(L"ASB_REPO_LOCAL", root);
        asb_log(L"Arch: using local source tree %s", root);
    }
}

/* Build the seed ISO: payload (agent/DKMS/units from the repo prefetch +
 * wsl-deps) and seed.env (per-VM settings incl. the password hash). */
static HRESULT arch_stage_seed(LinuxCreateArgs *args, VmInstance *network_vm,
                                  const wchar_t *seed_iso)
{
    wchar_t staging[MAX_PATH], extras[MAX_PATH], p[MAX_PATH], arg[2048];
    char pw_hash[256] = {0}, ssh_pubkey[640] = {0};
    HRESULT hr;

    swprintf_s(staging, MAX_PATH, L"%s\\_seed_staging", args->vhdx_dir);
    CreateDirectoryW(staging, NULL);
    swprintf_s(extras, MAX_PATH, L"%s\\payload", staging);
    CreateDirectoryW(extras, NULL);

    arch_use_local_tree();
    asb_log(L"Arch 1/3: fetching the Linux guest sources...");
    swprintf_s(arg, 2048,
        L"--prefetch-repo --tag \"" ASB_LINUX_CLIENT_TAG
        L"\" --mesa-tag \"" ASB_LINUX_MESA_TAG
        L"\" --dxgkrnl-tag \"" ASB_LINUX_DXGKRNL_TAG L"\" --out-dir \"%s\"",
        extras);
    if (spawn_iso_patch_prefetch(arg) != 0) {
        swprintf_s(args->error_msg, 512, L"Failed to download the AppSandbox Linux sources.");
        hr = E_FAIL;
        goto fail;
    }
    swprintf_s(p, MAX_PATH, L"%s\\arch\\install-arch.sh", extras);
    if (!path_exists_w(p)) {
        swprintf_s(args->error_msg, 512,
                   L"The source repo has no tools/linux/arch/install-arch.sh "
                   L"(the pinned client tag predates Arch support; use a dev build "
                   L"inside a checkout, or publish a tag that includes it).");
        hr = E_FAIL;
        goto fail;
    }

    asb_log(L"Arch 2/3: wsl-deps .so libs...");
    swprintf_s(p, MAX_PATH, L"%s\\wsl-deps", extras);
    swprintf_s(arg, 2048, L"--prefetch-wsl-deps --out-dir \"%s\"", p);
    if (spawn_iso_patch_prefetch(arg) != 0)
        asb_log(L"WARN: prefetch-wsl-deps failed (GPU libs will be missing)");

    if (!args->config.admin_pass[0] ||
        !unix_password_hash(args->config.admin_pass, pw_hash, sizeof(pw_hash))) {
        swprintf_s(args->error_msg, 512, L"Failed to hash the Linux password.");
        SecureZeroMemory(args->config.admin_pass, sizeof(args->config.admin_pass));
        hr = E_FAIL;
        goto fail;
    }
    SecureZeroMemory(args->config.admin_pass, sizeof(args->config.admin_pass));
    if (network_vm->ssh_deploy_key)
        seed_w2u(network_vm->ssh_pubkey, ssh_pubkey, sizeof(ssh_pubkey));

    swprintf_s(p, MAX_PATH, L"%s\\seed.env", staging);
    if (!arch_write_seed_env(p, args, pw_hash, network_vm->nat_ip, ssh_pubkey)) {
        if (!args->error_msg[0])
            swprintf_s(args->error_msg, 512, L"Failed to write installer seed settings.");
        hr = E_FAIL;
        goto fail;
    }

    hr = iso_create_from_dir(seed_iso, staging, L"ASBSEED");
    if (FAILED(hr))
        swprintf_s(args->error_msg, 512, L"Failed to build the installer seed ISO (0x%08X)", hr);

fail:
    SecureZeroMemory(pw_hash, sizeof(pw_hash));
    remove_dir_recursive(staging);   /* seed.env held the password hash */
    return hr;
}

/* Boot the live ISO's own kernel + initramfs (HCS LinuxKernelDirect, no
 * bootloader), start the installer over the serial console, and wait for it
 * to power the VM off. S_OK only if the installer reported DONE:OK. */
static HRESULT arch_run_installer_vm(LinuxCreateArgs *args, const wchar_t *iso_path,
                                        const wchar_t *seed_iso, const ArchBootInfo *boot)
{
    ArchSerial ser;
    VmInstance *ti;                  /* ~600 KB: heap, not stack */
    BOOL seen_running = FALSE;
    int last_progress = -1, not_running = 0;
    ULONGLONG t0;
    HRESULT hr;

    ZeroMemory(&ser, sizeof(ser));
    ser.pipe = INVALID_HANDLE_VALUE;
    ser.vm_name = args->config.name;

    wcscpy_s(args->config.image_path, MAX_PATH, iso_path);
    wcscpy_s(args->config.resources_iso_path, MAX_PATH, seed_iso);
    wcscpy_s(args->config.kernel_path, MAX_PATH, boot->kernel);
    wcscpy_s(args->config.initrd_path, MAX_PATH, boot->initrd);
    /* multi-user.target: no desktop/display manager needed in the live env. */
    swprintf_s(args->config.kernel_cmdline, 1024,
        L"archisobasedir=arch archisolabel=%s copytoram=n cow_spacesize=4G "
        L"console=ttyS0,115200 console=tty0 loglevel=4 systemd.unit=multi-user.target",
        boot->label);

    ti = (VmInstance *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmInstance));
    if (!ti) {
        swprintf_s(args->error_msg, 512, L"Out of memory.");
        return E_OUTOFMEMORY;
    }
    hr = (args->endpoint_guid[0] != L'\0')
        ? hcs_create_vm_with_endpoint(&args->config, args->endpoint_guid, ti)
        : hcs_create_vm(&args->config, ti);
    if (FAILED(hr)) {
        swprintf_s(args->error_msg, 512, L"Failed to create installer VM (0x%08X)", hr);
        goto out;
    }
    hr = hcs_start_vm(ti);
    if (FAILED(hr)) {
        swprintf_s(args->error_msg, 512, L"Failed to start installer VM (0x%08X)", hr);
        hcs_close_vm(ti);
        goto out;
    }

    asb_log(L"Arch 3/3: installing inside the guest (10-30 minutes, network required)...");
    t0 = GetTickCount64();
    for (;;) {
        ULONGLONG elapsed;
        Sleep(2000);
        elapsed = GetTickCount64() - t0;
        arch_serial_pump(&ser);
        arch_serial_trigger(&ser, elapsed);

        if (ser.progress != last_progress) {
            VmInstance *vm = asb_find_vm_by_id(args->vm_unique_id);
            last_progress = ser.progress;
            if (vm) {
                vm->vhdx_progress = ser.progress;
                notify_progress(vm_handle(vm), ser.progress, TRUE, g_progress_ud);
            }
        }
        if (!asb_find_vm_by_id(args->vm_unique_id)) { hr = E_ABORT; break; }   /* deleted meanwhile */
        if (hcs_is_running_by_enum(args->config.name)) {
            seen_running = TRUE;
            not_running = 0;
        } else if (seen_running || elapsed > 120000) {
            if (++not_running >= 2) { hr = S_OK; break; }                       /* powered off */
        }
        if (elapsed > 90ull * 60 * 1000) {
            swprintf_s(args->error_msg, 512, L"Arch installer timed out after 90 minutes.");
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
    }
    arch_serial_pump(&ser);       /* last lines (DONE:OK) arrive just before power-off */
    if (ser.pipe != INVALID_HANDLE_VALUE) CloseHandle(ser.pipe);

    hcs_terminate_vm(ti);
    hcs_close_vm(ti);
    Sleep(3000);                     /* let the async close finish */

    if (hr == S_OK && !ser.done_ok) {
        swprintf_s(args->error_msg, 512, ser.done_fail
            ? L"Arch installer reported a failure (see the log for [arch] lines)."
            : L"Arch installer exited without reporting success.");
        hr = E_FAIL;
    }
out:
    HeapFree(GetProcessHeap(), 0, ti);
    return hr;
}

/* In-guest install of an Arch ISO onto a fresh VHDX. Returns S_OK once
 * the installer reported success and the VM powered off; on return the VM
 * config points at the VHDX alone. */
static HRESULT arch_install(LinuxCreateArgs *args, VmInstance *network_vm,
                               const wchar_t *iso_path, const ArchBootInfo *boot)
{
    wchar_t seed_iso[MAX_PATH];
    HRESULT hr;

    if (args->config.network_mode != NET_NAT || !network_vm || !network_vm->nat_ip[0]) {
        /* The live installer needs a known static address (no DHCP on the
         * HCN NAT network). */
        swprintf_s(args->error_msg, 512, L"Arch install requires NAT networking.");
        return E_INVALIDARG;
    }
    if (!boot || !boot->kernel[0]) {
        swprintf_s(args->error_msg, 512,
                   L"Could not find the Arch live kernel/initramfs on the ISO.");
        return E_FAIL;
    }

    DeleteFileW(args->config.vhdx_path);
    hr = vhdx_create(args->config.vhdx_path, args->config.hdd_gb);
    if (FAILED(hr)) {
        swprintf_s(args->error_msg, 512, L"Failed to create VHDX (0x%08X)", hr);
        return hr;
    }
    args->vhdx_created = TRUE;

    swprintf_s(seed_iso, MAX_PATH, L"%s\\seed.iso", args->vhdx_dir);
    hr = arch_stage_seed(args, network_vm, seed_iso);
    if (SUCCEEDED(hr))
        hr = arch_run_installer_vm(args, iso_path, seed_iso, boot);

    /* The runtime VM boots the VHDX alone (systemd-boot lives on its ESP). */
    DeleteFileW(seed_iso);
    delete_file_force(boot->kernel);
    delete_file_force(boot->initrd);
    args->config.image_path[0] = L'\0';
    args->config.resources_iso_path[0] = L'\0';
    args->config.kernel_path[0] = L'\0';
    args->config.initrd_path[0] = L'\0';
    args->config.kernel_cmdline[0] = L'\0';
    network_vm->image_path[0] = L'\0';
    network_vm->resources_iso_path[0] = L'\0';

    if (SUCCEEDED(hr)) {
        vm_save_state_json(args->config.vhdx_path, TRUE);
        asb_log(L"Arch install finished.");
    }
    return hr;
}
