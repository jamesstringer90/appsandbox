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
});

/* ---- Initial state ---- */

function onFullState(msg) {
    vms = msg.vms || [];
    renderVmTable();
    revalidateVmName();
    if (msg.hostInfo) updateHostInfo(msg.hostInfo);
    if (msg.adapters) populateAdapters(msg.adapters, msg.defaultAdapter);
    if (msg.templates) populateTemplates(msg.templates);
    if (vms.length === 0 && msg.hostInfo) applySmartDefaults(msg.hostInfo);
    if (!minSizeReported) {
        minSizeReported = true;
        setTimeout(reportMinSize, 50);
    }
}

function applySmartDefaults(info) {
    var ram = Math.min(Math.floor(info.hostRamMb / 2), 16384);
    var cores = Math.min(Math.floor(info.hostCores / 2), 8);
    if (ram < 512) ram = 512;
    if (cores < 1) cores = 1;
    document.getElementById('ram-size').value = ram;
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
        testMode:    document.getElementById('test-mode').checked,
        sshEnabled:  document.getElementById('ssh-enabled').checked
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
        td.colSpan = 17;
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
            if (e.target.closest('.snap-cell')) return;
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

        /* Snapshot dropdown cell */
        tr.appendChild(makeSnapCell(vm, i));

        /* Action icons — disable all while VHDX is building */
        var bld = vm.buildingVhdx;
        var snapVal = selectedSnap[i] || 'current';
        tr.appendChild(makeIconCell('start', '\u25B6\uFE0F', !vm.running && !bld, (function(vmIdx, sv, vmObj) { return function() {
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
        }; })(i, snapVal, vm)));
        tr.appendChild(makeIconCell('connect-idd', '\uD83D\uDCFA', vm.running && !bld, function() { sendCmd('connectIddVm', {vmIndex: i}); }));
        var sshActive = vm.sshEnabled && vm.sshState === 2 && vm.running && !bld;
        var sshCell = makeIconCell('ssh', '>_', sshActive, (function(idx) { return function() { sendCmd('sshConnect', {vmIndex: idx}); }; })(i), !vm.sshEnabled ? 'hidden' : '');
        if (vm.sshEnabled) {
            var sshBtn = sshCell.querySelector('.icon-btn');
            if (vm.sshState === 1) sshBtn.title = 'Installing OpenSSH...';
            else if (vm.sshState === 2) sshBtn.title = 'SSH: localhost:' + vm.sshPort;
            else if (vm.sshState === 3) sshBtn.title = 'SSH install failed';
            else sshBtn.title = 'SSH: waiting for agent';
        }
        tr.appendChild(sshCell);
        tr.appendChild(makeIconCell('shutdown', '\u23FB', vm.running && !bld, function() { sendCmd('shutdownVm', {vmIndex: i}); }));
        tr.appendChild(makeIconCell('stop', '\u2715\uFE0F', vm.running && !bld, function() { onStopVm(i); }));
        tr.appendChild(makeIconCell('delete', '\uD83D\uDDD1\uFE0F', !bld, function() { onDeleteVm(i); }, vm.running ? 'running' : ''));
        tr.appendChild(makeIconCell('edit', editModeRow === i ? '\u2714\uFE0F' : '\u270F\uFE0F', !vm.running && !bld, function() { toggleEditMode(i); }));

        tbody.appendChild(tr);
    });
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
