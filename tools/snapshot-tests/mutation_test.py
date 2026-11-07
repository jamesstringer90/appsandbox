"""Breaks the snapshot code on purpose, one way at a time, and checks the tests notice.

A check that has never been seen failing proves nothing. Each mutant below is the current
snapshot.c with one deliberate bug. It is built and run with the test program, and has to
turn a named check red. A mutant that survives means that check cannot be trusted.

Usage: mutation_test.py <source root> <work folder> <folder the helper serves> [name ...]
"""
import csv
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# name, what is broken, scenario to run, what must turn red, [(old text, new text), ...]
MUTANTS = [
    ('fork-base-not-current',
     'a snapshot on a branch forks its parent, not the VM\'s current state (the original behaviour)',
     'take-on-snap-branch', 'content',
     [('hr = create_child_disk(branch_path, vhdx_path);',
       'hr = create_child_disk(branch_path, cur_branches ? cur_parent : vhdx_path);')]),

    ('delete-frozen-branch',
     'the frozen branch\'s file is deleted once the snapshot is committed',
     'take-on-snap-branch', 'no-loss',
     [('    /* Switch VM to the branch */\n    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);\n    return S_OK;',
       '    if (cur_branches) DeleteFileW(vhdx_path);\n    wcscpy_s(instance->vhdx_path, MAX_PATH, branch_path);\n    return S_OK;')]),

    ('rename-the-branch',
     'the branch is renamed, as PR #153 did',
     'take-on-snap-branch', 'no-rename',
     [('        wcscpy_s(vhdx_path, MAX_PATH, instance->vhdx_path);\n        if (cur_snap >= 0)',
       '        swprintf_s(vhdx_path, MAX_PATH, L"%s\\\\snapshot_%s.vhdx", tree->base_dir, snap_guid);\n'
       '        MoveFileExW(instance->vhdx_path, vhdx_path, 0);\n        if (cur_snap >= 0)')]),

    ('keep-frozen-branch-listed',
     'the frozen branch stays in the branch list, so it can be booted and written',
     'take-on-snap-branch', 'chain',
     [('        for (i = cur_branch; i < *cur_count - 1; i++)\n            cur_branches[i] = cur_branches[i + 1];\n'
       '        ZeroMemory(&cur_branches[*cur_count - 1], sizeof(BranchEntry));\n        (*cur_count)--;\n', '')]),

    ('boot-frozen-disk',
     'a VM on a frozen disk is started on it, with no branch made',
     'start-on-frozen-snap', 'immutable',
     [('    if (FAILED(hr)) return hr;\n\n    if ((tree->count > 0',
       '    if (FAILED(hr)) return hr;\n    return S_FALSE;\n\n    if ((tree->count > 0')]),

    ('write-to-frozen-disk',
     'taking a snapshot appends to the disk it is freezing',
     'take-on-snap-branch', 'immutable',
     [('    hr = snapshot_save(tree);\n    if (FAILED(hr)) {\n        tree->count--;',
       '    { FILE *x; if (_wfopen_s(&x, vhdx_path, L"ab") == 0 && x) { fputs("x", x); fclose(x); } }\n'
       '    hr = snapshot_save(tree);\n    if (FAILED(hr)) {\n        tree->count--;')]),

    ('wrong-parent-recorded',
     'tree.dat records no Parent= for a snapshot taken on another',
     'take-nested', 'chain',
     [('        if (cur_snap >= 0)\n            wcscpy_s(parent_guid, 64, tree->nodes[cur_snap].guid);\n', '')]),

    ('save-in-place',
     'tree.dat is rewritten in place, as stock does',
     'take-on-snap-branch', 'metadata',
     [('    if (!GetTempFileNameW(tree->base_dir, L"asb", 0, temporary))\n        return HRESULT_FROM_WIN32(GetLastError());\n',
       '    wcscpy_s(temporary, MAX_PATH, path);\n')]),

    ('ignore-save-failure',
     'a failed tree.dat save is reported as success',
     'take-on-snap-branch', 'honest',
     [('    hr = snapshot_save(tree);\n    if (FAILED(hr)) {\n        tree->count--;',
       '    hr = snapshot_save(tree); hr = S_OK;\n    if (FAILED(hr)) {\n        tree->count--;')]),

    ('no-rollback-in-memory',
     'after a failed save, the new snapshot stays in memory',
     'take-on-snap-branch', 'honest',
     [('    if (FAILED(hr)) {\n        tree->count--;\n        ZeroMemory(node, sizeof(SnapNode));\n',
       '    if (FAILED(hr)) {\n')]),

    ('base-kept-after-failure',
     'after a failed first snapshot, base_vhdx stays set',
     'first-snapshot', 'honest',
     [('        CopyMemory(tree->base_vhdx, saved_base, sizeof(saved_base));\n', '')]),

    ('no-rollback-of-branch',
     'after a failed save, a new branch stays in memory',
     'new-branch', 'honest',
     [('    if (FAILED(hr)) {\n        (*branch_count)--;\n        ZeroMemory(&branches[*branch_count], sizeof(BranchEntry));\n',
       '    if (FAILED(hr)) {\n')]),

    ('unreadable-tree-looks-empty',
     'a tree.dat that cannot be read is taken for an empty tree, as stock does',
     'start-on-frozen-snap', 'immutable',
     [('        if (open_error != ENOENT) tree->load_failed = TRUE;\n', '')]),

    ('stranded-on-deleted-disk',
     'a VM on a snapshot being deleted stays on its disk',
     'delete-snapshot', 'metadata',
     [('    fall_back(tree, instance, index);\n    return S_OK;\n', '    return S_OK;\n')]),

    ('stranded-on-deleted-branch',
     'a VM on a branch being deleted stays on it',
     'delete-current-branch', 'metadata',
     [('                wcscpy_s(instance->vhdx_path, MAX_PATH, branches[i].vhdx_path);\n                return S_OK;\n',
       '                return S_OK;\n')]),

    ('jump-off-an-unknown-disk',
     'a VM on a disk a damaged tree.dat does not list is moved to the base',
     'first-snapshot', 'behaviour:damaged-tree-keeps-vm',
     [('    if (instance->running) return E_NOT_VALID_STATE;\n    hr = tree_ready(tree);\n    if (FAILED(hr)) return hr;\n',
       '    if (instance->running) return E_NOT_VALID_STATE;\n    hr = tree_ready(tree);\n    if (FAILED(hr)) return hr;\n'
       '    { int s_, b_; snapshot_find_current(tree, instance->vhdx_path, &s_, &b_);\n'
       '      if (b_ < 0 && tree->base_vhdx[0]) wcscpy_s(instance->vhdx_path, MAX_PATH, tree->base_vhdx); }\n')]),

    ('delete-before-forgetting',
     'a branch\'s file is deleted before tree.dat forgets it, as stock does',
     'delete-other-branch', 'metadata',
     [('    /* As in snapshot_delete: forget the disk before deleting it */\n',
       '    DeleteFileW(saved[branch_idx].vhdx_path);\n')]),

    ('next-start-fails',
     'after any failure, the VM cannot start again',
     'take-on-snap-branch', 'usable',
     [('        DeleteFileW(branch_path);\n        if (!cur_branches) DeleteFileW(vhdx_path);\n        return hr;',
       '        DeleteFileW(instance->vhdx_path);\n        DeleteFileW(branch_path);\n        return hr;')]),

    # Behaviour tests. They run in every run, whatever the scenario.
    ('tree-left-changed-by-failed-delete',
     'after a failed delete, the branch list stays changed in memory',
     'delete-current-branch', 'honest',
     [('        CopyMemory(branches, saved, sizeof(saved));\n        *branch_count = saved_count;\n'
       '        wcscpy_s(instance->vhdx_path, MAX_PATH, saved_path);\n        return hr;\n',
       '        *branch_count = saved_count - 1;\n        wcscpy_s(instance->vhdx_path, MAX_PATH, saved_path);\n        return hr;\n')]),

    ('no-delete-guard',
     'a snapshot another one depends on can be deleted',
     'first-snapshot', 'behaviour:delete-guard',
     [('                return HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY);\n', '                continue;\n')]),

    ('tree-format-changed',
     'tree.dat is written back differently from what it read',
     'first-snapshot', 'behaviour:old-tree.dat-round-trips',
     [('fwprintf(f, L"Name=%s\\n", tree->nodes[i].name);', 'fwprintf(f, L"Name=%s \\n", tree->nodes[i].name);')]),

    ('created-not-saved',
     'a snapshot\'s creation time is not written to tree.dat',
     'first-snapshot', 'behaviour:restart-preserves-tree',
     [('            fwprintf(f, L"Created=%llu\\n", ft.QuadPart);\n', '')]),

    ('snapshot-holds-nothing',
     'a snapshot on a branch forks the base instead',
     'first-snapshot', 'behaviour:snapshots-hold-their-state',
     [('        wcscpy_s(vhdx_path, MAX_PATH, instance->vhdx_path);\n        if (cur_snap >= 0)',
       '        swprintf_s(vhdx_path, MAX_PATH, L"%s\\\\snapshot_%s.vhdx", tree->base_dir, snap_guid);\n'
       '        create_child_disk(vhdx_path, base);\n        if (cur_snap >= 0)')]),

    ('disk-not-flushed',
     'a new disk is not flushed before tree.dat names it',
     'take-on-frozen-snap', 'behaviour:durable-order',
     [('        if (!FlushFileBuffers(h)) hr = HRESULT_FROM_WIN32(GetLastError());\n', '')]),

    ('tree-not-flushed',
     'tree.dat is not flushed before it replaces the old one',
     'take-on-frozen-snap', 'behaviour:durable-order',
     [('ok = !ferror(f) && fflush(f) == 0 && _commit(_fileno(f)) == 0;', 'ok = !ferror(f) && fflush(f) == 0;')]),

    # The test program's own fence.
    ('escape-the-test-folder',
     'the code under test deletes a file outside the test folder',
     'first-snapshot', 'fence',
     [('    generate_guid_string(snap_guid, 64);\n\n    /* The snapshot must hold',
       '    DeleteFileW(L"C:\\\\asb-snapshot-tests-no-such-file");\n    generate_guid_string(snap_guid, 64);\n\n'
       '    /* The snapshot must hold')]),
]

CHECKS = ['content', 'no-loss', 'chain', 'metadata', 'immutable', 'no-rename', 'honest', 'usable']


def run_mutant(mutant, source_root, work, chains, build_cmd):
    name, what, scenario, must_fail, replacements = mutant
    src = os.path.join(work, 'mutants', name)
    out = os.path.join(work, 'build', 'mutant-' + name)
    run = os.path.join(chains, time.strftime('%Y%m%d-%H%M%S') + '-mutant-' + name)
    shutil.rmtree(src, ignore_errors=True)
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(os.path.join(source_root, 'src', 'backend_win'), os.path.join(src, 'src', 'backend_win'))
    shutil.copytree(os.path.join(source_root, 'src', 'core'), os.path.join(src, 'src', 'core'))

    path = os.path.join(src, 'src', 'backend_win', 'snapshot.c')
    with open(path, encoding='utf-8', newline='') as f:
        text = f.read()
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    for old, new in replacements:
        if text.count(old) != 1:
            return f'BAD MUTANT: found {text.count(old)} times: {old[:50]!r}'
        text = text.replace(old, new)
    if crlf:
        text = text.replace('\n', '\r\n')
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)

    built = subprocess.run(['cmd', '/c', build_cmd, src, out], capture_output=True, text=True)
    if built.returncode != 0:
        lines = [l for l in built.stdout.splitlines() if ' error ' in l]
        return 'BUILD FAILED: ' + (lines[0] if lines else built.stdout[-200:])

    exe = os.path.join(out, 'snapshot_tests.exe')
    done = subprocess.run([exe, 'run', run, 'mutant-' + name, '--filter', scenario],
                          capture_output=True, text=True, errors='replace')
    output = done.stdout + done.stderr

    if must_fail == 'fence':
        return 'caught' if 'outside the test folder' in output else f'SURVIVED (exit {done.returncode})'

    aborted = ''
    if 'FATAL' in output and not os.path.exists(os.path.join(run, 'COMPLETE')):
        aborted = 'ABORTED: ' + output[output.index('FATAL'):][:160].replace('\n', ' ')

    if must_fail.startswith('behaviour:'):
        test = must_fail.split(':', 1)[1]
        hit = [l for l in output.splitlines() if test in l and 'FAIL' in l]
        return 'caught: ' + hit[0].split(test, 1)[1].strip()[:90] if hit else (aborted or 'SURVIVED')

    # A later step aborting does not undo what the matrix has already seen.
    if not os.path.exists(os.path.join(run, 'results.tsv')):
        return aborted or 'SURVIVED: no results'
    with open(os.path.join(run, 'results.tsv'), encoding='utf-8', errors='replace', newline='') as f:
        rows = list(csv.reader(f, delimiter='\t', quoting=csv.QUOTE_NONE))[1:]
    col = 3 + CHECKS.index(must_fail)
    hits = [r for r in rows if r[col] == 'FAIL']
    if not hits:
        return aborted or 'SURVIVED'
    reason = next((w for w in hits[0][11:] if w.startswith(must_fail + ':')), '')
    return f'caught in {len(hits)}/{len(rows)} cases: {reason[len(must_fail) + 2:][:80]}'


def main():
    source_root, work, chains = sys.argv[1:4]
    only = sys.argv[4:]
    build_cmd = os.path.join(HERE, 'build-tests.cmd')
    failures = 0
    mutants = [m for m in MUTANTS if not only or m[0] in only]
    print(f'{len(mutants)} mutants. Each must turn the named check red.\n', flush=True)
    for m in mutants:
        result = run_mutant(m, source_root, work, chains, build_cmd)
        ok = result.startswith('caught')
        failures += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'} {m[0]:<30} {m[3]:<38} {result}", flush=True)
        print(f"     {m[1]}", flush=True)
    print(f'\n{len(mutants) - failures} of {len(mutants)} caught.', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
