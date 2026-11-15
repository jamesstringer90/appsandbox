# Accessibility testing handoff

Branch: `mac-accessibility`. Status: work in progress, not release qualification.
This handoff carries the native tester and the accessibility implementation changes
used during its development. No VM images, credentials, signing overrides, built
apps, or captured trees are included. VM names and paths in commands are examples.

## What is implemented

- `build/ax-suite`: Objective-C runner, headless API client, typed result comparison,
  SDK inventory and coverage reporting. No Python is used by this tester.
- `build/ax-probe`: persistent reader using public host AX APIs. It performs reads,
  actions, setters, parameterized queries and notification observation.
- `ASBAXConformance.app`: AppKit/WebKit fixture with dropdown pages, deterministic
  resets and observable state. There are 441 conformance cases and seven stress
  workloads. Provider-contract exercises are distinct from native widget behavior.
- `paired-window.m`: adds the native fixture beside the actual VM viewer in a
  separately generated test build. The production Xcode project and release
  packaging do not include the fixture.
- `reader/tree`: native replacement for the earlier Python tree comparator. Build
  it using `tools/accessibility-fixtures/reader/build.sh`; the older reader README's
  Python commands are superseded by this entry point.
- `web/asb-loading.html`: standalone browser fixture for gradual loading and
  repeated subtree replacement, supplementary to the automated WebKit workloads.

VM lifecycle and display control go through the existing headless API. SSH is used
by `prepare` to copy and launch the disposable fixture using API-discovered guest
information. Candidate observations and actions go through the production viewer's
public AX interface, never through SSH or a guest transport dump. The harness does
not control or inspect QEMU.

The accompanying implementation work preserves AX boolean/error fidelity, extends
Windows-to-AX selection and masked-value translation, retains usable snapshots
during updates, and adds incremental publication to both guest collectors. These
changes still require the live qualification described below.

## Set up the new Mac

Use an Apple silicon Mac with macOS 27 or newer and a full Xcode installation.
The checked-in SDK inventory was generated from macOS SDK 27.2. Select the intended
Xcode with `xcode-select`; do not silently replace the inventory to hide SDK drift.

Configure signing through the existing ignored
`src/app_mac/xcconfig/LocalUser.xcconfig`, following
[the signing instructions](../../sign/SIGNING_MAC.md). The paired build requires a
real signing identity and the same team for the executable and embedded framework.
Its verifier rejects ad-hoc host signing and mismatched teams. Do not re-sign just
the outer executable; that previously caused a dyld launch failure.

From the checkout root, as the regular desktop user:

```sh
git switch mac-accessibility
git pull --ff-only origin mac-accessibility
sh tools/accessibility-fixtures/conformance/build.sh
sh tools/accessibility-fixtures/reader/build.sh

suite="$PWD/tools/accessibility-fixtures/conformance/build/ax-suite"
work="$HOME/Library/Caches/AppSandbox-AX"
mkdir -p "$work"
host_build="$work/host-$(date +%Y%m%d-%H%M%S)"
"$suite" build-host --output "$host_build"
app="$host_build/Products/Release/AppSandbox.app"
```

`build-host` builds the existing product target with the fixture sources added to
a generated project. It checks signatures and performs an unprivileged product
launch that must report that elevation is required. Use a new output directory
for each rebuild; never overwrite a running app. The built paired app records its
source checkout path, so rebuild it on the new Mac rather than transferring an
old app bundle.

Run only one AppSandbox host. If another host owns test VMs, shut down those VMs
through the existing API before stopping that host. On the new test machine, launch
the newly built app from a local Terminal in the logged-in desktop session:

```sh
sudo "$app/Contents/MacOS/AppSandbox" --headless
```

Leave that Terminal running; closing it can terminate the host and its VMs. Use a
second Terminal for the commands below. Set `suite` and `work` there to the same
paths. Starting a host from an SSH-only session does not provide the required
AppKit viewer session.

Create a dedicated macOS test VM through the existing headless API, using
`Client.create` and `wait_online` documented in the
[headless API reference](../../headless-api/README.md). Use a macOS 27+ restore image,
enable SSH, and supply credentials locally. This is the existing headless SDK's
permitted Python interface, not a second tester or VM control implementation.
Do not run the full-lifecycle example unchanged: it includes VM deletion.
Provision a fresh VM from this build so the normal staging/firstboot system installs
its bundled guest helper; `prepare` only installs fixtures, not agent updates.

In the guest, sign in and grant Accessibility access to **AppSandbox Accessibility**
at `/Library/AppSandbox/AppSandboxAccessibility.app`. On the host, grant the AX
reader/its responsible application Accessibility access when required by macOS.
Do not copy permission databases or bypass consent. Keep both sessions unlocked,
prevent guest sleep/automatic locking for the test session, and keep the guest
fixture foreground. A locked guest is not a proxy compatibility failure.

```sh
vm="ax-test-macos"
"$suite" api --path /version
"$suite" api --path /vms
"$suite" start-viewer --vm "$vm"
"$suite" prepare --vm "$vm"
host_pid=$(/usr/bin/plutil -extract pid raw -o - \
  "$HOME/Library/Application Support/AppSandbox/host.json")
```

Use your own VM name. `start-viewer` starts the existing VM if needed and opens its
display. The paired native pane is attached when the viewer first becomes key;
bring it forward once if it has not attached. It can then remain behind other
host windows. Avoid minimizing or closing the viewer during ordinary comparisons.
The runner does not activate host windows; focus-dependent tests are opt-in.

## Run and iterate

Each output directory must be new. Use the paired host PID, not a guest process PID.

```sh
run="$work/run-$(date +%Y%m%d-%H%M%S)"
"$suite" run --vm "$vm" --paired-pid "$host_pid" \
  --window "$vm — Display" --output "$run"
"$suite" coverage --run "$run" --output "$work/coverage.json"

"$suite" run --vm "$vm" --paired-pid "$host_pid" \
  --cases 'text.*,web.*' --output "$work/targeted-$(date +%Y%m%d-%H%M%S)"
"$suite" run --vm "$vm" --paired-pid "$host_pid" \
  --failed-from "$run" --repeat 3 \
  --output "$work/repeat-$(date +%Y%m%d-%H%M%S)"

"$suite" stress --vm "$vm" --paired-pid "$host_pid" \
  --duration 20 --output "$work/stress-$(date +%Y%m%d-%H%M%S)"
"$suite" stress --vm "$vm" --paired-pid "$host_pid" \
  --cases 'slow-*' --duration 20 \
  --output "$work/slow-$(date +%Y%m%d-%H%M%S)"
```

Use `list` to inspect case IDs. `--native-only` checks the native fixture and reports
`NATIVE_OK`, never a proxy pass. Add `--include-focus` only in a dedicated focus
session. `catalog --check` compares the installed SDK with the committed inventory;
a mismatch requires reviewing the SDK differences, not overwriting the catalog
and calling the existing cases complete.

For fixture iteration without restarting the host, build the changed fixture, run
`prepare --vm "$vm" --local`, and use its standalone native process PID with
`--native-pid PID --proxy-under asb-remote-pane` instead of `--paired-pid`. Both
fixtures must match the current source revision. Rebuild/restart the paired host
when changing its embedded fixture or production host code. Provision through the
normal product path when changing guest helper binaries.

The UI's Run/Failed/Stop controls execute the same runner. Source changes to the
fixture, cases, reader or runner invalidate old coverage evidence. Never replace
an incomplete traversal or failed baseline with a passing cached control read.

## Continuous-update acceptance and remaining work

The stress matrix includes value updates, subtree replacement, 1,024-node updates
at a requested 60 Hz, deliberately slow attribute providers, WebKit replacement,
and simultaneous native/browser trees. Actual producer rates are recorded because
background WebKit timers may be throttled.

Cached public AX reads and ping actions run alongside independent fresh traversals.
Control lookups exclude the changing forest; the independent traversals do not.
Fresh traversals must find advancing deep markers while updates are still running.
The current thresholds are two seconds for state progress/read/action latency and
five seconds between successful fresh traversals. Cold-start cases begin updating
before discovery. Failed native baselines are `BASELINE_FAILED`, not proxy passes.
Fixtures have a bounded auto-stop limit if the reader exits.

Earlier standard testing recorded 358 PASS, 56 FAIL, 17 BASELINE_MISMATCH,
5 INCOMPLETE, 2 native unsupported/no-value results and 3 REQUIRES_FOCUS across
441 cases in about 98.5 seconds. These are historical observations recorded in the
[support chart](../../../docs/accessibility-validation/SUPPORT.md), not current
qualification or expected results for every OS version. Raw captures are not
included in this public handoff.

An earlier stress run found zero complete fresh traversals during a 20-second slow
capture despite responsive cached reads/actions. That version used a shallow
marker. The strengthened deep-marker/control-isolation version, including its
browser marker, has compiled but has not had a live VM rerun. Reproduce it first.

Next work, in order:

1. Build and launch this exact product/fixture revision on the new Mac, provision
   the guest normally, and establish a fresh native/proxy baseline.
2. Run all seven stress workloads and inspect raw cold-traversal errors. Distinguish
   node replacement during a strict walk from an unavailable or stalled service.
3. Investigate whether repeated actions/refresh requests cancel long captures before
   deep nodes publish. Trace `handleAction`, refresh handling and generation checks
   in `tools/agent_mac/appsandbox-accessibility.m`, then session/presenter lifecycle.
   This is a suspected cause, not an established diagnosis.
4. Rerun the standard cases, especially notifications, invalid child references,
   custom rotors, secure metadata, browser position queries and error fidelity.
   Schedule focus cases separately.
5. Add the expanded native Windows UIA baseline and equivalent fixture behavior.
   They are not implemented in this conformance runner. Existing Windows fixtures
   do not establish Windows-to-Windows proxy support or Hyper-V host qualification.

The inventory has 1,597 SDK declarations, including types/constants; assigned cases
are not proof of complete AX coverage. Do not present this as testing every feature.
The branch has not been merged with the newest `main` during this handoff; perform
a deliberate integration review before adopting newer main changes.

## Verification and public-data boundaries

For this handoff, the native suite, public AX reader and fixture were compiled in
an isolated directory. Changed macOS production sources passed syntax checks.
No GUI app, host, VM or live AX test was launched or restarted on the occupied Mac.
The full signed product build and Windows compilation/runtime checks remain for
the next machine's test cycle.

Keep results under the local cache directory above. Reports can contain process
paths, VM identifiers and application text. Do not commit reports, screenshots,
discovery files, SSH keys, signing files, guest disks or real credentials. Only
reviewed, anonymized aggregate results belong in the public support chart.
