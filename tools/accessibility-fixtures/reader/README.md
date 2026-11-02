# Public accessibility tree comparisons

This reader uses macOS `AXUIElement` APIs against an application PID. Point it at
AppSandbox's viewer process to read the public proxy tree, including controls from
either guest platform. It does not read the guest transport, take screenshots, send
input, perform accessibility actions, or depend on computer use.

Build with Xcode Command Line Tools; the runner needs Python 3:

```sh
tools/accessibility-fixtures/reader/build.sh
python3 tools/accessibility-fixtures/reader/tree.py snapshot \
  --pid VIEWER_PID --output /tmp/viewer-tree.json
```

macOS must grant Accessibility access to the invoking terminal/reader. The tool
reports missing access without opening a permission prompt. Snapshots traverse
application windows and their `AXChildren`, including remote elements. Application
menus outside those windows are not included. `--window 'Exact window title'`
restricts traversal to one window. PID and node IDs are recorded; node IDs are local
to a capture and are never compared directly between processes.

## Like-for-like baseline

Run the same fixture, in the same state, natively on the host and inside the VM.
Start AppSandbox using its regular GUI or headless launch procedure, then use
`--vm` to start the guest if needed and open its production viewer through the
existing API. This also records VM identity/state and obtains the actual viewer
PID. The fixture must already be installed and running in the guest; the reader
does not install it or change its controls.

```sh
python3 tools/accessibility-fixtures/reader/tree.py pair \
  --baseline-pid NATIVE_FIXTURE_PID --vm ax-test-macos \
  --spec tools/accessibility-fixtures/reader/specs/macos-controls.json \
  --output-dir /tmp/control-comparison
```

This takes native-before, viewer, and native-after snapshots. It saves all three,
the spec, timings, hashes and `result.json`. A changed native baseline prevents a
verdict for that case. Reads are sequential, not atomic; matching endpoints cannot
exclude a transient change between them. Capture duration is not streaming latency.
The runner leaves the VM/viewer running. `--vm-timeout` bounds startup (180 seconds
by default). Display readiness does not guarantee that the guest tree or fixture
is ready: a cold-start capture can return INCOMPLETE. Each invocation records one
attempt without retrying missing controls. To compare arbitrary applications or an already-open viewer directly,
use `--candidate-pid PID` instead; `--candidate-window` can select its window.
A same-process comparison is marked `sameProcess: true` and validates only repeat
reads, never the VM proxy. VM mode rejects using the viewer itself as the baseline.

`macos-controls.json` checks 13 controls in the initial AppKit Controls tab.
`macos-subtree.json` compares the entire active tab subtree, including ordered child
edges, aliases, values, supported actions and advertised parameterized queries.
These specs are for the same AppKit fixture on both sides. A Windows guest's native
UIA tree needs its own baseline/adapter for a cross-API fidelity verdict; matching a
Mac fixture to different WPF controls does not establish that. The reader can still
capture and regression-compare Windows guest trees exposed on a Mac host. A Windows
host UIA reader is not included.

## Selecting comparisons

Specs declare exact attribute selectors and compared attributes. Use `match` for
identical selectors or separate `baseline` and `candidate` selectors when choosing
different container roots. Each must match exactly one element. `expected` checks
baseline preconditions. No labels, roles, values or child order are silently
normalized. Geometry and focus are compared only if explicitly listed because the
native and viewer windows normally occupy different coordinates/focus states.

For `subtree: true`, absent attributes are compared as native absence/error codes;
matching absence does not imply that an attribute is supported. Ordinary control
checks require readable values for every declared attribute. Element references
are remapped within the selected subtree; unresolved references are inconclusive.
Known secure fields preserve mask strings and suppress other sensitive values.
That suppression is not a plaintext-leak test, and snapshots can contain ordinary
application text: use disposable fixtures.

Replay saved captures without running either application:

```sh
python3 tools/accessibility-fixtures/reader/tree.py compare \
  --spec /tmp/control-comparison/spec.json \
  --baseline /tmp/control-comparison/baseline.json \
  --candidate /tmp/control-comparison/candidate.json \
  --baseline-after /tmp/control-comparison/baseline-after.json
```

Exit codes: **0 PASS**, **1 FAIL** (observed differences), **2 INCOMPLETE** (missing
or ambiguous targets, unreadable attributes, changed baseline, permissions or
capture limits). Differences include paths and both observed values. A PASS covers
only the declared read-only comparison; advertised actions/queries do not prove
they execute correctly. It does not qualify the chart's interaction workflows.
Reports show the first 100 differences per case and flag truncation; snapshots
retain all captured observations.

Captures default to 8,192 nodes and ten seconds; override with `--max-nodes` and
`--timeout`. AX calls have a 250 ms messaging timeout; the runner also terminates a
stuck reader after the total budget plus one second. Partial captures never pass.
Builds and reports stay outside release packaging.

Run the comparator's rejection checks with:

```sh
python3 -m unittest discover -s tools/accessibility-fixtures/reader -p 'test_*.py'
```
