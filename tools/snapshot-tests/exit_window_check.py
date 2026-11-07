"""A VM that has exited reads as stopped about a second before its exit callback is done with it.
A script that sees "stopped" and acts at once lands in that second. This does exactly that, over and
over, on a real guest through the headless API, and removes its VM afterwards.

    python exit_window_check.py <tools\\headless-api folder> [take | start] [linux]

With "take" it only takes snapshots, and with "start" it only starts the VM again. It uses a Windows
guest unless told "linux": the window is in the host's exit callback, whichever guest it is, and a
Windows guest needs nothing downloaded to build.
ASB_ISO_WINDOWS or ASB_ISO_LINUX names the install image, as for the headless-api tests, and it uses
whichever daemon they would use.

Run the daemon under Windows' full page heap, or a write to freed memory can pass unnoticed: see
README.md. A round counts as in the window if vms.cfg, which the exit callback saves last, had not
been replaced yet when the VM first read as stopped. That is a lower bound: vms.cfg is saved for other
reasons too, so a round marked "after the window" may still have been in it. The check fails if too
few rounds were counted, since it would then have tested nothing.
"""
import os
import sys
import time

NAME = 'brk-window'
PLAN_ALL = ('take', 'start', 'take', 'base-branch', 'delete-branch') * 6
PLAN_TAKE = ('take',) * 24
PLAN_START = ('start',) * 30
SETTLE = 2   # seconds after a snapshot change, as in the headless-api tests


def run_plan(c, name, image, config, plan, wait_progress, destroy, settle=SETTLE, os_type='Windows'):
    failures = []
    in_window = 0
    snapshots = 0
    completed = False

    def check(label, ok, detail=''):
        print(('  PASS  ' if ok else '  FAIL  ') + '[%s] %s' % (name, label) + (('   ' + str(detail)) if detail != '' else ''), flush=True)
        if not ok:
            failures.append(label)
        return ok

    def stop_and_catch():
        """Shuts the VM down and returns as soon as it reads as stopped, polling as fast as is decent.
        True if the exit callback had not finished by then, None if the shutdown never came."""
        before = os.stat(config).st_mtime_ns
        code, _ = c.shutdown(name)
        if code != 202:
            raise RuntimeError('shutdown answered %s' % code)
        deadline = time.time() + 240
        while c.status(name)['state'] != 'stopped':
            if time.time() > deadline:
                return None
            time.sleep(0.002)
        return os.stat(config).st_mtime_ns == before

    def base_branches():
        return c.snapshots_full(name).get('baseBranches', [])

    def one_round(number, op):
        nonlocal in_window, snapshots
        if not c.status(name)['running']:
            c.start(name)
        c.wait_online(name, timeout=600)
        newest = base_branches()[-1] if op == 'delete-branch' else None

        caught = stop_and_catch()
        if caught is None:
            check('round %d, %s: the guest shut down' % (number, op), False)
            c.stop(name)
            c.wait(name, 'stopped', timeout=90)
            return
        in_window += 1 if caught else 0

        label = 'round %d, %s%s' % (number, op, '' if caught else ' (after the window)')
        if op == 'take':
            result = c.snap_take(name, 'w%d' % number)
            snapshots += 1 if result[0] == 202 else 0
            check(label + ': accepted', result[0] == 202, result)
            time.sleep(settle)
            check(label + ': listed', len(c.snapshots(name)) == snapshots, len(c.snapshots(name)))
        elif op == 'start':
            result = c.start(name)
            check(label + ': accepted', result[0] in (200, 202), result)
            c.wait_online(name, timeout=600)
            check(label + ': online', True)
        elif op == 'base-branch':
            branch = 'wb%d' % number
            result = c.start(name, snap_index=-2, branch_name=branch)
            check(label + ': accepted', result[0] in (200, 202), result)
            c.wait_online(name, timeout=600)
            check(label + ': online and listed', branch in [b['name'] for b in base_branches()], base_branches())
        elif op == 'delete-branch':
            result = c.snap_delete_branch(name, -2, newest['index'])
            check(label + ': accepted', result[0] == 202, result)
            time.sleep(settle)
            check(label + ': gone', newest['name'] not in [b['name'] for b in base_branches()], base_branches())
        else:
            raise ValueError(op)

    try:
        destroy(c, name)
        code, body = c.create(name=name, osType=os_type, ramMb=4096, hddGb=64, cpuCores=2, gpuMode=1, networkMode=1,
                              testMode=True, sshEnabled=True, sshDeployKey=True, adminUser='user', adminPass='test123',
                              imagePath=image)
        check('create -> 202', code in (200, 202), '%s %s' % (code, body))
        wait_progress(c, name, lambda s: not s['building'] and s['state'] != 'installing' and (s['running'] or s['state'] == 'stopped'),
                      stall=600 if os_type == 'Linux' else 3600)
        # The guest's agent restarts seconds after an install, and a shutdown sent just then is lost.
        c.wait_online(name, timeout=600)
        time.sleep(settle * 15)

        for number, op in enumerate(plan, 1):
            try:
                one_round(number, op)
            except (TimeoutError, RuntimeError, KeyError, IndexError) as e:
                # The VM, not the daemon: note it and carry on with the next round.
                check('round %d, %s: ran' % (number, op), False, repr(e))
                if c.status(name)['running']:
                    c.stop(name)
                    c.wait(name, 'stopped', timeout=90)

        if not c.status(name)['running']:
            c.start(name)
        c.wait_online(name, timeout=600)
        check('it still boots after all rounds', True)
        completed = True
    except Exception as e:
        # Which error a dying daemon gives depends on what it was doing, so ask it.
        try:
            c.status(name)
            check('ran without an unhandled exception', False, repr(e))
        except KeyError:
            check('ran without an unhandled exception', False, repr(e))
        except Exception:
            check('the daemon answered to the end', False, repr(e))
    finally:
        try:
            destroy(c, name)
        except Exception as e:
            check('removed its VM', False, repr(e))
    print('in the window: %d of %d rounds' % (in_window, len(plan)), flush=True)
    if completed:
        check('at least half the rounds acted in the window', in_window * 2 >= len(plan), '%d of %d' % (in_window, len(plan)))
    return failures


def main():
    suite = sys.argv[1]
    plan = PLAN_TAKE if 'take' in sys.argv[2:] else PLAN_START if 'start' in sys.argv[2:] else PLAN_ALL
    os_type, image = ('Linux', 'ASB_ISO_LINUX') if 'linux' in sys.argv[2:] else ('Windows', 'ASB_ISO_WINDOWS')
    sys.path.insert(0, os.path.join(suite, 'tests'))
    sys.path.insert(0, suite)
    import asb
    import helpers
    failures = run_plan(asb.connect(), NAME, os.environ[image], os.path.join(helpers.ASB_DIR, 'vms.cfg'), plan, helpers.wait_progress, helpers.destroy_vm, os_type=os_type)
    print('SOME FAILED' if failures else 'ALL GREEN', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
