# Snapshot failure-recovery tests

These test `src/backend_win/snapshot.c` on tiny real VHDX chains, with no VM. They are not part of
the build and the product carries no test code: `snapshot.c` is compiled a second time for the tests,
with `fault_hooks.h` force-included.

## Running them

```bat
tools\snapshot-tests\run.cmd
```

This needs Visual Studio and takes about a quarter of an hour. Windows asks for approval once, to start the
disk helper (see below). `run.cmd --filter take-on-snap-branch` runs a single scenario. Results are in
`out\chains\<timestamp>\`: `summary.txt`, and `results.tsv` with one line per case. A run counts only
if it has a `COMPLETE` file.

**Don't run this directly on a PC you need meanwhile, and restart after more than a run or two.** A full run
attaches and detaches thousands of disks, each one a device arrival. During a single run the Windows
taskbar stopped responding about 75 times. After about five hours of runs on one PC, the whole desktop
became unusable, including the power menu, and the PC had to be powered off. NVIDIA's display services
also gained handles with each attach and kept them until restarted, about 250,000 by then.

The run pauses while the taskbar is hung or gone, and `summary.txt` says how long it waited, but this
does not keep the desktop usable. A single scenario (`--filter`) takes a minute or two and is gentler.
Windows Sandbox refused to run the unsigned test programs when tried.

**A virtual machine avoids all this,** because the disks never reach your desktop. In a Windows 11 VM,
three versions ran in about 22 minutes, every case gave the same result as on the host, and the host's
desktop stayed normal, with no growth in NVIDIA's handle counts. The programs are statically linked, so
build them on the host with `build-helper.cmd` and `build-tests.cmd`, and copy in `asb-disk-helper.exe`,
`snapshot_tests.exe` and `source.txt`. Then, as administrator in the VM, start
`asb-disk-helper.exe <folder>` and run `snapshot_tests.exe run <folder>\<name> <label>`.

## How it works

A scenario builds a chain through the code under test, writing a marker through each layer. It then
runs one operation, such as `snapshot_take`, in a separate process.

A first run with nothing going wrong records every file operation that process performs. Each
recorded operation is then made to fail, to fail along with every operation after it (so rollback
fails too), and to be preceded and followed by killing the process. Nothing here depends on knowing
what a given version does, so the same test source builds against any version of `snapshot.c`.

A kill is `TerminateProcess`, so buffered writes are lost as they would be. Afterwards a fresh process
looks at what was left, as a restart would, and applies these checks:

| Check | What it means |
| --- | --- |
| `content` | The VM's disk, after a restart, shows exactly the markers it should |
| `no-loss` | Everything written is still reachable from a disk the metadata names |
| `chain` | Parent links in the disks match `tree.dat`, no working branch is anyone's parent, every disk attaches |
| `metadata` | Every path named exists, the VM's disk is known, no snapshot was forgotten |
| `immutable` | Frozen disks do not change, by hash, and the VM never runs on one |
| `no-rename` | No existing disk file disappeared from under its name |
| `honest` | Success means it happened; failure means nothing changed, on disk or in memory |
| `usable` | Afterwards the VM can write, and the next snapshot works |

Each scenario also runs with the VM's disk locked, with `tree.dat` read-only, and with `tree.dat`
locked when it is loaded and when it is saved. A few more tests cover behaviour when nothing goes
wrong; they are at the end of `summary.txt`.

## The disk helper

Writing a marker through a chain means attaching a disk, which needs admin rights. Nothing else here
does, so only `asb-disk-helper` runs elevated, and the code being tested never does. The helper is
small, and it stays running so that one approval covers every run, until you close its window, the PC
restarts, or it has been idle for 12 hours.

It writes to `\\.\PhysicalDriveN`, so it is careful about N. It only touches `.vhdx` files of exactly
the test size, inside the folder it was started with, checked on the final path. Every drive that
existed when it started is off limits. Before opening a drive for writing, it proves through a
read-only handle that the drive is the file-backed disk that request just attached. Attaches are never
permanent. Every run starts by seeing each of these refuse, against each real drive, through a
request type that has no way to write.

## What this does not show

- **A process kill is not a power cut.** Whether flushed data survives real power loss is reasoned,
  not measured. `durable-order` shows that the calls are made in the right order, nothing more.
- `vms.cfg` is simulated by a file holding the VM's disk path, saved atomically as `save_vm_list`
  does. `asb_core.c` is not compiled here, so its order of calls is copied in `child_main` and
  `leave_before_delete`, and has to be kept in step by hand.
- No guest boots. Whether a real VM runs on these chains needs a real VM.
- A failure injected at a call is what that call reporting failure looks like. The file system
  misbehaving in other ways is not covered.

## The second after a VM stops

`asb_core.c` is not compiled into the tests above, and an early version of this change had a bug there
that they could not see. A VM that has exited reads as stopped about a second before its exit callback
is done with it, and that version closed the compute system before a snapshot, from the API thread, in
that second. Two threads then closed one compute system, and the object was freed under one of them.

`exit_window_check.py` does what a script would: it shuts a real guest down, polls until it reads as
stopped, and acts at once. It repeats that with snapshots, starts, new base branches and branch deletes,
and fails if too few rounds landed in the window. `take` or `start` after the folder runs only that.

```bat
python tools\snapshot-tests\exit_window_check.py tools\headless-api
```

It needs a running daemon and an image in `ASB_ISO_WINDOWS` (or, with `linux` on the command line, in
`ASB_ISO_LINUX`), and takes about a quarter of an hour. A write
to freed memory usually goes unnoticed for a long time, so run the daemon under Windows' full page heap,
under a file name of its own so nothing else is affected: copy the program to `AppSandboxT.exe`, and set
`GlobalFlag=0x02000000` and `PageHeapFlags=3` (both DWORD) under `HKLM\SOFTWARE\Microsoft\Windows
NT\CurrentVersion\Image File Execution Options\AppSandboxT.exe`. Remove that key afterwards.

## Comparing versions, and testing the tests

`build-tests.cmd <source root> <output folder>` builds the tests against another checkout, which is
how to see what a different version of `snapshot.c` does.

`mutation_test.py` breaks `snapshot.c` on purpose, one way at a time, and checks that a named check
turns red. A check that has never been seen failing proves nothing. It takes about an hour.
