/* App Sandbox - WebView2 Frontend */
'use strict';

/* ---- State ---- */
let vms = [];
let selectedVm = -1;
let selectedSnap = -1;
let editModeRow = -1;
let editingCell = null; /* {row, col, element} */
let pendingConfirm = null; /* {resolve} */
let minSizeReported = false;

/* ---- Collapsible sections ---- */
function toggleSection(id) {
    var section = document.getElementById(id);
    var collapsed = section.classList.toggle('collapsed');
    localStorage.setItem('collapse_' + id, collapsed ? '1' : '0');
}
(function restoreCollapse() {
    var defaults = { 'config-section': '0', 'log-section': '1' };
    Object.keys(defaults).forEach(function(id) {
        var val = localStorage.getItem('collapse_' + id);
        if (val === null) val = defaults[id];
        if (val === '1') document.getElementById(id).classList.add('collapsed');
    });
})();

const netNames = ['None', 'NAT', 'External', 'Internal'];

/* ---- Message bridge ---- */

function sendCmd(action, data) {
    window.chrome.webview.postMessage(Object.assign({ action: action }, data || {}));
}

window.chrome.webview.addEventListener('message', function(event) {
    var msg = event.data;
    switch (msg.type) {
        case 'fullState':     onFullState(msg); break;
        case 'vmListChanged': vms = msg.vms; renderVmTable(); updateHostInfo(msg.hostInfo); revalidateVmName(); break;
        case 'vmStateChanged': onVmStateChanged(msg); break;
        case 'snapListChanged': renderSnapTable(msg.snapshots); break;
        case 'log':           appendLog(msg.message); break;
        case 'hostInfo':      updateHostInfo(msg); break;
        case 'browseResult':  onBrowseResult(msg.path); break;
        case 'confirmResult': if (pendingConfirm) pendingConfirm.resolve(msg.confirmed); break;
        case 'adapters':      populateAdapters(msg.adapters, msg.defaultIndex); break;
        case 'templates':     populateTemplates(msg.templates); break;
        case 'alert':         showModal('Error', msg.message, 'OK'); break;
    }
});

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
    var el;
    el = document.getElementById('host-cpu');
    if (el) el.textContent = 'Host: ' + info.hostCores + ' cores | VMs using: ' + info.vmCores;
    el = document.getElementById('host-ram');
    if (el) el.textContent = 'Host: ' + info.hostRamMb + ' MB | VMs using: ' + info.vmRamMb + ' MB';
    el = document.getElementById('host-hdd');
    if (el) el.textContent = 'Free: ' + info.freeGb + ' GB | VMs allocated: ' + info.vmHddGb + ' GB';
}

/* ---- Adapters ---- */

function populateAdapters(adapters, defaultIdx) {
    var sel = document.getElementById('net-adapter');
    sel.innerHTML = '<option value="">(Auto)</option>';
    if (adapters) {
        adapters.forEach(function(a) {
            var opt = document.createElement('option');
            opt.value = a;
            opt.textContent = a;
            sel.appendChild(opt);
        });
    }
    if (typeof defaultIdx === 'number' && defaultIdx >= 0 && defaultIdx < sel.options.length)
        sel.selectedIndex = defaultIdx;
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
    var hasImage = document.getElementById('image-path').value.trim() !== '';
    var hasTpl = document.getElementById('template-select').value !== '';
    document.getElementById('btn-create').disabled = !(hasImage || hasTpl);
    document.getElementById('btn-create-template').disabled = !hasImage;
}

/* Wire up change events */
document.getElementById('image-path').addEventListener('input', function() {
    if (this.value.trim() !== '') {
        selectTemplate('', templateDefaultLabel());
    }
    updateCreateButtons();
});

function revalidateVmName() {
    var name = document.getElementById('vm-name').value.trim();
    document.getElementById('vm-name-warn').textContent = validateVmName(name) || '';
}
document.getElementById('vm-name').addEventListener('input', revalidateVmName);
document.getElementById('admin-user').addEventListener('input', function() {
    document.getElementById('admin-user-warn').textContent = validateUsername(this.value.trim()) || '';
});

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
document.getElementById('admin-pass').addEventListener('input', checkPasswordMatch);
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
    document.getElementById('net-adapter').style.display = (mode === 2) ? '' : 'none';
}
onNetModeChange();

/* ---- Create VM ---- */

function gatherConfig() {
    return {
        name:        document.getElementById('vm-name').value.trim(),
        osType:      document.getElementById('os-type').value,
        imagePath:   document.getElementById('image-path').value.trim(),
        templateName: document.getElementById('template-select').value,
        hddGb:       parseInt(document.getElementById('hdd-size').value) || 64,
        ramMb:       parseInt(document.getElementById('ram-size').value) || 16384,
        cpuCores:    parseInt(document.getElementById('cpu-cores').value) || 8,
        gpuMode:     parseInt(document.getElementById('gpu-mode').value),
        networkMode: parseInt(document.getElementById('net-mode').value),
        netAdapter:  document.getElementById('net-adapter').value,
        adminUser:   document.getElementById('admin-user').value.trim(),
        adminPass:   document.getElementById('admin-pass').value,
        adminConfirm: document.getElementById('admin-confirm').value,
        testMode:    document.getElementById('test-mode').checked
    };
}

function clearCreateForm() {
    document.getElementById('image-path').value = '';
    selectTemplate('', templateDefaultLabel());
    updateCreateButtons();
}

function validateVmName(name) {
    if (!name) return 'VM name is required.';
    if (name.length > 15) return 'VM name cannot exceed 15 characters (NetBIOS limit).';
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

function validateUsername(name) {
    if (!name) return 'Username is required.';
    if (name.length > 20) return 'Username cannot exceed 20 characters.';
    if (/["\\/\[\]:;|=,+*?<>]/.test(name)) return 'Username contains invalid characters.';
    if (/^[.\s]+$/.test(name)) return 'Username cannot be only dots or spaces.';
    if (name.endsWith('.')) return 'Username cannot end with a period.';
    var reserved = ['CON','PRN','AUX','NUL',
        'COM1','COM2','COM3','COM4','COM5','COM6','COM7','COM8','COM9',
        'LPT1','LPT2','LPT3','LPT4','LPT5','LPT6','LPT7','LPT8','LPT9'];
    if (reserved.indexOf(name.toUpperCase()) >= 0) return 'Username is a reserved name.';
    return null;
}

function onCreateVm() {
    var cfg = gatherConfig();
    var nameErr = validateVmName(cfg.name);
    if (nameErr) { sendCmd('log', { message: nameErr }); return; }
    var userErr = validateUsername(cfg.adminUser);
    if (userErr) { sendCmd('log', { message: userErr }); return; }
    if (cfg.adminPass !== cfg.adminConfirm) {
        sendCmd('log', { message: 'Passwords do not match.' });
        return;
    }
    sendCmd('createVm', cfg);
    clearCreateForm();
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
}

/* ---- VM Table ---- */

function renderVmTable() {
    var tbody = document.getElementById('vm-tbody');
    tbody.innerHTML = '';

    if (vms.length === 0) {
        var tr = document.createElement('tr');
        var td = document.createElement('td');
        td.colSpan = 16;
        td.className = 'empty-state';
        td.textContent = 'Add a virtual machine using New VM Configuration';
        tr.appendChild(td);
        tbody.appendChild(tr);
        return;
    }

    vms.forEach(function(vm, i) {
        var tr = document.createElement('tr');
        tr.className = (i === selectedVm ? 'selected ' : '') +
                       (vm.running ? 'running' : 'stopped');
        tr.onclick = function(e) {
            if (e.target.closest('.icon-btn')) return;
            if (e.target.closest('.editing')) return;
            selectVm(i);
        };

        /* Data cells */
        tr.appendChild(makeCell(vm.name, i, 0));
        tr.appendChild(makeCell(vm.osType, i, 1));

        var statusTd = document.createElement('td');
        if (vm.buildingVhdx) {
            statusTd.className = 'status-building';
            var label = vm.vhdxStaging ? 'Staging files... ' : 'Building Disk (' + (vm.vhdxProgress || 0) + '%) ';
            statusTd.appendChild(document.createTextNode(label));
            var spin = document.createElement('span');
            spin.className = 'spinner';
            statusTd.appendChild(spin);
        } else if (vm.running && vm.shuttingDown) {
            statusTd.className = 'status-shutting-down';
            statusTd.textContent = 'Shutting Down';
        } else if (vm.running && vm.isTemplate) {
            statusTd.className = 'status-building';
            statusTd.innerHTML = 'Building Template <span class="spinner"></span>';
        } else if (vm.running && !vm.installComplete && !vm.isTemplate) {
            statusTd.className = 'status-building';
            statusTd.innerHTML = 'Installing Windows <span class="spinner"></span>';
        } else if (vm.running) {
            statusTd.className = 'status-running';
            statusTd.textContent = 'Running';
        } else {
            statusTd.className = 'status-stopped';
            statusTd.textContent = 'Stopped';
        }
        tr.appendChild(statusTd);

        var agentTd = document.createElement('td');
        agentTd.style.textAlign = 'center';
        var dotClass = 'agent-dot' + (vm.agentOnline ? ' online' : '') + (vm.isTemplate ? ' disabled' : '');
        agentTd.innerHTML = '<span class="' + dotClass + '"></span>';
        tr.appendChild(agentTd);

        tr.appendChild(makeCell(vm.cpuCores, i, 4));
        tr.appendChild(makeCell(vm.ramMb + ' MB', i, 5));
        tr.appendChild(makeCell(vm.hddGb + ' GB', i, 6));
        tr.appendChild(makeCell(vm.gpuName || (vm.gpuMode === 1 ? 'Default GPU' : 'None'), i, 7));
        tr.appendChild(makeCell(netNames[vm.networkMode] || 'None', i, 8));

        /* Action icons — disable all while VHDX is building */
        var bld = vm.buildingVhdx;
        tr.appendChild(makeIconCell('start', '\u25B6\uFE0F', !vm.running && !bld, function() { sendCmd('startVm', {vmIndex: i}); }));
        tr.appendChild(makeIconCell('connect', '\uD83D\uDDA5\uFE0F', vm.running && !bld, function() { sendCmd('connectVm', {vmIndex: i}); }));
        tr.appendChild(makeIconCell('connect-idd', '\uD83D\uDCFA', vm.running && !bld, function() { sendCmd('connectIddVm', {vmIndex: i}); }));
        tr.appendChild(makeIconCell('shutdown', '\u23FB', vm.running && !bld, function() { sendCmd('shutdownVm', {vmIndex: i}); }));
        tr.appendChild(makeIconCell('stop', '\u2715\uFE0F', vm.running && !bld, function() { onStopVm(i); }));
        tr.appendChild(makeIconCell('delete', '\uD83D\uDDD1\uFE0F', !bld, function() { onDeleteVm(i); }, vm.running ? 'running' : ''));
        tr.appendChild(makeIconCell('edit', editModeRow === i ? '\u2714\uFE0F' : '\u270F\uFE0F', !vm.running && !bld, function() { toggleEditMode(i); }));

        tbody.appendChild(tr);
    });

    /* Show/hide snap section (if present) */
    var snapSec = document.getElementById('snap-section');
    if (snapSec) snapSec.className = (selectedVm >= 0) ? 'visible' : '';
}

function makeCell(text, row, col) {
    var td = document.createElement('td');
    td.textContent = text;

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

function makeIconCell(cls, icon, active, handler, extraClass) {
    var td = document.createElement('td');
    td.className = 'icon-col';
    var btn = document.createElement('button');
    btn.className = 'icon-btn ' + cls + (active ? '' : ' inactive') + (extraClass ? ' ' + extraClass : '');
    btn.textContent = icon;
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
    selectedSnap = -1;
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
        sel.innerHTML = '<option value="0">None</option><option value="1">Default GPU</option>';
        sel.value = String(vm.gpuMode);
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

    if (field) {
        sendCmd('editVm', { vmIndex: row, field: field, value: value });
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

function renderSnapTable(snapshots) {
    var tbody = document.getElementById('snap-tbody');
    tbody.innerHTML = '';
    if (!snapshots) return;

    snapshots.forEach(function(snap, i) {
        var tr = document.createElement('tr');
        tr.className = (i === selectedSnap) ? 'selected' : '';
        tr.onclick = function() { selectedSnap = i; renderSnapTable(snapshots); };

        var td1 = document.createElement('td');
        td1.textContent = snap.name;
        tr.appendChild(td1);

        var td2 = document.createElement('td');
        td2.textContent = snap.date || '';
        tr.appendChild(td2);

        var td3 = document.createElement('td');
        td3.textContent = snap.parentVhdx || '';
        tr.appendChild(td3);

        tbody.appendChild(tr);
    });
}

function onSnapRevert() {
    if (selectedSnap < 0) return;
    sendCmd('snapRevert', { snapIndex: selectedSnap });
}

function onSnapDelete() {
    if (selectedSnap < 0) return;
    sendCmd('snapDelete', { snapIndex: selectedSnap });
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

/* ---- Modal ---- */

function showModal(title, message, confirmText) {
    document.getElementById('modal-title').textContent = title;
    document.getElementById('modal-message').textContent = message;
    document.getElementById('modal-confirm-btn').textContent = confirmText || 'Confirm';
    var cb = document.getElementById('modal-dont-show');
    if (cb) cb.parentElement.style.display = 'none';
    document.getElementById('modal-overlay').classList.add('active');

    return new Promise(function(resolve) {
        pendingConfirm = { resolve: resolve };
    });
}

function modalResolve(result) {
    document.getElementById('modal-overlay').classList.remove('active');
    if (pendingConfirm) {
        pendingConfirm.resolve(result);
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
