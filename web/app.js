/* App Sandbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
let selectedVm = -1;
let selectedSnap = {};  /* vmIndex -> string value: 'current', 'base', 'base-N', 'S', 'S-N' */
let editModeRow = -1;
let editingCell = null; /* {row, col, element} */
let pendingConfirm = null; /* {resolve} */
let minSizeReported = false;
let lastHostInfo = null;
let rowCache = {};          /* vm.name -> <tr> — persistent rows so the status spinner doesn't reset on every update */
let rowSigCache = {};       /* vm.name -> last render signature; skip rebuild when unchanged */

/* ---- Collapsible sections ---- */
function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    localStorage.setItem('collapse_' + id, collapsed ? '1' : '0');
}
(function restoreCollapse() {
    var defaults = { 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        var val = localStorage.getItem('collapse_' + id);
        if (val === null) val = defaults[id];
        if (val === '1') document.getElementById(id).classList.add('collapsed');
    });
})();

const netNames = ['None', 'NAT', 'External', 'Internal'];

/* ---- Message bridge ----
 *
 * Two host environments are supported:
 *   - WebView2 on Windows  (window.chrome.webview)
 *   - WKWebView on macOS   (window.webkit.messageHandlers.host)
 *
 * Native code on both platforms calls window.onHostMessage(obj) with a
 * parsed message object; the JS side only sees one uniform surface. On
 * Windows we keep using the native chrome.webview event path because it
 * is the existing, tested route — onHostMessage is simply wired into the
 * same listener.
 */

var hostBridge = (function() {
    var isWebView2 = !!(window.chrome && window.chrome.webview);
    var isWKWebView = !!(window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.host);

    function send(action, data) {
        var msg = Object.assign({ action: action }, data || {});
        if (isWebView2) {
            window.chrome.webview.postMessage(msg);
        } else if (isWKWebView) {
            /* WKWebView only accepts JSON-serializable values; strings round-trip
             * most reliably so we hand the native side the raw JSON text. */
            window.webkit.messageHandlers.host.postMessage(JSON.stringify(msg));
        } else {
            console.warn('[hostBridge] no native host available; dropping', msg);
        }
    }

    return { send: send, isWebView2: isWebView2, isWKWebView: isWKWebView, isMac: isWKWebView };
})();

function sendCmd(action, data) { hostBridge.send(action, data); }

/* On a macOS host, hide the Windows-*host*-only features (templates, snapshots,
 * test-mode, build-template — none supported when the host is a Mac) and the
 * dormant Linux-version row. The OS-type dropdown stays ENABLED so the user can
 * pick Windows (built from a Microsoft ISO via QEMU) or macOS (VZ restore image).
 * Per-OS field visibility — including the .needs-iso picker — is driven by
 * applyOsTypeUI(), which runs on both hosts. */
if (hostBridge.isMac) {
    var hide = document.querySelectorAll('.win-only, .needs-linux-version');
    for (var i = 0; i < hide.length; i++) hide[i].style.display = 'none';
}

/* OS Type dropdown: drop guest types that aren't available on this host.
 * Windows host: macOS unavailable (Apple Virtualization is Mac-only).
 * macOS host:   Linux unavailable (Windows IS supported — QEMU+ivshmem). */
{
    var unavailable = hostBridge.isMac ? ['Linux'] : ['macOS'];
    unavailable.forEach(function(v) {
        var opt = document.querySelector('#os-type option[value="' + v + '"]');
        if (opt) opt.remove();
    });
}

/* Apply Create-modal visibility rules for the currently selected OS type.
 *   Windows: .win-only shown, .needs-iso shown,             .needs-linux-version hidden
 *   Linux:   .win-only hidden, .needs-iso hidden,           .needs-linux-version shown
 *   macOS:   handled by the isMac branch above; this function is a no-op there.
 *
 * Linux is back to user-picks-an-ISO (Ubuntu Desktop ISO etc.), same as
 * Windows. The version-dropdown / cloud-image flow is preserved in
 * asb_core.c under #if 0 in case we need to bring it back. */
function applyOsTypeUI() {
    var osType = document.getElementById('os-type').value;
    var isWindows = osType === 'Windows';
    var isLinux = osType === 'Linux';
    var winOnly = document.querySelectorAll('.win-only');
    var needsIso = document.querySelectorAll('.needs-iso');
    var needsWindows = document.querySelectorAll('.needs-windows');
    var needsLinuxVersion = document.querySelectorAll('.needs-linux-version');
    /* .win-only = template/snapshot features that exist only on a Windows *host*;
       never shown on a Mac host, even for a Windows guest. */
    for (var i = 0; i < winOnly.length; i++)
        winOnly[i].style.display = (!hostBridge.isMac && isWindows) ? '' : 'none';
    /* .needs-windows = Windows-*guest* options (Test Mode); shown for a Windows
       guest on EITHER host (a Windows-on-Mac VM uses it too), hidden otherwise. */
    for (var w = 0; w < needsWindows.length; w++) needsWindows[w].style.display = isWindows ? '' : 'none';
    /* ISO picker shows for both Windows and Linux now. */
    for (var j = 0; j < needsIso.length; j++) needsIso[j].style.display = (isWindows || isLinux) ? '' : 'none';
    /* Linux distribution dropdown is dormant — kept in the DOM but always
       hidden so the cloud-image code path can be revived without
       re-adding the markup. */
    for (var k = 0; k < needsLinuxVersion.length; k++) needsLinuxVersion[k].style.display = 'none';
    /* Swap the default VM name between OS conventions, but only when the
       field still holds the *other* OS's untouched default — never clobber a
       name the user typed. Linux hostnames must be lowercase. */
    var nameEl = document.getElementById('vm-name');
    if (isLinux && nameEl.value === 'MyAppSandbox') nameEl.value = 'myappsandbox';
    else if (!isLinux && nameEl.value === 'myappsandbox') nameEl.value = 'MyAppSandbox';
    revalidateVmName();
    revalidateUsername();
    revalidatePassword();
    updateCreateButtons();
}

/* Unified dispatch. Native code on either platform calls
 * window.onHostMessage(obj) with an already-parsed object. WebView2 also
 * delivers messages through chrome.webview.addEventListener('message'),
 * which we route into the same handler so both paths end up in one place. */
window.onHostMessage = function(msg) {
    if (!msg || typeof msg !== 'object') return;
    switch (msg.type) {
        case 'fullState':     onFullState(msg); break;
        case 'vmListChanged': vms = msg.vms; renderVmTable(); updateHostInfo(msg.hostInfo); revalidateVmName(); break;
        case 'vmStateChanged': onVmStateChanged(msg); break;
        case 'snapListChanged': break; /* snapshots now inline in vmListChanged */
        case 'log':           appendLog(msg.message); break;
        case 'hostInfo':      updateHostInfo(msg); break;
        case 'browseResult':  onBrowseResult(msg.path); break;
        case 'confirmResult': if (pendingConfirm) pendingConfirm.resolve(msg.confirmed); break;
        case 'adapters':      populateAdapters(msg.adapters, msg.defaultIndex); break;
        case 'templates':     populateTemplates(msg.templates); break;
        case 'alert':         showModal('Error', msg.message, 'OK'); break;
        case 'prereqRequired': onPrereqRequired(); break;
        case 'prereqReboot':   onPrereqReboot(); break;
        case 'prereqProgress': onPrereqProgress(msg); break;
        case 'prereqResult':   onPrereqResult(msg); break;
    }
};

/* WebView2 delivers events as DOM CustomEvents; forward them into
 * window.onHostMessage so both transports converge on the same handler. */
if (hostBridge.isWebView2) {
    window.chrome.webview.addEventListener('message', function(event) {
        window.onHostMessage(event.data);
    });
}

/* ---- Initial state ---- */

function onFullState(msg) {
    vms = msg.vms || [];
    renderVmTable();
    revalidateVmName();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
    if (msg.adapters) populateAdapters(msg.adapters, msg.defaultAdapter);
    if (msg.templates) populateTemplates(msg.templates);
    if (!minSizeReported) {
        minSizeReported = true;
        setTimeout(reportMinSize, 50);
    }
}

/* Hyper-V/HCS requires VM memory aligned to 2 MB, so RAM (MB) must be even;
   round an odd value down by 1 (an odd value is rejected and the VM won't boot). */
function alignRamMb(mb) { return mb - (mb % 2); }

function applySmartDefaults(info) {
    var ram = Math.min(Math.floor(info.hostRamMb / 2), 16384);
    var cores = Math.min(Math.floor(info.hostCores / 2), 8);
    if (ram < 512) ram = 512;
    if (cores < 1) cores = 1;
    document.getElementById('ram-size').value = alignRamMb(ram);
    document.getElementById('cpu-cores').value = cores;
}

function onVmStateChanged(msg) {
    if (msg.vmIndex >= 0 && msg.vmIndex < vms.length) {
        Object.assign(vms[msg.vmIndex], msg);
    }
    renderVmTable();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
}

/* ---- Host info ---- */

function updateHostInfo(info) {
    if (!info) return;
    lastHostInfo = info;
    var el;
    el = document.getElementById('host-cpu');
    if (el) el.textContent = 'Host: ' + info.hostCores + ' cores | VMs using: ' + info.vmCores;
    el = document.getElementById('host-ram');
    if (el) el.textContent = 'Host: ' + info.hostRamMb + ' MB | VMs using: ' + info.vmRamMb + ' MB';
    el = document.getElementById('host-hdd');
    if (el) el.textContent = 'Free: ' + info.freeGb + ' GB | VMs allocated: ' + info.vmHddGb + ' GB';
}

/* ---- Adapters ---- */

var currentAdapters = [];
var currentDefaultAdapter = '';

function populateAdapters(adapters, defaultIdx) {
    var sel = document.getElementById('net-adapter');
    sel.innerHTML = '<option value="">(Auto)</option>';
    currentAdapters = adapters || [];
    if (adapters) {
        adapters.forEach(function(a) {
            var opt = document.createElement('option');
            opt.value = a;
            opt.textContent = a;
            sel.appendChild(opt);
        });
    }
    if (typeof defaultIdx === 'number' && defaultIdx >= 0 && defaultIdx < sel.options.length) {
        sel.selectedIndex = defaultIdx;
        currentDefaultAdapter = sel.value;
    } else if (adapters && adapters.length > 0) {
        currentDefaultAdapter = adapters[0];
    }
}

/* ---- Templates ---- */

var currentTemplates = [];

function templateDefaultLabel() {
    var n = currentTemplates.length;
    if (n === 0) return '(None)';
    return '(' + n + ' template' + (n === 1 ? '' : 's') + ' available)';
}

function populateTemplates(templates) {
    currentTemplates = templates || [];
    var list = document.getElementById('template-dropdown-list');
    var hidden = document.getElementById('template-select');
    list.innerHTML = '';

    /* Default (None) item — always shows "None" inside the list */
    var noneItem = document.createElement('div');
    noneItem.className = 'template-dropdown-item';
    noneItem.innerHTML = '<span class="tpl-name">(None)</span>';
    noneItem.addEventListener('click', function() { selectTemplate('', templateDefaultLabel()); });
    list.appendChild(noneItem);

    currentTemplates.forEach(function(t) {
        var item = document.createElement('div');
        item.className = 'template-dropdown-item';

        var nameSpan = document.createElement('span');
        nameSpan.className = 'tpl-name';
        nameSpan.textContent = t.name + ' [' + t.osType + ']';
        item.appendChild(nameSpan);

        var delBtn = document.createElement('span');
        delBtn.className = 'tpl-delete';
        delBtn.textContent = '\uD83D\uDDD1\uFE0F';
        delBtn.title = 'Delete template';
        delBtn.addEventListener('click', function(e) {
            e.stopPropagation();
            closeTemplateDropdown();
            onDeleteTemplate(t.name);
        });
        item.appendChild(delBtn);

        item.addEventListener('click', function() {
            selectTemplate(t.name, t.name + ' [' + t.osType + ']');
        });
        list.appendChild(item);
    });

    /* If the currently selected template was deleted, reset */
    if (hidden.value !== '') {
        var found = currentTemplates.some(function(t) { return t.name === hidden.value; });
        if (!found) selectTemplate('', templateDefaultLabel());
    } else {
        /* No template selected — update default label in case count changed */
        document.getElementById('template-dropdown-selected').textContent = templateDefaultLabel();
    }
}

function selectTemplate(value, label) {
    document.getElementById('template-select').value = value;
    document.getElementById('template-dropdown-selected').textContent = label;
    closeTemplateDropdown();
    if (value !== '') {
        document.getElementById('image-path').value = '';
    }
    updateCreateButtons();
}

function closeTemplateDropdown() {
    document.getElementById('template-dropdown').classList.remove('open');
}

document.getElementById('template-dropdown-selected').addEventListener('click', function() {
    document.getElementById('template-dropdown').classList.toggle('open');
});

/* Close dropdown when clicking outside */
document.addEventListener('click', function(e) {
    if (!e.target.closest('#template-dropdown')) {
        closeTemplateDropdown();
    }
});

function onDeleteTemplate(name) {
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete template "' + name + '"?\n\nThis will permanently delete the template disk image.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteTemplate', { name: name });
        }
    });
}

/* ---- Browse result ---- */

function onBrowseResult(path) {
    if (path) {
        document.getElementById('image-path').value = path;
        selectTemplate('', templateDefaultLabel());
        updateCreateButtons();
    }
}

/* ---- Create buttons state ---- */

function updateCreateButtons() {
    var osType = document.getElementById('os-type').value;
    var hasImage = (document.getElementById('image-path').value.trim() !== '');
    var hasTpl = document.getElementById('template-select').value !== '';
    /* macOS guests auto-download their restore image (no path needed). Windows
       and Linux guests build from a user-picked ISO — or, on a Windows host, a
       saved template. Holds on both hosts: on a Mac the template UI is hidden so
       hasTpl stays false and a Windows guest genuinely requires the ISO. */
    var createOk = (osType === 'macOS') ? true : (hasImage || hasTpl);
    document.getElementById('btn-create').disabled = !createOk;
    /* Templates are Windows-only; disabling create-as-template for Linux
       (and macOS) is fine since hasImage is the only signal we check. */
    document.getElementById('btn-create-template').disabled = (osType !== 'Windows') || !hasImage;
}

/* Wire up change events */
document.getElementById('image-path').addEventListener('input', function() {
    if (this.value.trim() !== '') {
        selectTemplate('', templateDefaultLabel());
    }
    updateCreateButtons();
});

/* RAM must be 2 MB-aligned: snap an odd entry down by 1 when the field is committed. */
document.getElementById('ram-size').addEventListener('change', function() {
    var mb = parseInt(this.value, 10);
    if (!isNaN(mb)) this.value = alignRamMb(mb);
});

function revalidateVmName() {
    var name = document.getElementById('vm-name').value.trim();
    document.getElementById('vm-name-warn').textContent = validateVmName(name) || '';
}
document.getElementById('vm-name').addEventListener('input', revalidateVmName);
/* Username validity depends on the VM name (Windows: username != VM name). */
document.getElementById('vm-name').addEventListener('input', revalidateUsername);

function revalidateUsername() {
    var u = document.getElementById('admin-user').value.trim();
    document.getElementById('admin-user-warn').textContent = validateUsername(u) || '';
}
function revalidatePassword() {
    var p = document.getElementById('admin-pass').value;
    document.getElementById('admin-pass-warn').textContent = validatePassword(p) || '';
}
document.getElementById('admin-user').addEventListener('input', revalidateUsername);

function checkPasswordMatch() {
    var pass = document.getElementById('admin-pass').value;
    var confirm = document.getElementById('admin-confirm');
    if (confirm.value === '' && pass === '') {
        confirm.classList.remove('pass-mismatch', 'pass-match');
        return;
    }
    if (confirm.value === pass) {
        confirm.classList.remove('pass-mismatch');
        confirm.classList.add('pass-match');
    } else {
        confirm.classList.remove('pass-match');
        confirm.classList.add('pass-mismatch');
    }
}
document.getElementById('admin-pass').addEventListener('input', function() {
    checkPasswordMatch();
    revalidatePassword();
});
document.getElementById('admin-confirm').addEventListener('input', checkPasswordMatch);
checkPasswordMatch();

function showPassword() {
    document.getElementById('admin-pass').type = 'text';
    document.getElementById('admin-confirm').type = 'text';
}
function hidePassword() {
    document.getElementById('admin-pass').type = 'password';
    document.getElementById('admin-confirm').type = 'password';
}

function onNetModeChange() {
    /* Adapter dropdown only relevant for External */
    var mode = parseInt(document.getElementById('net-mode').value);
    var show = (mode === 2) ? '' : 'none';
    document.getElementById('net-adapter').style.display = show;
    document.getElementById('net-adapter-label').style.display = show;
}
onNetModeChange();

/* ---- Create VM ---- */

function gatherConfig() {
    var osType = document.getElementById('os-type').value;
    /* Same ISO-picker path for Windows and Linux. The cloud-image
       Linux-version dropdown is dormant (see applyOsTypeUI). */
    var imagePath = document.getElementById('image-path').value.trim();
    return {
        name:        document.getElementById('vm-name').value.trim(),
        osType:      osType,
        imagePath:   imagePath,
        templateName: document.getElementById('template-select').value,
        hddGb:       parseInt(document.getElementById('hdd-size').value) || 64,
        ramMb:       alignRamMb(parseInt(document.getElementById('ram-size').value) || 16384),
        cpuCores:    parseInt(document.getElementById('cpu-cores').value) || 8,
        gpuMode:     parseInt(document.getElementById('gpu-mode').value),
        networkMode: parseInt(document.getElementById('net-mode').value),
        netAdapter:  document.getElementById('net-adapter').value,
        adminUser:   document.getElementById('admin-user').value.trim(),
        adminPass:   document.getElementById('admin-pass').value,
        adminConfirm: document.getElementById('admin-confirm').value,
        testMode:    document.getElementById('test-mode').checked,
        sshEnabled:  document.getElementById('ssh-enabled').checked,
        sshDeployKey: document.getElementById('ssh-deploy-key').checked
    };
}

/* "Deploy SSH key" depends on "SSH Server": grey it out (and clear it) unless
   SSH is enabled. The core also gates deploy on ssh_enabled as a backstop. */
function onSshToggle() {
    var ssh = document.getElementById('ssh-enabled').checked;
    var dep = document.getElementById('ssh-deploy-key');
    dep.disabled = !ssh;
    if (!ssh) dep.checked = false;
}

function clearCreateForm() {
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    updateCreateButtons();
}

/* VM name / hostname validation. Per-guest-OS rules, keyed off the
   selected OS Type (on a macOS host the dropdown is locked to 'macOS',
   so osType is an accurate guest discriminator on all hosts). */
function validateVmName(name) {
    if (!name) return 'VM name is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'macOS') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (macOS LocalHostName limit).';
    } else if (osType === 'Linux') {
        if (name.length > 63) return 'VM name cannot exceed 63 characters (Linux hostname limit).';
        if (/[A-Z]/.test(name)) return 'Linux hostname must be lowercase.';
    } else { /* Windows */
        if (name.length > 15) return 'VM name cannot exceed 15 characters (NetBIOS limit).';
    }
    if (/[^a-zA-Z0-9-]/.test(name)) return 'VM name can only contain letters, digits, and hyphens.';
    if (/^\d+$/.test(name)) return 'VM name cannot be only digits.';
    if (name.startsWith('-') || name.endsWith('-')) return 'VM name cannot start or end with a hyphen.';
    var lower = name.toLowerCase();
    for (var i = 0; i < vms.length; i++) {
        if (vms[i].name.toLowerCase() === lower) return 'A VM with this name already exists.';
    }
    for (var j = 0; j < currentTemplates.length; j++) {
        if (currentTemplates[j].name.toLowerCase() === lower) return 'A template with this name already exists.';
    }
    return null;
}

/* Username validation. Per-guest-OS rules keyed off osType. Each branch
   is explicit so it's clear which OS's account rules apply. */
function validateUsername(name) {
    if (!name) return 'Username is required.';
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        /* Ubuntu useradd/adduser: lowercase, start with a letter or
           underscore, then [a-z0-9_-], max 32 chars. */
        if (name.length > 32) return 'Username cannot exceed 32 characters (Linux limit).';
        if (!/^[a-z_][a-z0-9_-]*$/.test(name))
            return 'Lowercase alphanumeric only.';
        return null;
    }
    /* macOS and Windows: keep the existing Windows-account ruleset.
       (macOS-specific shortname rules are not yet verified; treated the
       same as Windows for now — see validatePassword note.) */
    if (name.length > 20) return 'Username cannot exceed 20 characters.';
    if (/["\\/\[\]:;|=,+*?<>]/.test(name)) return 'Username contains invalid characters.';
    if (/^[.\s]+$/.test(name)) return 'Username cannot be only dots or spaces.';
    if (name.endsWith('.')) return 'Username cannot end with a period.';
    var reserved = ['CON','PRN','AUX','NUL',
        'COM1','COM2','COM3','COM4','COM5','COM6','COM7','COM8','COM9',
        'LPT1','LPT2','LPT3','LPT4','LPT5','LPT6','LPT7','LPT8','LPT9'];
    if (reserved.indexOf(name.toUpperCase()) >= 0) return 'Username is a reserved name.';
    /* Windows guests: the VM name becomes the guest ComputerName, and a user
       named like the machine cannot be added to local groups during the
       unattend pass (LookupAccountName resolves the machine object first,
       NetLocalGroupAddMembers fails with ERROR_NO_SUCH_MEMBER). The account
       ends up group-less, OOBE autologs defaultuser0 instead and the agent
       never installs. */
    if (osType === 'Windows') {
        var vmNameEl = document.getElementById('vm-name');
        var vmName = vmNameEl ? vmNameEl.value.trim() : '';
        if (vmName && name.toLowerCase() === vmName.toLowerCase())
            return 'Username cannot be the same as the VM name.';
    }
    return null;
}

/* Password validation. Per-guest-OS rules keyed off osType.
   - Linux: Ubuntu accepts ALL characters via the host's $6$ hash path
     (usermod -p bypasses pwquality), so the only limits are non-empty
     and a sane byte ceiling.
   - macOS / Windows: no extra content rule enforced here today. */
function validatePassword(pass) {
    var osSelect = document.getElementById('os-type');
    var osType = osSelect ? osSelect.value : 'Windows';
    if (osType === 'Linux') {
        if (!pass) return 'Password is required.';
        /* UTF-8 byte length (encodeURIComponent escapes multibyte). */
        var bytes = unescape(encodeURIComponent(pass)).length;
        if (bytes > 255) return 'Password is too long (max 255 bytes).';
        return null;
    }
    /* macOS / Windows: no additional constraints today. */
    return null;
}

function onCreateVm() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    var userErr = validateUsername(cfg.adminUser);
    if (userErr) { sendCmd('log', { message: userErr }); return; }
    var passErr = validatePassword(cfg.adminPass);
    if (passErr) { sendCmd('log', { message: passErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

function onCreateTemplate() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    cfg.isTemplate = true;
    sendCmd('createVm', cfg);
    clearCreateForm();
    closeCreateModal();
}

/* ---- Create Sandbox modal ---- */

function openCreateModal() {
    /* Reset to defaults every time the modal opens */
    document.getElementById('vm-name').value = 'MyAppSandbox';
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    document.getElementById('hdd-size').value = 64;
    document.getElementById('gpu-mode').value = '1';
    document.getElementById('net-mode').value = '1';
    document.getElementById('admin-user').value = 'user';
    document.getElementById('admin-pass').value = 'test123';
    document.getElementById('admin-confirm').value = 'test123';
    document.getElementById('test-mode').checked = false;
    document.getElementById('ssh-enabled').checked = false;
    document.getElementById('ssh-deploy-key').checked = false;
    onSshToggle();   /* re-grey "Deploy SSH key" to match the cleared SSH checkbox */
    /* Reset OS type to Windows on each open. Valid on both hosts (a Mac host
       supports Windows via QEMU); the user can switch to macOS on a Mac. */
    document.getElementById('os-type').value = 'Windows';

    /* Smart defaults (RAM/cores) from latest host info */
    if (lastHostInfo) applySmartDefaults(lastHostInfo);

    /* Clear validation state */
    document.getElementById('vm-name-warn').textContent = '';
    document.getElementById('admin-user-warn').textContent = '';
    document.getElementById('admin-pass-warn').textContent = '';
    checkPasswordMatch();
    onNetModeChange();
    applyOsTypeUI();   /* fires updateCreateButtons + revalidateVmName */

    document.getElementById('create-vm-overlay').classList.add('active');
    setTimeout(function() { document.getElementById('vm-name').focus(); }, 0);
}

function closeCreateModal() {
    document.getElementById('create-vm-overlay').classList.remove('active');
}

/* Close on backdrop click — but only when the press also STARTED on the backdrop.
   A click targets the common ancestor of the mousedown and mouseup, so pressing
   inside the modal (e.g. selecting text in a field) and releasing on the backdrop
   would otherwise close it. */
let createBackdropPress = false;
document.getElementById('create-vm-overlay').addEventListener('mousedown', function(e) {
    createBackdropPress = (e.target === this);
});
document.getElementById('create-vm-overlay').addEventListener('click', function(e) {
    if (e.target === this && createBackdropPress) closeCreateModal();
    createBackdropPress = false;
});

/* Close on Escape */
document.addEventListener('keydown', function(e) {
    if (e.key !== 'Escape') return;
    if (document.getElementById('create-vm-overlay').classList.contains('active')) {
        closeCreateModal();
    }
});

/* ---- VM Table ---- */

/* Update the status <td> in place. Preserves the spinner element across
   updates so its CSS animation doesn't restart on every staging-file tick. */
function updateStatusCell(td, vm) {
    var needsSpinner = false;
    var label = '';
    var className = '';

    if (vm.buildingVhdx) {
        needsSpinner = true;
        label = vm.vhdxStaging ? 'Staging files... ' : 'Building Disk (' + (vm.vhdxProgress || 0) + '%) ';
        className = 'status-building';
    } else if (vm.running && vm.shuttingDown) {
        className = 'status-shutting-down';
        label = 'Shutting Down';
    } else if (vm.running && vm.isTemplate) {
        needsSpinner = true;
        label = 'Building Template ';
        className = 'status-building';
    } else if (vm.running && !vm.installComplete && !vm.isTemplate) {
        needsSpinner = true;
        var defaultLabel;
        if (vm.osType === 'macOS')      defaultLabel = 'Installing macOS ';
        else if (vm.osType === 'Linux') defaultLabel = 'Installing Linux ';
        else                            defaultLabel = 'Installing Windows ';
        label = (vm.installStatus && vm.installStatus.length > 0)
            ? (vm.installStatus + ' ')
            : defaultLabel;
        className = 'status-building';
    } else if (vm.running) {
        className = 'status-running';
        label = 'Running';
    } else {
        className = 'status-stopped';
        label = 'Stopped';
    }

    td.className = className;

    var existingSpinner = td.querySelector('.spinner');

    if (needsSpinner) {
        /* Drop any existing children except the spinner, then insert the new text
           before it. The spinner stays in the document the whole time, so its
           CSS animation clock isn't reset. */
        if (existingSpinner) {
            var child = td.firstChild;
            while (child) {
                var next = child.nextSibling;
                if (child !== existingSpinner) td.removeChild(child);
                child = next;
            }
            td.insertBefore(document.createTextNode(label), existingSpinner);
        } else {
            td.textContent = '';
            td.appendChild(document.createTextNode(label));
            var spin = document.createElement('span');
            spin.className = 'spinner';
            td.appendChild(spin);
        }
    } else {
        /* No spinner needed — wipe and set plain text. Any existing spinner is
           removed along with the old text. */
        td.textContent = label;
    }
}

/* Build the list of <td> cells for a row. The status cell is passed in and
   updated in place (rather than recreated) so the spinner animation survives. */
function buildRowCells(vm, i, statusTd) {
    updateStatusCell(statusTd, vm);

    var agentTd = document.createElement('td');
    var agentOff = !vm.running || vm.isTemplate;
    var dotClass = 'agent-dot' + (vm.agentOnline ? ' online' : '') + (agentOff ? ' disabled' : '');
    agentTd.innerHTML = '<span class="' + dotClass + '"></span>';
    agentTd.title = vm.isTemplate
        ? 'Templates do not run the in-VM agent'
        : (!vm.running
            ? 'VM is not running'
            : (vm.agentOnline
                ? 'In-VM agent is connected — host can manage the guest'
                : 'In-VM agent is not connected'));

    var bld = vm.buildingVhdx;
    var snapVal = selectedSnap[i] || 'current';

    var sshActive = vm.sshEnabled && (vm.sshState === 2 || vm.sshState === 4) && vm.running && !bld;
    var sshCell = makeIconCell('ssh', '>_', sshActive, (function(idx) { return function() { sendCmd('sshConnect', {vmIndex: idx}); }; })(i), !vm.sshEnabled ? 'hidden' : '');
    if (vm.sshEnabled) {
        var sshBtn = sshCell.querySelector('.icon-btn');
        if (vm.sshState === 1) sshBtn.title = 'Installing OpenSSH in the guest...';
        else if (vm.sshState === 4) sshBtn.title = 'Open an SSH terminal (localhost:' + vm.sshPort + '; AppSandbox key deployed — key auth works)';
        else if (vm.sshState === 2) sshBtn.title = 'Open an SSH terminal to the VM (localhost:' + vm.sshPort + ', tunneled over HvSocket)';
        else if (vm.sshState === 3) sshBtn.title = 'SSH install failed';
        else sshBtn.title = 'SSH: waiting for the in-VM agent to come online';
    }

    var cells = [
        makeCell(vm.name, i, 0),
        makeCell(vm.osType, i, 1),
        statusTd,
        agentTd,
        makeCell(vm.cpuCores, i, 4, 'Number of virtual CPU cores assigned to this VM'),
        makeCell(vm.ramMb + ' MB', i, 5, 'Memory allocated to this VM, in megabytes'),
        makeCell(vm.hddGb + ' GB', i, 6, 'Virtual disk size, in gigabytes'),
        makeCell(vm.gpuName || (vm.gpuMode === 2 ? 'Try all' : vm.gpuMode === 1 ? 'Default GPU' : 'None'), i, 7, 'GPU passed through to the VM via GPU-PV, or None'),
        makeCell(netNames[vm.networkMode] || 'None', i, 8, 'Networking mode: NAT (shared), External (bridged), Internal (host-only), or None'),
    ];
    if (!hostBridge.isMac) cells.push(makeSnapCell(vm, i));
    cells.push(
        makeIconCell('start', '\u25B6\uFE0F', !vm.running && !bld, (function(vmIdx, sv, vmObj) { return function() {
            var p = parseSnapValue(sv);
            if ((p.snapIndex >= 0 || p.snapIndex === -2) && p.branchIndex < 0) {
                /* Creating a new branch — prompt for name */
                var parentName = p.snapIndex === -2 ? 'Base' : ((vmObj.snapshots && vmObj.snapshots[p.snapIndex]) ? vmObj.snapshots[p.snapIndex].name : 'Snapshot');
                var now = new Date();
                var pad = function(n) { return n < 10 ? '0' + n : '' + n; };
                var defaultName = now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + ' ' + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
                showModal('New Branch', 'A new branch will be created from ' + parentName + '. Branches are independent working copies \u2014 changes in one branch don\u2019t affect others or modify the base snapshot.', 'Boot', {
                    confirmClass: 'primary',
                    input: { label: 'Branch name:', value: defaultName }
                }).then(function(result) {
                    if (result === false) return;
                    selectedSnap[vmIdx] = 'current';
                    sendCmd('startVm', { vmIndex: vmIdx, snapIndex: p.snapIndex, branchIndex: p.branchIndex, branchName: result });
                });
            } else {
                sendCmd('startVm', { vmIndex: vmIdx, snapIndex: p.snapIndex, branchIndex: p.branchIndex });
            }
        }; })(i, snapVal, vm), '', 'Start the VM (boots from the selected snapshot/branch)'),
        makeIconCell('connect-idd', '\uD83D\uDCFA', vm.running && !bld, function() { sendCmd('connectIddVm', {vmIndex: i}); }, '', 'Open the VM display window (IDD virtual monitor)'),
        sshCell,
        makeIconCell('shutdown', '\u23FB', vm.running && !bld, function() { sendCmd('shutdownVm', {vmIndex: i}); }, '', 'Request a graceful shutdown from the guest OS'),
        makeIconCell('stop', '\u2715\uFE0F', vm.running && !bld, function() { onStopVm(i); }, '', 'Force power off the VM immediately (may lose unsaved guest data)'),
        makeIconCell('delete', '\uD83D\uDDD1\uFE0F', !bld, function() { onDeleteVm(i); }, vm.running ? 'running' : '', 'Delete this VM and its virtual disks'),
        makeIconCell('edit', editModeRow === i ? '\u2714\uFE0F' : '\u270F\uFE0F', !vm.running && !bld, function() { toggleEditMode(i); }, '', 'Edit VM configuration (CPU, RAM, GPU, network) — VM must be stopped'),
    );
    return cells;
}

function renderVmTable() {
    var tbody = document.getElementById('vm-tbody');

    if (vms.length === 0) {
        rowCache = {};
        rowSigCache = {};
        tbody.innerHTML = '';
        var tr = document.createElement('tr');
        var td = document.createElement('td');
        td.colSpan = hostBridge.isMac ? 16 : 17;
        td.className = 'empty-state';
        var btn = document.createElement('button');
        btn.className = 'primary empty-state-btn';
        btn.textContent = '+ Create your first sandbox';
        btn.onclick = openCreateModal;
        td.appendChild(btn);
        tr.appendChild(td);
        tbody.appendChild(tr);
        return;
    }

    /* Drop cached rows for VMs that no longer exist. */
    var seen = {};
    vms.forEach(function(vm) { seen[vm.name] = true; });
    Object.keys(rowCache).forEach(function(name) {
        if (!seen[name]) {
            var stale = rowCache[name];
            if (stale.parentNode) stale.parentNode.removeChild(stale);
            delete rowCache[name];
            delete rowSigCache[name];
        }
    });

    /* Remove any non-cached tbody children (e.g. leftover empty-state row). */
    var kids = Array.prototype.slice.call(tbody.children);
    kids.forEach(function(c) {
        var cached = false;
        for (var n in rowCache) { if (rowCache[n] === c) { cached = true; break; } }
        if (!cached) tbody.removeChild(c);
    });

    /* Skip the cell rebuild when button-relevant fields are unchanged; the
     * install progress tick would otherwise destroy the button DOM mid-click. */
    vms.forEach(function(vm, i) {
        var tr = rowCache[vm.name];
        var firstBuild = !tr;
        if (!tr) {
            tr = document.createElement('tr');
            rowCache[vm.name] = tr;
        }

        var statusTd = tr.children[2] || document.createElement('td');
        updateStatusCell(statusTd, vm);

        var sig = [
            i === selectedVm, editModeRow === i,
            vm.running, vm.buildingVhdx, vm.shuttingDown, vm.agentOnline,
            vm.installComplete, vm.isTemplate,
            vm.sshEnabled, vm.sshState, vm.sshPort,
            vm.osType, vm.ramMb, vm.hddGb, vm.cpuCores,
            vm.gpuMode, vm.gpuName, vm.networkMode,
            selectedSnap[i] || 'current',
            /* Snapshot tree: take/delete/rename/branch must trigger a row rebuild
               so makeSnapCell re-runs. These fields only change on user snapshot
               actions (never on install-progress ticks — a VM can't be snapshotted
               while running), so the rebuild-skip optimization above is preserved. */
            vm.hasSnapshots, vm.snapCurrent, vm.snapCurrentBranch,
            JSON.stringify(vm.snapshots || []), JSON.stringify(vm.baseBranches || [])
        ].join('|');

        if (!firstBuild && rowSigCache[vm.name] === sig) {
            if (tbody.children[i] !== tr) {
                tbody.insertBefore(tr, tbody.children[i] || null);
            }
            return;
        }
        rowSigCache[vm.name] = sig;

        tr.className = (i === selectedVm ? 'selected ' : '') +
                       (vm.running ? 'running' : 'stopped');
        tr.onclick = function(e) {
            if (e.target.closest('.icon-btn')) return;
            if (e.target.closest('.editing')) return;
            if (e.target.closest('.snap-cell')) return;
            selectVm(i);
        };

        var cells = buildRowCells(vm, i, statusTd);

        for (var c = 0; c < cells.length; c++) {
            var newCell = cells[c];
            var oldCell = tr.children[c];
            if (oldCell === newCell) continue;
            if (oldCell) tr.replaceChild(newCell, oldCell);
            else tr.appendChild(newCell);
        }
        while (tr.children.length > cells.length) tr.removeChild(tr.lastChild);

        if (tbody.children[i] !== tr) {
            tbody.insertBefore(tr, tbody.children[i] || null);
        }
    });
}

function makeCell(text, row, col, title) {
    var td = document.createElement('td');
    td.textContent = text;
    if (title) td.title = title;

    /* Editable columns: 4=CPU, 5=RAM, 7=GPU, 8=Network */
    if (editModeRow === row && (col === 4 || col === 5 || col === 7 || col === 8)) {
        td.style.cursor = 'pointer';
        td.title = 'Click to edit';
        td.onclick = function(e) {
            e.stopPropagation();
            startInlineEdit(row, col, td);
        };
    }
    return td;
}

function makeIconCell(cls, icon, active, handler, extraClass, title) {
    var td = document.createElement('td');
    td.className = 'icon-col';
    var btn = document.createElement('button');
    btn.className = 'icon-btn ' + cls + (active ? '' : ' inactive') + (extraClass ? ' ' + extraClass : '');
    btn.textContent = icon;
    if (title) btn.title = title;
    if (active) btn.onclick = handler;
    else btn.disabled = true;
    td.appendChild(btn);
    return td;
}

/* ---- VM Selection ---- */

function selectVm(idx) {
    if (editingCell) commitInlineEdit();
    if (editModeRow >= 0 && editModeRow !== idx) editModeRow = -1;
    selectedVm = idx;
    renderVmTable();
    sendCmd('selectVm', { vmIndex: idx });
}

/* ---- Inline Editing ---- */

function toggleEditMode(row) {
    if (editingCell) commitInlineEdit();
    if (vms[row] && vms[row].running) return;
    editModeRow = (editModeRow === row) ? -1 : row;
    renderVmTable();
}

function startInlineEdit(row, col, td) {
    if (editingCell) commitInlineEdit();
    var vm = vms[row];
    if (!vm || vm.running) return;

    var oldValue;
    /* Lock cell width before swapping content to prevent column resize */
    var cellWidth = td.getBoundingClientRect().width;
    td.style.width = cellWidth + 'px';
    td.style.maxWidth = cellWidth + 'px';
    td.classList.add('editing');

    if (col === 7) {
        /* GPU combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">Default GPU</option><option value="2">Try all</option>';
        sel.value = String(vm.gpuMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else if (col === 8) {
        /* Network combo */
        var sel = document.createElement('select');
        sel.innerHTML = '<option value="0">None</option><option value="1">NAT</option><option value="2">External</option><option value="3">Internal</option>';
        sel.value = String(vm.networkMode);
        sel.onclick = function(e) { e.stopPropagation(); };
        sel.onchange = function() { commitInlineEdit(); };
        sel.onblur = function() { setTimeout(commitInlineEdit, 100); };
        td.textContent = '';
        td.appendChild(sel);
        editingCell = { row: row, col: col, element: sel };
        sel.focus();
        setTimeout(function() { try { sel.showPicker(); } catch(e) {} }, 0);
    } else {
        /* Text/number input */
        var inp = document.createElement('input');
        inp.type = 'number';
        inp.value = col === 4 ? String(vm.cpuCores) : String(vm.ramMb);
        inp.onkeydown = function(e) {
            if (e.key === 'Enter') commitInlineEdit();
            else if (e.key === 'Escape') cancelInlineEdit();
        };
        inp.onblur = function() { commitInlineEdit(); };
        td.textContent = '';
        td.appendChild(inp);
        inp.select();
        inp.focus();
        editingCell = { row: row, col: col, element: inp };
    }
}

function commitInlineEdit() {
    if (!editingCell) return;
    var el = editingCell.element;
    var row = editingCell.row;
    var col = editingCell.col;
    var value = el.value;
    editingCell = null;

    var field;
    if (col === 4) field = 'cpuCores';
    else if (col === 5) field = 'ramMb';
    else if (col === 7) field = 'gpuMode';
    else if (col === 8) field = 'networkMode';

    /* RAM must be 2 MB-aligned (HCS requirement): round an odd entry down by 1. */
    if (field === 'ramMb') {
        var mb = parseInt(value, 10);
        if (!isNaN(mb)) value = String(alignRamMb(mb));
    }

    if (field) {
        sendCmd('editVm', { vmIndex: row, field: field, value: value });
        if (field === 'networkMode' && value === '2' && currentDefaultAdapter) {
            sendCmd('editVm', { vmIndex: row, field: 'netAdapter', value: currentDefaultAdapter });
        }
    }
}

function cancelInlineEdit() {
    editingCell = null;
    renderVmTable();
}

/* ---- Force Stop VM ---- */

function onStopVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    if (vm.isTemplate) {
        showModal(
            'Cancel Template Build',
            'Stopping a template build will delete the incomplete template "' + vm.name + '".\n\nAre you sure?',
            'Stop & Delete'
        ).then(function(confirmed) {
            if (confirmed) {
                sendCmd('stopVm', { vmIndex: idx });
                sendCmd('deleteVm', { vmIndex: idx });
            }
        });
    } else {
        if (localStorage.getItem('suppress_force_stop_warn') === '1') {
            sendCmd('stopVm', { vmIndex: idx });
        } else {
            showForceStopModal(idx);
        }
    }
}

function showForceStopModal(idx) {
    document.getElementById('modal-title').textContent = 'Force Stop';
    document.getElementById('modal-message').textContent =
        'Force Stop will immediately power-off "' + vms[idx].name + '" which may result in corruption of its data.';
    document.getElementById('modal-confirm-btn').textContent = 'Force Stop';

    var cb = document.getElementById('modal-dont-show');
    if (cb) { cb.checked = false; cb.parentElement.style.display = ''; }

    document.getElementById('modal-overlay').classList.add('active');
    pendingConfirm = { resolve: function(confirmed) {
        if (confirmed) {
            if (cb && cb.checked) localStorage.setItem('suppress_force_stop_warn', '1');
            sendCmd('stopVm', { vmIndex: idx });
        }
        if (cb) cb.parentElement.style.display = 'none';
    }};
}

/* ---- Delete VM ---- */

function onDeleteVm(idx) {
    var vm = vms[idx];
    if (!vm) return;
    showModal(
        'Confirm Delete',
        'Are you sure you want to delete VM "' + vm.name + '"?\n\nThis will permanently delete all disk data and snapshots.',
        'Delete'
    ).then(function(confirmed) {
        if (confirmed) {
            sendCmd('deleteVm', { vmIndex: idx });
        }
    });
}

/* ---- Snapshots ---- */

/* Parse select value string into {snapIndex, branchIndex} */
function parseSnapValue(val) {
    if (!val || val === 'current') return {snapIndex: -1, branchIndex: -1};
    if (val === 'base') return {snapIndex: -2, branchIndex: -1};
    if (val.substring(0, 5) === 'base-') return {snapIndex: -2, branchIndex: parseInt(val.substring(5))};
    var parts = val.split('-');
    if (parts.length === 1) return {snapIndex: parseInt(parts[0]), branchIndex: -1};
    return {snapIndex: parseInt(parts[0]), branchIndex: parseInt(parts[1])};
}

function makeSnapCell(vm, vmIdx) {
    var td = document.createElement('td');
    td.className = 'snap-cell';
    var snaps = vm.snapshots || [];
    var baseBranches = vm.baseBranches || [];
    var curSnap = vm.snapCurrent;       /* -2=base, -1=pre-snapshot, >=0=snapshot index */
    var curBranch = vm.snapCurrentBranch; /* branch index or -1 */
    var hasSn = vm.hasSnapshots;
    var sel = selectedSnap[vmIdx] || 'current';

    var snapWrap = document.createElement('span');
    snapWrap.className = 'snap-wrap';

    var select = document.createElement('select');
    select.className = 'snap-select';
    select.disabled = vm.running;

    function addOpt(value, text, selected) {
        var o = document.createElement('option');
        o.value = value;
        o.textContent = text;
        if (selected) o.selected = true;
        select.appendChild(o);
    }

    if (!hasSn) {
        addOpt('current', 'No snapshots', true);
    } else {
        /*  Tree with multiple branches per node:
         *    Current (base, branch 1)
         *    \u251C Base                       <- new branch
         *    \u2502 \u251C branch 1 (date)     <- resume
         *    \u2502 \u2514 branch 2 (date)     <- resume
         *    \u251C Snapshot A (date)           <- new branch
         *    \u2502 \u2514 branch 1 (date)     <- resume
         *    \u2514 Snapshot B (date)           <- new branch
         */

        /* "Current" — resume whatever is active */
        addOpt('current', 'Current', sel === 'current');

        /* Base + its branches */
        addOpt('base', '\u251C Base [create new child branch]', sel === 'base');
        baseBranches.forEach(function(br, b) {
            var brChar = (b === baseBranches.length - 1) ? '\u2514' : '\u251C';
            var label = '\u2502\u00A0\u00A0' + brChar + ' ' + (br.name || 'branch ' + (b + 1));
            if (br.date) label += ' (' + br.date + ')';
            if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
            addOpt('base-' + b, label, sel === 'base-' + b);
        });

        /* Snapshots + their branches */
        snaps.forEach(function(snap, i) {
            var isLast = (i === snaps.length - 1);
            var treePfx = isLast ? '\u2514 ' : '\u251C ';
            var contPfx = isLast ? '\u00A0\u00A0\u00A0' : '\u2502\u00A0\u00A0';
            var branches = snap.branches || [];

            addOpt(String(i), treePfx + snap.name + ' (' + snap.date + ') [create new child branch]', sel === String(i));

            branches.forEach(function(br, b) {
                var brChar = (b === branches.length - 1) ? '\u2514' : '\u251C';
                var label = contPfx + brChar + ' ' + (br.name || 'branch ' + (b + 1));
                if (br.date) label += ' (' + br.date + ')';
                if (br.sizeGb) label += ' [' + br.sizeGb + ' GB]';
                addOpt(i + '-' + b, label, sel === i + '-' + b);
            });
        });
    }

    select.onchange = function(e) {
        e.stopPropagation();
        selectedSnap[vmIdx] = select.value;
        renderVmTable();
    };
    snapWrap.appendChild(select);

    /* Chain overlay — shows selected path when dropdown is closed */
    if (hasSn) {
        var p = parseSnapValue(sel);
        var chainText = '';
        if (sel === 'current') {
            /* Show the currently active chain */
            if (curSnap >= 0 && snaps[curSnap]) {
                chainText = 'base \u2192 ' + snaps[curSnap].name;
                if (curBranch >= 0 && snaps[curSnap].branches && snaps[curSnap].branches[curBranch])
                    chainText += ' \u2192 ' + (snaps[curSnap].branches[curBranch].name || 'branch ' + (curBranch + 1));
            } else if (curSnap === -2) {
                chainText = 'base';
                if (curBranch >= 0 && baseBranches[curBranch])
                    chainText += ' \u2192 ' + (baseBranches[curBranch].name || 'branch ' + (curBranch + 1));
            } else {
                chainText = 'base';
            }
        } else if (p.snapIndex === -2) {
            chainText = 'base';
            if (p.branchIndex >= 0 && baseBranches[p.branchIndex])
                chainText += ' \u2192 ' + (baseBranches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        } else if (p.snapIndex >= 0 && snaps[p.snapIndex]) {
            chainText = 'base \u2192 ' + snaps[p.snapIndex].name;
            if (p.branchIndex >= 0 && snaps[p.snapIndex].branches && snaps[p.snapIndex].branches[p.branchIndex])
                chainText += ' \u2192 ' + (snaps[p.snapIndex].branches[p.branchIndex].name || 'branch ' + (p.branchIndex + 1));
            else
                chainText += ' [create new child branch]';
        }
        var overlay = document.createElement('span');
        overlay.className = 'snap-overlay';
        overlay.textContent = chainText;
        snapWrap.appendChild(overlay);
    }
    td.appendChild(snapWrap);

    /* Take snapshot button — only when stopped */
    var takeBtn = document.createElement('button');
    takeBtn.className = 'snap-btn';
    takeBtn.textContent = '+';
    takeBtn.title = 'Take snapshot';
    takeBtn.disabled = vm.running;
    takeBtn.onclick = function(e) {
        e.stopPropagation();
        var defaultName = 'Snapshot ' + (snaps.length + 1);
        showModal('New Snapshot', 'Create a new snapshot of the base disk. Snapshots are frozen points in time that you can create independent branches from.', 'Create', {
            confirmClass: 'primary',
            input: { label: 'Snapshot name:', value: defaultName }
        }).then(function(result) {
            if (result === false) return;
            sendCmd('snapTake', { vmIndex: vmIdx, name: result });
        });
    };
    td.appendChild(takeBtn);

    /* Delete button — context-sensitive */
    var parsed = parseSnapValue(sel);
    if (!vm.running && parsed.snapIndex >= 0) {
        var delBtn = document.createElement('button');
        delBtn.className = 'snap-btn danger';
        delBtn.textContent = '\u2715';

        if (parsed.branchIndex >= 0) {
            /* Delete a single branch */
            delBtn.title = 'Delete branch';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Delete Branch',
                    'Delete this branch? The snapshot will be kept.',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: parsed.snapIndex, branchIndex: parsed.branchIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        } else {
            /* Delete entire snapshot + all branches */
            delBtn.title = 'Delete snapshot';
            delBtn.onclick = function(e) {
                e.stopPropagation();
                var snapName = snaps[parsed.snapIndex] ? snaps[parsed.snapIndex].name : '';
                showModal('Delete Snapshot',
                    'Delete snapshot "' + snapName + '" and all its branches?',
                    'Delete'
                ).then(function(confirmed) {
                    if (confirmed) {
                        sendCmd('snapDelete', { vmIndex: vmIdx, snapIndex: parsed.snapIndex });
                        selectedSnap[vmIdx] = 'current';
                    }
                });
            };
        }
        td.appendChild(delBtn);
    }

    /* Delete button for base branches */
    if (!vm.running && parsed.snapIndex === -2 && parsed.branchIndex >= 0) {
        var delBrBtn = document.createElement('button');
        delBrBtn.className = 'snap-btn danger';
        delBrBtn.textContent = '\u2715';
        delBrBtn.title = 'Delete base branch';
        delBrBtn.onclick = function(e) {
            e.stopPropagation();
            showModal('Delete Branch',
                'Delete this base branch?',
                'Delete'
            ).then(function(confirmed) {
                if (confirmed) {
                    sendCmd('snapDeleteBranch', { vmIndex: vmIdx, snapIndex: -2, branchIndex: parsed.branchIndex });
                    selectedSnap[vmIdx] = 'current';
                }
            });
        };
        td.appendChild(delBrBtn);
    }

    /* Rename button — when a snapshot or branch is selected */
    if (!vm.running && parsed.snapIndex !== -1) {
        var currentName = '';
        if (parsed.snapIndex === -2 && parsed.branchIndex >= 0 && baseBranches[parsed.branchIndex]) {
            currentName = baseBranches[parsed.branchIndex].name || '';
        } else if (parsed.snapIndex >= 0 && snaps[parsed.snapIndex]) {
            if (parsed.branchIndex >= 0) {
                var br = snaps[parsed.snapIndex].branches && snaps[parsed.snapIndex].branches[parsed.branchIndex];
                currentName = br ? br.name || '' : '';
            } else {
                currentName = snaps[parsed.snapIndex].name || '';
            }
        }
        if (currentName || parsed.snapIndex >= 0) {
            var renBtn = document.createElement('button');
            renBtn.className = 'snap-btn';
            renBtn.textContent = '\u270F';
            renBtn.title = 'Rename';
            renBtn.onclick = function(e) {
                e.stopPropagation();
                showModal('Rename', 'Enter a new name:', 'Rename', {
                    confirmClass: 'primary',
                    input: { label: 'Name:', value: currentName }
                }).then(function(result) {
                    if (result === false || result === currentName) return;
                    var cmd = { vmIndex: vmIdx, snapIndex: parsed.snapIndex, name: result };
                    if (parsed.branchIndex >= 0) cmd.branchIndex = parsed.branchIndex;
                    sendCmd('snapRename', cmd);
                });
            };
            td.appendChild(renBtn);
        }
    }

    return td;
}

/* ---- Log ---- */

function appendLog(msg) {
    var panel = document.getElementById('log-panel');
    var div = document.createElement('div');
    div.className = 'log-line';
    div.textContent = msg;
    panel.appendChild(div);
    panel.scrollTop = panel.scrollHeight;
}

/* ---- Prerequisite check ---- */

function onPrereqRequired() {
    document.getElementById('prereq-message').innerHTML =
        'App Sandbox requires the <strong>Virtual Machine Platform</strong> Windows feature to create and run VMs. This feature is not currently enabled.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Cancel</button>' +
        '<button class="primary" onclick="enableFeature()">Enable</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function onPrereqReboot() {
    document.getElementById('prereq-message').innerHTML =
        '<strong>Virtual Machine Platform</strong> has been enabled but a reboot is required before VMs can be created or started.';
    document.getElementById('prereq-buttons').innerHTML =
        '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
        '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
    document.getElementById('prereq-buttons').style.display = '';
    document.getElementById('prereq-overlay').classList.add('active');
}

function enableFeature() {
    document.getElementById('prereq-message').innerHTML =
        'Enabling <strong>Virtual Machine Platform</strong>. This may take a minute...' +
        '<div class="prereq-progress"><div class="prereq-progress-bar" id="prereq-bar"></div></div>' +
        '<div class="prereq-pct" id="prereq-pct">0%</div>';
    document.getElementById('prereq-buttons').style.display = 'none';
    sendCmd('enableFeature');
}

function onPrereqProgress(msg) {
    var bar = document.getElementById('prereq-bar');
    var pctEl = document.getElementById('prereq-pct');
    if (bar) bar.style.width = msg.pct + '%';
    if (pctEl) pctEl.textContent = msg.pct + '%';
}

function onPrereqResult(msg) {
    if (msg.ok && !msg.reboot) {
        document.getElementById('prereq-overlay').classList.remove('active');
    } else if (msg.ok && msg.reboot) {
        document.getElementById('prereq-message').innerHTML =
            '<strong>Virtual Machine Platform</strong> has been enabled. A reboot is required for the change to take effect.';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Later</button>' +
            '<button class="primary" onclick="sendCmd(\'enableFeatureReboot\')">Reboot Now</button>';
        document.getElementById('prereq-buttons').style.display = '';
    } else {
        document.getElementById('prereq-message').innerHTML =
            'Failed to enable <strong>Virtual Machine Platform</strong>.<br><br>' +
            'Try enabling it manually:<br>' +
            'Settings &gt; System &gt; Optional Features &gt; More Windows Features &gt; Virtual Machine Platform';
        document.getElementById('prereq-buttons').innerHTML =
            '<button onclick="document.getElementById(\'prereq-overlay\').classList.remove(\'active\')">Close</button>';
        document.getElementById('prereq-buttons').style.display = '';
    }
}

/* ---- Modal ---- */

function showModal(title, message, confirmText, opts) {
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    var confirmBtn = document.getElementById('modal-confirm-btn');
    confirmBtn.textContent = confirmText || 'Confirm';
    confirmBtn.className = (opts && opts.confirmClass) || 'danger';
    var cb = document.getElementById('modal-dont-show');
    if (cb) cb.parentElement.style.display = 'none';
    var inputRow = document.getElementById('modal-input-row');
    var inputEl = document.getElementById('modal-input');
    if (opts && opts.input) {
        inputRow.style.display = 'block';
        inputEl.value = opts.input.value || '';
        if (opts.input.label) document.getElementById('modal-input-label').textContent = opts.input.label;
        inputEl.onkeydown = function(e) { if (e.key === 'Enter') modalResolve(true); };
        inputEl.oninput = function() {
            /* Strip characters that would break the INI-style .dat file */
            var clean = inputEl.value.replace(/[\n\r\t\[\]\\]/g, '');
            if (clean !== inputEl.value) inputEl.value = clean;
        };
        inputEl.maxLength = 127;
        setTimeout(function() { inputEl.select(); inputEl.focus(); }, 50);
    } else {
        inputRow.style.display = 'none';
    }
    document.getElementById('modal-overlay').classList.add('active');

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve, hasInput: !!(opts && opts.input) };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        if (result && pendingConfirm.hasInput) {
            pendingConfirm.resolve(document.getElementById('modal-input').value);
        } else {
            pendingConfirm.resolve(result);
        }
        pendingConfirm = null;
    }
}

/* ---- Minimum size reporting ---- */

function reportMinSize() {
    var minW = 0;

    /* Measure <table> elements directly — they always report true natural width */
    var tables = document.querySelectorAll('table');
    tables.forEach(function(t) {
        if (t.scrollWidth > minW) minW = t.scrollWidth;
    });

    /* Add wrapper border (2px) + body padding (24px) */
    minW += 28;

    /* Height: sum of all sections at minimum height (log just needs ~100px) */
    var minH = 0;
    var sections = document.querySelectorAll('section');
    sections.forEach(function(s, i) {
        if (i < sections.length - 1) {
            minH += s.scrollHeight + 12;
        } else {
            minH += 100;
        }
    });
    minH += 16;

    sendCmd('setMinSize', { width: minW, height: minH });
}

/* ---- Init ---- */
/* Signal to C that the UI is ready */
sendCmd('uiReady');

/* Report min size once layout is complete (covers case with no VMs) */
setTimeout(function() {
    if (!minSizeReported) {
        minSizeReported = true;
        reportMinSize();
    }
}, 300);
