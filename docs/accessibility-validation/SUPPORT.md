# VM accessibility support

**Canonical support chart for macOS and Windows guests.** Reference this file for support claims, known failures and testing priorities. Update this same file after relevant testing, attribution or implementation changes.

Last updated: **2026-11-03 UTC**. Branch: `mac-accessibility`. Latest production workflow run: signed Release build from `d411c3b` plus local fixes. Earlier scoped workflow test: `f23d89fe0872686e4b74533e5a27596dfafbbc9c`; read-only tree and viewer lifecycle test: `e930e98`. Earlier broad source audit: `a0d0960a61f653804233b812de2b451ca199ffc6`.

**Release qualification: NOT VALIDATED on either platform.** Common interactions have observed passes, but complete accessibility support and Mac/Windows parity are not established. Small trees can also fail. There is no defensible percentage of all realistic application support.

The capability chart is the current assessment. The workflow table preserves the recorded results from the 20261029 run, whose observations extend into October 30. Later follow-ups refine scope and attribution without rewriting those results. No new live testing was performed to initialize this chart. The 17 capability groups, 51 workflows and 45 planned conformance families are different inventories and must not be combined into a pass percentage.

Both guest columns describe tests through a **macOS host**. **Windows-to-Windows is not validated**; the Windows host UIA proxy is separate work. The [test fixtures](../../tools/accessibility-fixtures/README.md) are available for that testing.

This chart and the [public tree reader](../../tools/accessibility-fixtures/reader/README.md) are tracked. Historical raw captures, diagnostic harnesses and local reports are not published. Evidence references below identify those local artifacts without linking to absent repository files.

## Public tree reader comparison, 2026-11-02 UTC

The initial reader run used the signed `c31a32b` host build, the real `ax-test-macos` VM
started through the headless API, and that build's guest helper installed by the
existing firstboot script under `com.appsandbox.accessibility`. The baseline ran
the exact fixture binary copied from the guest. Host macOS was 27.2; guest macOS
was 26.5.2, so provider-version differences remain possible. Reads used the host's
public AX tree, not a guest socket dump.

| Check | Direct reader result | Relationship to the computer-use audit |
|---|---|---|
| 13 selected control/property checks | 10 matched; button, disabled button and checkbox differed only in the compared `AXEnabled` type: native boolean versus proxy integer 0/1. | Earlier raw probes already contain this difference. Their state checker accepted boolean and 0/1 representations; the saved client tree correctly marked **Unavailable** disabled. Historical functional outcomes are unchanged. |
| Active tab subtree | All 36 nodes and ordered child edges matched. Strict attribute comparison differed in enabled representation/availability and unsupported-versus-no-value errors. | Exposes details omitted by the client tree; does not establish an interaction failure. |
| Observation time | Viewer reads took 67–75 ms; each paired comparison completed in under 300 ms. | Two saved combined computer-use captures took 84 ms and 453 ms. Different runs and scope; no controlled speedup claim. Automatic comparison removes manual review work. |

The signed `e930e98` headless merge and its bundled guest helper reproduced the
same 10 matches and three type differences using `--vm ax-test-macos`. After the user
reported possible interruption, a passive-monitor repeat recorded no input events,
held input or counter changes. Viewer reads took 75–77 ms. Focusing the existing
viewer kept the same presenter; closing stopped it; reopening created a new
presenter and restored the identical 36-node subtree, first observed at 3.38 seconds.
Earlier timing observations are retained as potentially interrupted. Cold-start
and helper-installation captures with unavailable trees remain INCOMPLETE; no
automatic retry promoted them to passes.

These are initial-state read-only comparisons, not repeats of the 51 workflows.
Supported actions/queries are metadata, not proof of successful execution.
Local captures and provenance: `docs/accessibility-validation/runs/20261102-public-tree-reader/`.

## Proxy fidelity follow-up, 2026-11-02 UTC

Local changes preserve boolean types, native attribute absence/errors, and
parameterized-query errors. The signed Release bundle rebuilt both changed helpers;
the installed Mac guest helper matched the bundled binary. Tests used the real VMs,
headless viewer lifecycle, and public host AX. Independent native guest reads used
SSH only for fixture setup and baseline observation.

| Check | Result |
|---|---|
| Mac Controls page | All 13 control checks PASS; strict 36-node subtree comparison PASS. |
| Mac Lists and outline page | Strict 509-node subtree comparison PASS. |
| Mac text queries | Six valid results, three invalid-range errors (`-25201`), and one unsupported-query error (`-25213`) match the native baseline. |
| Mac Text editing page | All compared values and structure match across 12 nodes; strict comparison still differs because secure-field `AXBoundsForRange` is filtered. The reader now recognizes AppKit's U+F79A password mask; native and proxy both returned 11 masks. |
| Windows guest on Mac host | Five controls match native UIA enabled/range reads; one button press updates its guest status; three table-cell lookups match native cell names and counts. |

This earlier follow-up found Windows WPF out-of-range cell queries returning null
natively but `cannotComplete` publicly. The expanded run below fixes that translation. A negative index
returns `illegalArgument` publicly. One Windows tab-selection request timed out
although the page changed; it is not counted as a passing action. The original
browser-table error case and Windows-host path were not rerun. These results do
not qualify the full interaction suite or performance.

Local evidence: `docs/accessibility-validation/runs/20261102-proxy-fidelity/`.

## Expanded native, browser and stress run, 2026-11-02 UTC

All **51 workflows on each guest (102 platform cases)** were attempted through the
public AX API of the real headless-launched viewers. Both VMs used their normally
installed helpers and production transports. SSH served fixture setup and independent
native AX/UIA baselines. No computer-use client or guest socket dump supplied candidate
results. The complete 306-slot, three-repetition qualification plan was **not** run.

| Inventory | macOS guest | Windows guest |
|---|---:|---:|
| Native controls and text | 22/22 attempted | 22/22 attempted |
| HTML workflows | 16/16 attempted | 16/16 attempted |
| Extra native/browser/folder cases | 8/8 attempted | 8/8 attempted |
| Large-tree, recovery and timing cases | 5/5 attempted | 5/5 attempted |

The signed Release build was restarted before testing the two new Mac-host adapter
fixes. First selection in Windows text fields now uses the provider's selection
capability, even when its current selection is empty. Three fresh Edge trials passed
selection of `quiet morning` and exact replacement with `calm afternoon`; independent
native UIA confirmed the first selection before typing. One trial's broader initial
tree capture was incomplete, so these are not three clean full-workflow passes.
Native null grid cells now return `noValue`: three valid WPF cells, two out-of-range
null results and the existing negative-index rejection passed through the viewer.

| Follow-up | Observed result |
|---|---|
| HTML table relationships | Both platforms match all nine cells, counts, indices, spans, supported headers, traversal/query identities and invalid-coordinate errors. Windows focus/scroll/selection side-effect checks pass; Mac global focus and ancestor scroll remain unmeasured. |
| Text and lists | Both native text chains produce the expected replacement and queried bold/italic spans. Both HTML multiselect cases select items 23 and 24. Windows native row/outline reveal and selection work; native keyboard multiselection remains unsuccessful. |
| Large trees | Both expose all 1,000 unique targets and activate target 1,000. The first Mac post-action read remained updating; later public and native reads agree on `STRESS RESULT 01000`. This is not latency qualification. |
| Helper pause/recovery | Both resume the same helper after a confirmed 20-second pause, retain Meal=0 without a late toggle, and accept a fresh 0→1 action. Mac's retained request returns at about 5.00 seconds; Windows rejects earlier, around 1.04 seconds. Both errors surface as generic `illegalArgument`; error fidelity and broader races remain unqualified. |
| Timing | Three matched toggle sequences per guest. Mac public actions take about 105 ms versus 102–106 ms natively; Windows public actions take 35–38 ms versus 2.6–3.2 ms natively. These are API-call timings, not CPU/60 Hz measurements or a declared performance pass. |

Direct native tests reproduce Safari's unfocused title/search setters reporting success
without changing values, Edge's `Go` setter producing no suggestion popup, and dialog
focus returning to the opener instead of Travelers on both browsers. AppKit also
rejects the explicit row-reveal action; the proxy incorrectly changes that error from
`actionUnsupported` to `illegalArgument`. The original date input remains inconclusive
on the Mac locale; Windows passes after the driver replaces the existing date text.

This run found missing Windows native secure-mask readback. It also found seven
Color-dialog panes whose enabled flags differ from the .NET UIA baseline. A later
native UIA8 comparison—the API used by the helper—reports all seven enabled, matching
the proxy. The enabled-state discrepancy is an API/provider difference, not a proven
proxy defect. Earlier modal-owner and application
focus gaps remain open. Browser popup dismissal and native multiselection retain their
recorded limitations; this run does not establish complete compatibility.

Failed launches, incomplete reads, premature update checks and incorrect synthetic
modifier events remain in the evidence. Corrected repetitions and separate replays
identify those driver failures without erasing them. The runner's PASS flag covers its
implemented assertions only; it is not an original-suite or release verdict. Full tree
captures also expose native getter failures, including Finder errors mirrored correctly
by the proxy, even when folder navigation succeeds.

Release build, strict bundle signature verification, nine reader tests and focused
adapter checks pass. The first-selection regression check fails against the previous
source. Changes remain local; nothing was committed or pushed. Local evidence and
per-case attempts: `docs/accessibility-validation/runs/20261102-browser-expanded/`.

## Compatibility fixes follow-up, 2026-11-02 UTC

The rebuilt headless host and installed Windows helper were tested through the public
AX trees of the actual viewers. The helper was built on Windows with its existing
MSBuild project for ARM64 and x64; the ARM64 test binary runs from
`C:\Windows\AppSandbox` under the existing agent watchdog. This local test binary is
unsigned; signed release packaging and Windows-host execution remain unqualified.
No build or deployment code changed.

| Change | Production evidence |
|---|---|
| Windows secure TextPattern masks | WPF and Edge now match independent native UIA8 reads at 11 masks, 9 masks, empty, and four UTF-16 mask units after Unicode input. Non-mask TextPattern output is rejected; existing masked ValuePattern values retain precedence. Secure range queries and selection/character metadata remain unavailable. |
| Action and write errors | Mac unsupported row reveal now returns native `actionUnsupported`. Paused-helper action and writable-slider requests return `cannotComplete` at about five seconds, with no late mutation after recovery; fresh requests then work. Windows rejects its retained request with `cannotComplete` at about 0.67 seconds and recovers without a late toggle; that earlier rejection does not exercise the full five-second timeout. |
| Proxy-root focus relationship | The root now exposes `AXFocusedUIElement`, returning the focused native field on both guests. Host application-wide lookup still returns `noValue`; I12 remains open. |
| Regressions | Native AppKit and Safari secure fields pass the same four value states and query restrictions. Windows browser nonsecure selection/replacement passes. |

Safari rejected the fixture file URL after reboot, so its repeat used the identical
HTML bytes served over guest loopback HTTP. Candidate accessibility still used the
production transport. The Windows browser retained its file URL. Browser-origin and
binary provenance are saved with the results.

The earlier Color-dialog enabled-state claim was corrected: native UIA8 agrees with
the proxy on all seven panes; .NET UIA disagrees. Failed login/setup, early-read and
baseline-selection attempts are retained separately from successful repeats.

Signed Mac Release build and strict signature verification pass, as do both Windows
builds, 19 adapter checks and 11 presenter checks. Local evidence:
`docs/accessibility-validation/runs/20261102-compatibility-fixes/`.

## Modal ownership and resize follow-up, 2026-11-02–03 UTC

The Windows collector now follows explicit HWND ownership and traverses owner trees
before auxiliary windows. The normal guest-agent watchdog restarted its ARM64 build;
the existing project builds successfully for both ARM64 and x64. The local Windows test
binary is unsigned; signed release packaging and Windows-host execution remain unqualified.

The presenter now preserves its cache while geometry is temporarily incompatible and
accepts snapshots received during that interval. A cache-only live repeat still lost
the Safari field. The Mac guest helper now also observes display-configuration changes
and schedules a refresh through the existing event path. No polling loop was added.
The signed Release host was restarted; the rebuilt guest helper runs under its normal
LaunchAgent. Candidate results below come from public AX on the actual viewers.

| Check | Result |
|---|---|
| WPF confirmation dialog | All 80 native/public nodes match labels, mapped roles, enabled states and ordered children. Confirm, reopen and Cancel pass. |
| Win32 Color dialog | The same structural comparison matches all 98 nodes. Cancel, subsequent checkbox toggles and the disabled-control flag pass. |
| Mac Safari resize | Three alternating wide/right-half transitions recover without keyboard input or manual refresh. All 306 page nodes match native AX in seven compared attributes and ordered children, with element references normalized by tree path. Secure-field edits pass after each transition. |
| Windows Edge resize | The same three transitions preserve secure masks matching native UIA8; edits pass afterward. This compares secure values, not the complete translated page. |
| Viewer lifecycle | Both guests pass minimize/restore, headless close/reopen and a fresh edit after reopening. The closed viewer is absent from public AX. |
| Cache regression checks | Ten checks pass, including restoration without retransmission, a snapshot arriving while hidden, and clearing after failure/disconnect. The previous source fails restoration. |

Mac resize recovery was first observed at 4.75–5.78 seconds after the frame-setting
commands, with temporary tree gaps; Windows' first reads completed at 0.61–0.62 seconds.
Complete post-reopen reads arrived at 11.05 seconds for Mac and 2.51 seconds for Windows.
These observations satisfy the declared 30-second observation bound, not a latency
qualification. The Mac reopen also had an earlier incomplete read, retained in evidence.

Startup-focus, initial Safari page omissions, early reads and corrected comparison
matchers remain recorded separately. Fresh-open Safari omission remains unresolved
(I15). Authentication used computer use only; candidate tests used the helper reader.
This is targeted verification, not full-suite or release qualification. Local evidence:
`docs/accessibility-validation/runs/20261102-compatibility-next/`.

## Large-page streaming follow-up, 2026-11-03 UTC

The current local changes publish partial trees during collection, retain readable
content during refresh, and coalesce background changes without restarting an
unfinished capture. Mac attribute writability is queried on demand. Tests use the
real headless-launched VM, installed helper, production transport and public host
AX reader; native reads provide independent baselines. One VM runs at a time.

| Current Mac build check | Observed result |
|---|---|
| Wikipedia fresh navigation | Heading and search field first observed after 3.09–3.91 seconds, including navigation and reader time; 3.16 seconds with the final presenter. |
| Wikipedia complete page | All 18,770 page nodes match native AX for role, title, description, value, busy state and ordered children, including a repeat with the final presenter. An earlier comparison during streaming exposes 17,726 nodes with busy=true; that partial result is retained. |
| Wikipedia actions | Light→Dark→Light succeeds; changed values observed after 3.70–3.76 seconds. Calls themselves take 1.21–1.30 seconds; full-tree target lookup adds time. |
| Continuously changing page | All 3,000 final targets exposed; first and last button actions succeed. Settled page matches the native tree for the same five fields and ordered children. Safari menu references can expire during a read; these incomplete reads are retained separately. |
| Host responsiveness | During the final changing-page run, all 499 headless status requests succeed; maximum response time 25.4 ms. This measures API responsiveness, not CPU overhead. |
| Small-tree regressions | Button, checkbox, slider, sheet, Unicode title and secure-value checks pass. Text replacement initially reads the cached value; a measured follow-up observes the update after 0.82 seconds and matches native. Formatting action/query succeeds; exact formatting spans were not requalified. |

Windows builds pass ARM64 and x64 through the existing project with warnings as
errors. The current helper batches child property reads, skips vanished UIA nodes,
retains published branches across failed captures and accepts actions on published
partial trees. Focused-control metadata is refreshed before partial publication,
and text-selection validation uses the text that was published. The following
checks use the installed helper over the production transport; diagnostic collector
runs are excluded.

| Current Windows build check | Observed result |
|---|---|
| Continuously changing page | Page remains exposed in all 30 observations over 40 seconds after setup. A button press during loading succeeds. All 3,000 targets appear after pausing; the last target activates. All 3,009 page nodes match native UIA names, child order and button roles. |
| Change latency under heavy churn | The sampled status advances throughout loading, then about every 8–9 seconds during continuous replacement. Paused content converges in 7.46 seconds. These are observation timings, not an instantaneous-tree guarantee. |
| Viewer close/reopen during changes | Usable controls return in 0.89 seconds; the next counted press appears after 0.84 seconds. |
| Wikipedia fresh navigation | Heading and Search link appear after 3.11 seconds on the final helper (2.54 seconds on the preceding helper). The original search-field matcher never passes because this responsive layout collapses the field; its raw observations are retained. |
| Wikipedia complete page | All 11,429 page nodes match native UIA names, child order and button roles. The final complete public-tree walk takes 9.91 seconds; an earlier complete read of the partial tree exposes 1,558 nodes in 1.46 seconds. |
| Wikipedia selected-state update | **Fixed in nine current-helper transitions:** all actions and selected readbacks succeed. First readback takes 3.98 seconds; subsequent eight take 0.75–0.83 seconds. Earlier stale values persist for at least 49 seconds despite native selected=true; diagnostic traces attribute these to stale properties in the guest partial snapshot. Those failures and host-lock interruptions remain recorded. |
| Host responsiveness | All 614 headless status requests succeed during the final changing-page run; p95 1.98 ms, maximum 22.77 ms. This measures API responsiveness, not CPU overhead. |

Seven Windows small-control workflows pass directly: button, checkbox, slider,
Unicode title, secure value, text selection/replacement and formatting action/query.
Exact formatting spans are not requalified. The sheet workflow's first read catches
closing references and retains its original FAIL; the next complete read matches
native after 0.78 seconds. A later timed repetition passes with complete trees: modal
opens in 0.48 seconds and dismisses in 0.66 seconds, with matching native state.
The app-restart check exposed a mixed-snapshot child-resolution race in the host.
Structural children now resolve from one captured state; a deterministic regression
fails on the previous method and passes on the current method, and all three live
fixture restarts produce complete trees. A final read-only browser comparison with
that presenter matches all 3,009 Windows fixture page nodes for native names, child
order and button roles. The final presenter also passes the
20-second helper-suspension test: complete trees before/after, bounded action
rejection, same-helper recovery and no delayed checkbox mutation. The headless
status request during suspension takes 22.95 ms. Diagnostic and host-lock-interrupted
runs are not qualified. These results do not qualify Windows-host support, signed
release distribution, every provider or unlimited trees. Local evidence is in
`docs/accessibility-validation/runs/20261103-tree-loading/`; `candidate-final-manifest.json`
records source and binary hashes and the scope of the final comparisons. Earlier
failures remain alongside the passing repetitions.

## Reading the status

| Label | Meaning |
|---|---|
| Observed passes | Specified cases worked. Not a guarantee about every provider or operation in the family. |
| Partial | Required behavior/evidence is missing, or outcomes within the family differ. |
| Bridge failure | Independent native and public viewer observations establish incorrect or missing mirrored state. |
| Native limitation | Same exact operation failed in a matched native comparison; scoped to that provider, build and state. |
| Client blocked | Required operation/query unavailable or rejected by the computer-use client; bridge success or failure not inferred. |
| Unimplemented | Mapping absent from current source. |
| Unknown | Failure ownership or behavior unresolved. |
| UNRUN | No completed execution of the specified protocol and required variants recorded. |

Historical PASS, PARTIAL, FAIL and BLOCKED verdicts retain their original meanings. Native reproduction changes attribution without erasing an original FAIL. Pointer/keyboard fallback can complete a task without passing a semantic accessibility-action assertion.

## Current capability chart

IDs are stable; maintain both platform columns independently. Evidence references identify recorded local runs or link to tracked source.

| ID and capability | macOS guest | Windows guest | Evidence |
|---|---|---|---|
| C01 Metadata and hierarchy | Controls and list/outline subtrees match native in the scoped fidelity follow-up (36 and 509 nodes); boolean types and captured attribute errors preserved. Complete provider coverage unverified. | Matching disabled/progress comparisons PASS; status text preserved in AXDescription. Full UIA property coverage absent. The seven Color-dialog pane flags match native UIA8; the older .NET UIA baseline disagrees. | [Native controls comparison][E01], [M1], [W1], [A1] |
| C02 Buttons and toggle states | Existing semantic passes retained. Latest disabled client workflow PROVISIONAL after disclosed desktop interference; separate native AXPress returns success with no effect. | Existing button/toggle passes retained. Latest disabled client workflow PROVISIONAL; no AXPress advertised/traced, native Invoke throws ElementNotEnabled. Equivalent semantic error path unqualified. | [Native controls comparison][E01], [M1], [W1], [F1], [F2] |
| C03 Text values and numeric setters | Unicode/text setters and native slider cases pass. Read-only progress 0.65/0..1 and level 40/0..100 match native. | Same progress values/ranges/read-only flags PASS, alongside existing text/slider passes. | [Native controls comparison][E01], [M1], [W1], [R1] |
| C04 Increment and decrement | Native stepper and HTML 2→3→2 actions pass with the direct reader; the HTML maximum-bound setup still fails. | Native stepper no-op remains; direct-reader HTML increment/decrement and min/max checks pass. | [M1], [W1], [N1] |
| C05 Menus tabs dialogs and disclosure | Partial: standard disclosure has functional passes; semantic route unproved. **Custom expansion no-op reproduced with both native AXDisclosing and AXExpanded setters; viewer state matches native.** | Partial: working paths; popup completion failures reproduced natively. | [Disclosure attribution][E02], [M1], [W1], [N1], [R1] |
| C06 Text selection replacement and formatting | **All four native text-chain workflows observed PASS**, including selected replacement, inherited bold, menu italic and Command+B. The direct-reader follow-up also passes HTML first selection/replacement; complete repetitions/timing qualification remain incomplete. | **The same four native workflows observed PASS**, confirming historical passes. The empty-selection setter fix also passes three fresh Edge selection/replacement checks; one initial full-tree capture is incomplete. Full qualification remains incomplete. | [T2], [M1], [W1] |
| C07 Parameterized queries and text markers | Scoped text-query follow-up passes six valid results, three invalid-range errors and an unsupported-query error. Geometry, other types and markers remain unqualified. | Nine valid table-cell queries match native. The presenter propagates translated query errors; the original browser table repeat matches all three invalid-coordinate errors. Native WPF null-cell results now become noValue, verified in the restarted production viewer. Text-query, geometry and marker equivalence remain unqualified. | [T2], [Table comparison][E03], [A1], [MAC], [UIA], [CODEC] |
| C08 Table and grid relationships | **Original 3×3 HTML table:** relationships PASS; all nine cell bounds also PASS at the user's resized viewer pose. Other variants and complete side-effect qualification remain incomplete. | **Same table:** matching relationships PASS; native column information preserved without structural column objects. The latest original-table repeat preserves invalid-coordinate errors; native null WPF cells now map to noValue. Other variants and full bounds remain unqualified. | [Table comparison][E03], [Geometry comparison][E04], [M1], [W1], [A1] |
| C09 Explicit offscreen reveal before selection | Direct-reader HTML reveal/selection works. AppKit native rows reject AXScrollToVisible; the current proxy preserves the native actionUnsupported error. | Direct-reader explicit reveal followed by selection works for the original HTML list, native table row and native outline targets. Broader geometry qualification remains incomplete. | [M1], [W1] |
| C10 Multiselection and virtualization | HTML Shift+Up selects packing items 23 and 24 in the direct-reader repeat. Native table Shift+Down remains unsuccessful with the original focus requirements. | HTML Shift+Up selects packing items 23 and 24; native table Shift+Down remains unsuccessful. ItemContainer discovery is unimplemented. | [M1], [W1], [UIA] |
| C11 Focus modal return geometry and hit testing | Six native controls **and the modal Confirm button** pass bounds/frame/window identity and precise center hits. Sheet ownership and focused flags match native. **Application focused-element lookup fails** despite a correctly focused mirrored control. Original nine HTML cell bounds pass. | Same native geometry and hit assertions pass. **Current WPF and Color-dialog comparisons preserve owner/background hierarchy. Application focused-element lookup still fails**, while individual focused flags match native in foreground observations. | [Modal comparison][E05], [Native controls][E06], [HTML geometry][E04] |
| C12 Secure input | Original native 11→9 workflow PASS. **HTML native/public/CU masks and pixels also pass after explicit recovery from I14.** Tested public reads expose no plaintext; native HTML RTF privacy remains unqualified. | **Current direct-reader WPF and Edge checks PASS:** native/public masks match for 11→9, empty and Unicode values. Secure range and selection/character reads remain blocked. Historical HTML CU passes retained; full privacy and pixel qualification remain incomplete. | [HTML comparison][E07], [Native comparison][E08] |
| C13 Guest-originated updates | Checkbox value update: **3/3 monitored PASS**. Other event-update variants remain unrun. | Same checkbox value update: **3/3 monitored PASS**. Other event-update variants remain unrun. | [P1], [P2] |
| C14 Large trees and availability during refresh | Current large-page run exposes all 3,000 changing targets and matches the 18,770-node Wikipedia page to native AX for five fields and ordered children. The earlier three Safari resize transitions recover all 306 page nodes, with 4.75–5.78-second gaps. The separate initial control-page omission (I15) remains unqualified. | Current run exposes all 3,000 changing targets; 3,009 fixture nodes and 11,429 Wikipedia nodes match native names, ordered children and button roles. Page remains available during all sampled changes; close/reopen and three app restarts pass. Earlier Edge resize checks preserve secure values and edits. Provider-wide completeness and truncation remain unqualified. | [HTML comparison][E07], [T2], [L1], [S1], [W1]; current follow-up above |
| C15 Events and announcements | Selected snapshot-derived notifications; full native event stream not relayed. Announcement parity unqualified. | Same host notification subset; complete UIA event semantics not relayed/qualified. | [HOST], [ELEMENT], [A1] |
| C16 Recovery expiry and concurrent actions | A confirmed 20-second pause resumes the same helper, preserves value 0 and permits a fresh 0→1 action. Action and writable-slider timeouts now return `cannotComplete` at about five seconds. Broader overlap guarantees remain incomplete. | A confirmed 20-second pause resumes the same helper, preserves value 0 and permits a fresh 0→1 action. The latest retained target rejects with `cannotComplete` before the bridge timeout; other race guarantees remain incomplete. | [M1], [W1], [R1] |
| C17 Refresh latency and performance | Wikipedia heading/search appear in 3.09–3.91 seconds; tested theme readbacks take 3.70–3.76 seconds after the action. No general latency guarantee. | Wikipedia heading/search appear in 3.11 seconds. Focused-control refresh fixes scoped stale selected-state reads: eight post-action readbacks take 0.75–0.83 seconds after the first takes 3.98 seconds. Background status changes on the 3,000-target stress page still advance about every 8–9 seconds. | [F1], [F2], [R1], [Q1]; current follow-up above |

## Production refactor follow-up — 2026-10-31

The [production run and inconclusive-check repeats][E09] used the rebuilt, restarted Release headless daemon, normal Windows helper, IVSHMEM and public Mac viewer. Six independent native/public text-state comparisons PASS after repeating an interrupted selection check. A supplemental button/disabled-button retry produced the expected count changes without a click-away interruption. Lists recovered after an updating interval, but explicit reveal-before-select remains client blocked and exact latency is unqualified.

Common native controls and five HTML control cases also produced their expected observed results. At that time, stepper, popup, modal-owner and native secure-mask limitations remained. The current secure-mask fix is documented above. This is a scoped Windows-guest functional follow-up, not a full rerun of the historical 51 workflows; it does not promote unrun browser, macOS-guest, stress, Hyper-V or release-distribution cases to current-build passes. The capability and historical workflow tables retain their previously documented scopes.

## Controlled comparison — 2026-10-30

The [executable recorder and replay checker][HARNESS] now qualify one narrow case: an existing native checkbox changes from 1 to 0 inside the guest, without further viewer input, and the change must reach public viewer accessibility and a combined OpenAI computer-use tree/screenshot. These are three repetitions per platform on the same host build, with the viewer independently verified in the foreground. [Replayed raw evidence and per-assertion results][P1] are authoritative for this slice.

| Measured assertion | macOS guest | Windows guest |
|---|---:|---:|
| Native action and independent native readback agree on 0 | 3/3 PASS | 3/3 PASS |
| Independent public viewer value reaches 0 within the frozen observation interval | 3/3 PASS | 3/3 PASS |
| Combined CU tree and reviewed screenshot agree on 0 | 3/3 PASS | 3/3 PASS |
| Independent input/focus audit: no input, stable foreground viewer/window, complete sampling | 3/3 PASS | 3/3 PASS |

**Earlier attempts in this batch are provisional.** The user disclosed desktop activity, and we found that a raised viewer window had remained in the background. Those raw observations and their original reports are preserved with a [superseding audit notice][P2]. During the clean Mac repetitions the host locked before one external mutation; the monitor detected the interruption, that attempt was preserved without a product verdict, and a fresh attempt completed after unlock.

**Current result: PASS on both platforms.** I01 is closed for the tested checkbox-update case; the earlier provisional observations remain archived and are not counted as an active failure. Work now moves to other chart rows. No production bridge code changed. These six comparisons do not rerun the original 51 workflows, qualify the complete event-update family, establish atomic screen/tree capture, or measure exact mutation-to-publication latency.

## Implementation boundaries

These are implementation boundaries, not conformance passes.

| Area | macOS guest | Windows guest |
|---|---|---|
| Collection scope | Frontmost application's windows/menu bar, not every guest process. | Foreground window and related visible owned windows. |
| Actions | Discover/forward advertised native names; unfamiliar names are not inherently rejected. | Explicit press, increment/decrement, show/cancel, reveal and four page-scroll directions. |
| Writable properties | Discover native settable attributes; validate and forward encodable values. | Value, focus, selection, expanded/disclosing state, selected children/rows and selected text range. |
| Queries | Advertised ordinary/parameterized getters; passive snapshot attributes and on-demand queries. | Cell by column/row; string, bounds and attributed string for range; range for position. |
| Data and references | Defined codec types; cross-application PID references not preserved. | Defined translated types; no arbitrary custom UIA-pattern forwarding. |
| Specialized functionality | Generic dispatch is not proof every custom attribute, setter, parameter or type survives translation. | Transform, Dock, MultipleView, ItemContainer discovery and specialized spreadsheet/annotation interfaces lack mappings; some implemented patterns are partial. |
| Window embedding | Guest windows/sheets embedded as groups; host relationships/geometry intentionally adapted. | Same host embedding; modal state mapped, WindowPattern Close/SetWindowVisualState not directly dispatched. |
| Notifications | Selected value, selection, layout, focus and destruction notifications. | Same host notification subset. |
| Protected contents | Masked AXValue preserved; secure text/character metadata and parameterized reads restricted. Some unadvertised native Safari range queries return masks but are denied publicly. | Masked/empty ValuePattern preserved; validated secure TextPattern masks fill absent/empty ValuePattern values. Non-mask text and secure text queries remain blocked; WPF/Edge scoped checks pass (I13). |
| Size and time budgets | Shared safety bounds, not guaranteed capacities. | Shared safety bounds plus collector-specific limits. |

Exact source inventory and operation lists: [A1], [MAC], [UIA], [UIA_HOST], [CODEC], [ATTR], [PROTOCOL], [SESSION], [HOST], [ELEMENT]. Windows guests send UIA data; the Mac host adapter performs UIA-to-AX translation. An absent specialized interface does not imply ordinary buttons or text in that application are inaccessible.

## Recorded workflow results

Source arms: [Mac 20261029][M1] and [Windows 20261029][W1]. These are one recorded functional arm per platform, not completed release qualification. Preserve IDs and procedures in the [suite plan][PLAN] and [case review][REVIEW].

| Recorded outcome | macOS | Windows |
|---|---:|---:|
| PASS | 22 | 28 |
| PARTIAL | 17 | 11 |
| FAIL | 3 | 4 |
| BLOCKED | 9 | 8 |
| Total active workflow cases | 51 | 51 |

These counts are not percentages of accessibility support. Original HTML slider and HTML formatting exclusions remain excluded; native equivalents remain active. Later follow-ups do not promote unrelated historical cases to current-build passes.

| Case | macOS recorded | Windows recorded | Attribution or remaining gap |
|---|---|---|---|
| `native/button` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/disabled` | PARTIAL | PARTIAL | Historical verdicts retained. Latest client click/capture results PROVISIONAL on both guests after disclosed interference; audits cover follow-up reads, not the full action. Disabled metadata and separate native attribution observed. Windows equivalent semantic error path remains unqualified. [Current comparison][E01] |
| `native/checkbox` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/switch` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/radio` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/slider` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/stepper` | PASS | FAIL | Mac increment/decrement passed. Windows exact Up.Invoke no-op reproduced natively [N1]. |
| `native/popup` | PASS | PARTIAL | Mac selection/closure observed. Windows native SelectionItem.Select also leaves popup open [N1]. |
| `native/combo` | PARTIAL | PASS | Mac two-stage case remains partial. Windows both stages observed. |
| `native/segments` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/date` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/color` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/progress` | PASS | PASS | Historical functional passes retained. Current native/public values/ranges/read-only flags PASS within audited reads; combined visual/workflow qualification PROVISIONAL after disclosed interference. [Current comparison][E01] |
| `native/sheet` | PASS | PARTIAL | Historical Windows owner evidence was incomplete. The [paired follow-up][E05] confirms a missing Windows owner window, with working Open/Confirm actions on both platforms. |
| `native/offscreen_table` | BLOCKED | BLOCKED | Both: native/proxy target and reveal exposed; prescribed client reveal-before-select unavailable. |
| `native/table_multiselect` | BLOCKED | BLOCKED | Both: offscreen-selection prerequisite blocked; keyboard extension unverified. |
| `native/offscreen_outline` | BLOCKED | BLOCKED | Both: prescribed reveal/expand/reveal/select sequence unverified. |
| `native/title_set` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `native/secure_field` | PARTIAL | PARTIAL | Historical results retained. [Controlled follow-up][E08]: Mac 11→9 workflow passes; Windows native masks are now proven and their public omission is a bridge support gap. |
| `native/text_select` | BLOCKED | PASS | Historical Mac prerequisite failure. Latest matched native chain passes on both platforms [T2]. |
| `native/format_button` | FAIL | PASS | Historical Mac selection failure. Latest foreground selection and phrase-only bold pass on both platforms [T2]. |
| `native/format_menu` | BLOCKED | PASS | Historical Mac prerequisite block. Latest selected-range italic and retained bold pass on both platforms [T2]. |
| `html/list-single-offscreen` | BLOCKED | BLOCKED | Both: explicit reveal-before-select client blocked; no substitute counted. |
| `html/list-multiple-offscreen` | BLOCKED | BLOCKED | Both: explicit reveal prerequisite client blocked; keyboard extension unverified. |
| `html/nested-scrolling` | PARTIAL | PASS | Mac semantic activation route incomplete. Windows automatic reveal plus AXPress verified. |
| `html/native-popup` | PARTIAL | FAIL | Mac route/completion incomplete. Windows native Invoke reproduces popup remaining open [N1]. |
| `html/menu-submenu` | PARTIAL | PASS | Mac semantic routes incompletely proved. Windows hierarchy, command and dismissal verified. |
| `html/text-value` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `html/text-range` | FAIL | PASS | Mac client rejected selection before setter. Windows range and exact replacement verified. |
| `html/tabs` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `html/disclosure` | FAIL | PASS | Historical outcomes preserved. Mac standard disclosure works; custom no-op now reproduced with both native expansion setters, so attribution is native-provider behavior for that route. Windows prescribed coordinate collapse remains physical input. [Disclosure attribution][E02] |
| `html/mixed-checkbox` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `html/stepper` | BLOCKED | BLOCKED | Both: prescribed actions unavailable through client surface, not proof of bridge no-op. |
| `html/combobox` | PARTIAL | FAIL | Mac route/focus comparison incomplete. Windows native SetValue also fails to open suggestions in matched state [N1]. |
| `html/dialog` | PARTIAL | PARTIAL | Both: controlled focus-return leg incomplete. |
| `html/negative-controls` | PARTIAL | PARTIAL | Full negative/error contract unverified. Original disabled HTML button lacks an activation handler, so its unchanged neighbor counter alone is not an enforcement oracle. Read-only mutation remains pending. [Source review][E10] |
| `html/table-relations` | PARTIAL | PARTIAL | Historical result preserved. Later paired comparison passes counts, contents, indices, spans, headers, cell queries, aliases and containing-table relations on the original table; full bounds remain unverified. Windows invalid-query error fidelity fails. [Table comparison][E03] |
| `html/bounds-and-offscreen` | PARTIAL | PARTIAL | Mac activation route incomplete. Windows reveal/press observed; full geometry/hit-test contract incomplete. |
| `extra/html-button` | PARTIAL | PASS | Mac trace proves reveal, not semantic AXPress. Windows AXPress observed. |
| `extra/html-checkbox` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `extra/html-switch` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `extra/html-radio` | PASS | PASS | Recorded functional assertions passed on both platforms; scope is this case and recorded build. |
| `extra/html-main-popup` | PARTIAL | FAIL | Mac opening route incomplete. Windows native Invoke also leaves selected popup open [N1]. |
| `extra/html-secure` | PASS | PARTIAL | Historical results retained. [Current comparison][E07] proves native/public/CU 11→9 masks on both guests; Windows exact pixel count remains unqualified. Mac pixels pass after explicit recovery from the separate I14 resize failure. |
| `extra/native-cmd-b` | BLOCKED | PASS | Historical Mac prerequisite block. Latest Command+B/Ctrl+B removes bold and retains italic on both platforms [T2]. |
| `extra/finder-modal-value` | PASS | BLOCKED | Mac exact modal value workflow passed. Windows equivalent modal procedure unresolved. |
| `stress/large-page-completeness` | PARTIAL | PARTIAL | Both: deadline/truncation accounting incomplete. Windows all 1000 targets exposed; separate Mac real-page failure [L1]. |
| `stress/large-page-offscreen-action` | PARTIAL | PASS | Mac pointer fallback does not prove AX activation. Windows offscreen target, AXPress and result verified. |
| `stress/stalled-agent-responsiveness` | PARTIAL | BLOCKED | Mac raw timeout attribution incomplete. Windows client rejected target before bridge request. |
| `stress/recovery-after-agent-stall` | PASS | PASS | Both: fresh action after recovery observed; preceding timeout contract still unproved. |
| `stress/native-vm-performance-parity` | PARTIAL | PARTIAL | Measurements exist; no frozen latency acceptance threshold or performance-parity pass. |

## Follow-up evidence

| Evidence date | Scope | macOS finding | Windows finding | Remaining limit |
|---|---|---|---|---|
| 2026-10-30 | Semantic-action refresh change | Selected control workflows pass; diagnostic median checkbox observation about 1.39 s to 0.32 s. | Selected control workflows pass; corresponding median about 1.84 s to 0.82 s. | Diagnostic timing is separate from workflow passes; full-suite, physical-input and overlap qualification incomplete. [F1], [F2], [F3] |
| 2026-10-30 | Guest-originated change | Native value changed, public AX stale; independent native observers received notifications. | No equivalent comprehensive conclusion. | Exact subscription/capture/publication fault unisolated. [F3] |
| 2026-10-30 | Large real page | Native content existed while public content was missing; later input recovered a large tree. | No equivalent matched real-page arm. | Cause and full completeness unknown; observation gaps are not capture durations. [L1] |
| 2026-10-30 | Snapshot availability | Delayed-update fixture remained usable; large-page refresh temporarily exposed only updating status. | Shared pending-state architecture applies; equivalent observation not established here. | Full AX traversal and screenshot lack a demonstrated shared-frame guarantee. [S1], [SESSION] |
| 2026-10-30 | Real-site scheduling comparison | Recorded search/filter/navigation outcomes pass; many clicks use pointer fallback. | Recorded semantic outcomes pass; fresh address setter passes, original failed attempt unattributed. | Sequential cases do not prove overlapping-action safety; account-gated workflow blocked. [R1] |
| 2026-10-30 | Attributed-text query cost | No equivalent isolated conclusion. | Repeated current-path queries take about 2.1 s. | Native versus scheduling/translation cost unisolated; parent/current query ranges differ. [Q1], [R1] |
| 2026-10-30 | User-resized geometry | All nine original-table bounds, frames and window identities match the independent OS transform exactly. Eighteen cell/text-center hits match native coarse page results; both native AX roots reproduce them. | Matching arm unrun. | Current Mac pose only; precise targeting, additional poses/DPI/clipping/overlays remain unqualified. [Geometry comparison][E04] |
| 2026-10-30 | Native control geometry and precise hits | All six prescribed control centers return the exact target; bounds, frame/window identity and state match. | Same six assertions pass; native/public hits return the expected direct child. | Current interiors/poses only; no action passes, boundary/overlay/offscreen or full DPI qualification. [Native controls][E06] |
| 2026-10-30 | Owned modal and focus comparison | Open/Confirm use AXPress; sheet owner/background hierarchy, dismissal/status and modal button geometry/hit pass. Focused flags pass; application focused-element lookup fails. | Open/Confirm use AXPress; dismissal/status and modal button geometry/hit pass. Native owner/background tree is omitted publicly; focused-element lookup fails. | Windows Confirm readback occurred with host viewer in background; functional outcome retained, focus scored only in separate monitored foreground state. No complete-family or atomic-capture claim. [Modal comparison][E05] |
| 2026-10-30 | Original native secure field | Native AX value, viewer value and reviewed pixels show 11→9 masks after one CU setter; secure state and writability preserved. | Native TextPattern proves 11→9 masks after one CU setter. Native/public ValuePattern value stays safely empty; masked TextPattern readback is missing publicly. | No plaintext in the specifically tested reads/CU captures. Windows exact pixel count, HTML arm, stale captures and live secure-flag changes remain unqualified. [Secure-field comparison][E08] |
| 2026-10-30 | Original HTML secure field and right-half viewer placement | Native/public/CU 11→9 masks and reviewed pixels pass after explicit recovery. New I14: resize leaves no guest controls publicly despite native availability; Escape restores them. | HTML 11→9 native/public/CU masks pass via ValuePattern, narrowing I13 to the missing TextPattern path. Exact pixel count remains unqualified. | Public tested reads expose no plaintext. Native RTF bytes remain unqualified; a later Windows selection diagnostic identifies a line feed. Failed/background attempts retained. [HTML comparison][E07] |
| 2026-10-30 | Original native disabled and read-only progress controls | Client interaction PROVISIONAL after disclosed interference. Audited metadata/range reads match; separate native AXPress succeeds with no effect. | Client interaction PROVISIONAL. Audited metadata/range reads match; native Invoke throws ElementNotEnabled, equivalent public semantic path unqualified. | Post-click quiet audits do not cover the action itself. Full action/capture monitoring required on rerun. No production edits. [Native controls comparison][E01] |

## Outstanding issues and next evidence

Completed tested cases: **I01 checkbox update — PASS on both platforms**, three monitored repetitions each; **I04 foreground native text selection — observed PASS on Mac**, with the full four-workflow chain completed [T2]. Historical attempts are retained and do not override these current passing results. Broader repetitions and focus variants remain coverage work.

| ID | Platform | Classification | Evidence needed to close |
|---|---|---|---|
| I02 Native content missing from mirror | macOS | Bridge failure, cause unknown | Compare collection, transmitted snapshot and public AX for one attempt; identify loss and repeat across complex providers. |
| I03 Tree unavailable during refresh | Observed macOS; shared architecture | Availability gap | Small text edits lack public targets temporarily; standard disclosure also lacks its target at +1.96/+1.91 s despite correct native state, then has correct CU tree/pixels by returns at +3.56/+3.43 s. These are observation times, not exact gap durations. Establish bounded availability and screen/tree consistency requirements before changing behavior. [T2], [Disclosure attribution][E02] |
| I05 Custom disclosure ineffective | macOS | **Attribution resolved: native setter no-op** | Both native `AXDisclosing=true` and `AXExpanded=true` report success without changing expansion or children in the original custom tree. Public state matches native; the client route forwards AXDisclosing. Keep the functional failure, but do not count it as a proven bridge defect. [Disclosure attribution][E02] |
| I06 Attributed-text query delay | Windows | Unknown attribution | Compare equivalent native/viewer queries over the same range and formatting runs; separate provider and scheduling costs. |
| I07 Reveal and raw-query coverage | Both | Client blocked or partial oracle | Exercise prescribed exposed operations when available; report independent bridge evidence separately. No replacement control harness or weaker procedure. |
| I08 Specialized capability coverage | Both | Mac generic conformance unverified; Windows mappings absent | Enumerate required v1 operations/platform equivalents; prove native capability before generalized implementation. |
| I09 Qualification incomplete | Both | UNRUN and partial evidence | Pin actual host/guest artifacts, enumerate variants and complete required repetitions; retain blocked cases. |
| I10 Native query errors discarded | Both guest platforms | **Presenter fixed; Mac invalid-range cases verified** | Local fidelity follow-up preserves all three Mac invalid-range errors and an unsupported-query error. Windows browser invalid-coordinate errors match native in the expanded repeat, and WPF null cells now map to `noValue`. Other query families and provider variants remain unqualified. Historical failures: [T2], [Table comparison][E03]. |
| I11 Owned modal drops background window | Windows | **Fixed in scoped current-helper repeats** | Explicit HWND ownership replaces the root-ancestor lookup that omitted the WPF owner. Owner-first UIA traversal preserves native hierarchy. WPF (80 nodes) and Color (98 nodes) match native UIA8 labels, mapped roles, enabled states and ordered edges. Confirm, reopen, Cancel and subsequent checkbox actions pass. Nested/hidden-owner and Windows-host variants remain unqualified. Historical evidence: [Modal comparison][E05]. |
| I12 Application focused-element lookup missing | Both | **Our public focus relationship fails** | In monitored foreground observations, native AX/UIA identifies a focused element and the public element carries the matching focused flag, but host application `AXFocusedUIElement` returns `-25212`/no value. The proxy-root `AXFocusedUIElement` relationship now returns the focused field on both platforms. Host application-wide lookup still fails; that remaining route is unisolated. [Modal comparison][E05] |
| I13 Secure TextPattern masks omitted | Windows | **Fixed; WPF and Edge verified** | **Fixed in the current scoped run:** WPF TextPattern-only masks and Edge values match native UIA8 at 11, 9, empty and Unicode states. Secure text queries remain blocked. Broader provider/privacy variants remain unqualified; historical failures are retained. [Native comparison][E08], [HTML comparison][E07] |
| I14 Guest tree lost after viewer resize | Both guests tested on Mac host | **Persistent loss fixed in scoped repeats; transient gaps remain** | Presenter cache retention plus guest display-change refresh restore the Mac page without Escape or manual refresh across three transitions. All 306 page nodes match native; post-resize edits pass. Matched Windows secure-value/edit checks pass. Minimize/restore and close/reopen pass on both. Recovery timing and limits are recorded above; broader resize/geometry variants remain unqualified. Historical failure: [HTML comparison][E07]. |
| I15 Initial Safari page subtree omitted | macOS | **Observed mismatch; current-helper repeat unrun** | During browser setup, native AX contains the secure field while the complete public tree contains browser chrome but no page. An explicit setup refresh restores it. Reproduced before the guest display-notification change; fresh-open behavior with that final helper has not been qualified. Raw attempts r1/r3 are retained in the current run. |

## Next testing and harness work

The checkbox-update row and four native text workflows have passing observations on **both** guest platforms. The original HTML table now has paired passing evidence for counts, contents, nine cell queries, indices, spans, headers, aliases and containing-table relationships. Query error loss is proven on both guest paths. The active goal is to fill remaining chart gaps; more table variants, bounds, query families and unresolved failures take priority over repeating known passes. Do not keep retesting a passing row instead of advancing coverage.

Reuse the existing fixtures, native observers, suite plan and result schema. The [public tree reader](../../tools/accessibility-fixtures/reader/README.md) provides direct host AX snapshots and like-for-like comparisons without computer use. Its read-only results are separate from the historical workflow verdicts, which used OpenAI computer use. The [recorder][HARNESS] preserves command output, exact code/tool bytes, binary identities, bounded scheduled reads and input/focus evidence; its checkers derive scoped results from raw artifacts. Matched native diagnostic actions establish attribution and never count as client activation passes.

| Order | Harness or testing work | Required result |
|---|---|---|
| 1 | Pin actual host/guest builds, fixture hashes and viewer state. Record one existing button or checkbox workflow on each VM. | Native result, public viewer state, client tree, screenshot and actual action route agree. Save explicit failures when a required observation is missing. |
| 2 | Extend guest-originated update coverage to focus and structure changes. The checkbox value-update variant is already PASS. | Record the new variants with matched independent native/public/client evidence and controlled observation intervals. |
| 3 | Add capability discovery and a result checker using the existing schema. | Every observed action, writable property, query and relationship has a tested assertion or visible gap. Reject unsupported PASS claims; do not infer outcome from successful returns. |
| 4 | Test text selection, expansion, explicit offscreen reveal-before-selection and table relationships. | Resolve I04/I05 and separate I07 client restrictions from missing or incorrect bridge behavior. Preserve the exact original procedures. |
| 5 | Recheck complex real-site workflows on both VMs during the same development cycle. | Confirm fixes generalize beyond the fixture. Use identical workflows/viewports where supported; report native differences rather than adding site-specific code. |
| 6 | Expand controlled tree size, shape, delayed updates, virtualization, lifecycle and provider-fault variants. | Measure complete-tree availability, correctness, latency and recovery separately. Define acceptance criteria before scoring; retain unknowns and failed first attempts. |
| 7 | Run the unchanged 51-case suite on both VMs for the required repetitions, plus explicitly enumerated conformance variants. | All scheduled results accounted for and linked here; no silently removed failures, blocked cases or exclusions. |

Completed paired harness scope includes the external checkbox-value comparison above and independent replay of all six states in the four native text workflows on both platforms. Text replay checks exact text, UTF-16 selection, formatting coverage and font metadata; broader repetition and timing qualification remain incomplete. Other listed work remains planned or partial. The checkers reject incomplete scans, ambiguous targets, getter errors, altered artifacts, wrong runtime identities and contradictory observations; controlled workflow qualification also requires input/focus coverage. Rejection checks are component checks and earn no VM compatibility passes. Internal collection/transport/presentation boundary instrumentation is still absent; endpoint observations alone cannot isolate those stages individually.

The detailed execution contract remains in [the existing validation design][E11] and [tester instructions][E12]. Keep the full baseline fixtures unchanged; use separately versioned fixtures for new conformance scenarios. Report direct API evidence separately whenever the client cannot express the required operation.

## Automated conformance run, 2026-11-03 UTC

The native Objective-C runner exercised the real `ax-test-macos` VM through the
production viewer's public AX interface. Host and guest run macOS 27.2. The native
baseline and guest fixture have matching source revisions; SSH only copied and
launched the fixture. This run used a background standalone baseline while the
existing paired host retained its older embedded fixture. No computer-use input
or window activation was performed by the runner.

**441 cases classified in 98.49 seconds:** 358 PASS, 56 FAIL, 17 BASELINE_MISMATCH,
5 INCOMPLETE, 2 native unsupported/no-value results, and 3 REQUIRES_FOCUS.
The median case took 3.24 ms; the 95th percentile took 1.10 seconds.
A three-repetition follow-up covering actions, descriptions, dynamic controls,
basic controls, and geometry echoes recorded 89 PASS and 4 INCOMPLETE in 33.13
seconds. The incomplete reads exposed invalid children on the proxy side; they
were not retried into passing results.

| Test layer | Cases | PASS | Remaining results |
|---|---:|---:|---|
| Native controls, text, geometry, and embedded WebKit | 180 | 154 | 18 comparison failures; 3 incomplete; 2 native unsupported/no-value; 3 focus-only |
| Synthetic action execution, descriptions, enumeration | 25 | 25 | None in this run |
| Synthetic metadata and role declarations | 164 | 161 | Three intentional window/application-to-group mappings differ from literal native roles |
| Synthetic notification delivery | 54 | 0 | 35 comparison failures; 17 notifications absent from the native baseline; 2 incomplete |
| Synthetic typed values | 18 | 18 | None in this run |

The fixture's legacy provider now uses `NSObject` and implements observer lifecycle
support; its previous base class rejected action enumeration. Dynamic control
creation no longer queries an unsupported label getter during AX dispatch. Outline
postconditions run after state convergence. Color/geometry comparisons allow only
floating-point round-off; other typed values and errors remain exact.

Outstanding observations include notification posts without a tree-state change
not reaching the host, invalid child references during repeated transitions,
missing custom rotors, secure-field metadata filtering, browser position-query
differences, and native/proxy error-code differences. Notification posts are
synthetic transport checks, not evidence that real application lifecycle events
work. The three focus cases and expanded Windows conformance suite remain unrun.

The SDK inventory contains 1,597 declarations, including constants and types:
514 have assigned cases and 1,083 remain gaps. These counts do not measure complete
feature support. Coverage rejects changed fixture, case, reader, or runner evidence;
this run has no stale evidence or unknown catalog symbols.
Local evidence: `/tmp/asb-conformance/production-proxy-r5/`.

## Planned conformance coverage

The [capability catalog][CATALOG] defines **45 families and 169 mandatory assertion clauses**. Required variants are not fully enumerated and locked. Checkbox updates, native text workflows, original table relationships and the Mac disclosure attribution now have scoped evidence as listed below. No whole family is qualified. Overlapping workflow observations do not make a family PASS. Establish platform-specific applicability explicitly.

| Conformance family | Mandatory clauses | macOS protocol | Windows protocol |
|---|---:|---|---|
| `cap/discovery` | 4 | UNRUN | UNRUN |
| `cap/negative-capabilities` | 3 | PARTIAL: audited disabled metadata and native diagnostic observed; client no-effect workflow PROVISIONAL after disclosed interference; other variants pending | PARTIAL: audited disabled metadata observed; client no-effect workflow PROVISIONAL; equivalent native Invoke/error not exercised publicly |
| `cap/scalar-values` | 4 | PARTIAL: tested finite progress values and zero/min/max preserved; complete type/error/empty/nonfinite protocol pending | PARTIAL: matching finite numeric values preserved; native nonfinite steps retained in evidence, mapping/type variants pending |
| `cap/structured-values` | 4 | UNRUN | UNRUN |
| `cap/custom-capabilities` | 4 | UNRUN | UNRUN |
| `cap/tree-graph` | 4 | UNRUN | UNRUN |
| `cap/external-relations` | 4 | UNRUN | UNRUN |
| `cap/identity` | 4 | UNRUN | UNRUN |
| `cap/invoke` | 3 | UNRUN | UNRUN |
| `cap/overlapping-patterns` | 3 | UNRUN | UNRUN |
| `cap/toggle` | 3 | UNRUN | UNRUN |
| `cap/range` | 3 | PARTIAL: two read-only progress value/min/max/writable comparisons PASS; exact pixel fractions, steps and boundary/write variants pending | PARTIAL: matching read-only progress comparisons PASS; remaining variants pending |
| `cap/focus` | 4 | PARTIAL: native modal sequence focused flags match; application focused-element lookup FAIL; other variants pending | PARTIAL: foreground modal/closed focused flags match; application focused-element lookup FAIL; action-time focus return not qualified |
| `cap/selection` | 3 | UNRUN | UNRUN |
| `cap/selection-arrays` | 4 | UNRUN | UNRUN |
| `cap/selection-anchor` | 3 | UNRUN | UNRUN |
| `cap/expand-collapse` | 3 | PARTIAL: original standard disclosure works; custom expansion failure reproduced natively, mirrored state matches. Other variants pending. | UNRUN |
| `cap/menus` | 4 | UNRUN | UNRUN |
| `cap/tabs` | 3 | UNRUN | UNRUN |
| `cap/modal-windows` | 4 | PARTIAL: original sheet Open/Confirm, owner/background hierarchy and dismissal/status PASS; native AXModal unavailable on sheet; other variants pending | PARTIAL: original dialog Open/Confirm and dismissal/status PASS; modal flag preserved; owner/background tree FAIL; other variants pending |
| `cap/reveal-before-select` | 4 | UNRUN | UNRUN |
| `cap/virtualization` | 3 | UNRUN | UNRUN |
| `cap/direct-offscreen-action` | 3 | UNRUN | UNRUN |
| `cap/page-scroll` | 4 | UNRUN | UNRUN |
| `cap/text-values` | 3 | UNRUN | UNRUN |
| `cap/text-selection` | 4 | PARTIAL: original unfocused selection and exact replacement PASS; other variants pending | PARTIAL: matching original selection/replacement PASS; other variants pending |
| `cap/text-queries` | 4 | PARTIAL: six valid ranges PASS; three invalid-range error comparisons FAIL; geometry/Unicode/stale cases pending | PARTIAL: full-document text/count/selection match; remaining query variants pending |
| `cap/attributed-text` | 3 | PARTIAL: original bold/replacement/italic/shortcut chain and font metadata PASS; other styles/variants pending | PARTIAL: same original chain and font metadata PASS; other styles/variants pending |
| `cap/text-markers` | 3 | UNRUN | UNRUN |
| `cap/table-relations` | 5 | PARTIAL: original 3×3 relationships and error parity PASS; complete side-effect qualification and other variants remain unrun. | PARTIAL: original 3×3 variant relationship checks PASS; invalid-query error fidelity FAIL. Other variants unrun. |
| `cap/outline-relations` | 3 | UNRUN | UNRUN |
| `cap/event-updates` | 4 | PARTIAL: checkbox value 3/3 PASS; other variants UNRUN | PARTIAL: checkbox value 3/3 PASS; other variants UNRUN |
| `cap/atomic-capture` | 4 | UNRUN | UNRUN |
| `cap/passive-observation` | 4 | UNRUN | UNRUN |
| `cap/live-queries` | 4 | UNRUN | UNRUN |
| `cap/geometry` | 4 | PARTIAL: six native controls, modal Confirm button and nine HTML cells pass bounds/frame/window identity at observed poses; other variants unrun | PARTIAL: same six native controls and modal Confirm button pass bounds/frame/window identity; HTML and other variants pending |
| `cap/hit-testing` | 3 | PARTIAL: six native control centers and modal Confirm button return precise targets; 18 HTML cell/text centers preserve native coarse page results. Other variants unrun | PARTIAL: same six native centers and modal Confirm button preserve precise target-descendant hits; other variants unrun |
| `cap/secure-text` | 4 | PARTIAL: native workflow PASS; HTML 11→9 masks/pixels PASS after I14 recovery; tested public reads contain no plaintext; native RTF and broader privacy variants unqualified | PARTIAL: HTML 11→9 masks/readback PASS through ValuePattern; current WPF TextPattern-only and Edge direct-reader mask states PASS; pixel counts and broader privacy variants unqualified |
| `cap/session-lifecycle` | 4 | UNRUN | UNRUN |
| `cap/provider-errors` | 4 | PARTIAL: scoped invalid-range error preservation PASS; other variants pending | PARTIAL: browser invalid-coordinate and WPF null-cell errors now verified; paused-helper failure preserves `cannotComplete`. Other variants remain unqualified. |
| `cap/expiry-and-recovery` | 5 | UNRUN | UNRUN |
| `cap/request-races` | 5 | UNRUN | UNRUN |
| `cap/completeness-and-limits` | 5 | UNRUN | UNRUN |
| `cap/performance` | 5 | UNRUN | UNRUN |
| `cap/input-route` | 4 | UNRUN | UNRUN |

## Updating this chart

1. **Read this chart before planning tests or making support claims.** Preserve the existing suite IDs, exclusions, native/HTML procedures and explicit reveal-before-selection.
2. **Keep raw evidence immutable.** Save each new attempt separately in the ignored run directory. Record actual host/guest executable identities, source revision, dirty changes, fixture/procedure versions, dates and relevant environment. New builds cannot inherit unrelated passes.
3. **Update affected platform cells and issues after every completed run, attribution result or relevant implementation change.** Link evidence and state what was observed. A code fix stays awaiting validation until its regression test passes. Mark affected claims awaiting revalidation; retain historical passes as historical.
4. **Separate outcome and ownership.** Native reproduction changes attribution, not an original failed outcome. Client rejection does not prove a bridge failure. Pointer fallback does not pass a semantic-action assertion. Preserve unknowns.
5. **Keep the inventories visible.** Add equivalent Mac/Windows results as a new dated run section and preserve prior run tables. Record applicability with evidence. Never remove failed first attempts, blocked cases or exclusions to improve totals.
6. **Use percentages only for a frozen, explicitly named contract.** Keep unrun and unresolved coverage in the accounting; distinguish native unsupported, client blocked and implemented-but-unverified. Do not call workflow pass rate real-world accessibility support.
7. **Append change history and verify.** Check case counts against linked summaries, local links, platform scope and absence of secrets. Reference this file in subsequent status reports.
8. **Retain operational preferences.** Use the public tree reader for read-only comparisons; record action method and evidence separately for interaction workflows. Historical computer-use verdicts remain unchanged. Production fixes remain general purpose. When switching VMs, position the viewer on the right half of the host screen so the terminal stays visible; account for any later user resize in the next observation. This chart does not require VM shutdown or production edits.

## Change history

| Date | Change | Basis |
|---|---|---|
| 2026-10-30 | Established canonical paired chart, all 51 recorded workflows, 45 unrun conformance families and current issue ownership. No new tests or production edits. | Existing summaries and source audit [A1]. |
| 2026-10-30 | Moved the canonical chart to `docs/accessibility-validation/SUPPORT.md` after documentation ownership was repaired. Results unchanged. | Documentation relocation only. |
| 2026-10-30 | Added testing and harness priorities beginning with paired evidence capture and the small-control stale-update failure. No new test results or harness implementation claimed. | I01 through I09 and the existing validation design. |
| 2026-10-30 | Implemented recorder, raw-evidence replay and mandatory passive input/focus audit. Preserved the initial batch as provisional after desktop activity was disclosed. Same foreground checkbox-update comparison passes 3/3 on each guest; one pre-mutation host-lock interruption was preserved and repeated. I01 remains unresolved. No production edits or full-suite pass. | [P1], [P2], [HARNESS] |
| 2026-10-30 | Accepted the current checkbox-update PASS on both platforms, closed I01 for that tested case, and moved the active work to the remaining chart rows. Historical artifacts remain unchanged. | User direction and the six controlled passing comparisons in [P1]. |
| 2026-10-30 | Added controlled Mac phrase-selection/Bold-selection functional passes, exact native/public text and font-run replay, and an AXPress route audit. Preserved initial background attempt and later host-lock interruption. Ten-second readback qualification and remaining text chain/Windows repetitions are pending. No production edits. | [T1] |
| 2026-10-30 | Completed all four Mac native text workflows and 14 functional assertions. Added six matching valid-range results and three invalid-range error-fidelity failures, isolated to host presentation. Recorded two transient missing-target reads without erasing later functional passes. Windows matching arm started; no production changes. | [T2] |
| 2026-10-30 | Confirmed the matching four Windows text workflows and 14 assertions with independent state replay. These were already supported; no new capability or product fix is claimed. Prioritized untested table/grid coverage over further known-pass repetitions. | [T2], user direction |
| 2026-10-30 | Added previously unqualified original-table relationship comparisons on both platforms: nine cell lookups, counts, ordering, indices, spans, headers, identities and containing tables pass. Confirmed Windows invalid-argument errors reach the host and are discarded; native Safari's no-value table errors are preserved. Full bounds and broader table variants remain unqualified. No production edits. | [Table comparison][E03] |
| 2026-10-30 | Resolved I05 attribution with exact native Mac setters: AXDisclosing and AXExpanded each return success but leave the custom tree collapsed; native/public state matches. Standard disclosure opens/closes functionally but immediate public reads reconfirm temporary target absence. No production fix or new Windows pass claimed. | [Disclosure attribution][E02] |
| 2026-10-30 | Added independent Mac geometry evidence after the user's resize: nine cell bounds/frames/window identities PASS with zero measured transform error. Eighteen hits preserve native coarse page results; separate native application/system-wide calls reproduce them. Precise-hit support and other poses remain unqualified; no production change or Windows pass claimed. | [Geometry comparison][E04] |
| 2026-10-30 | Added precise native-control hit evidence on Mac: all six predetermined controls return the exact target at native/public centers, with matching geometry and state. Preserved a guest-lock setup interruption; no action or product-code change. Matching Windows comparison started. | [Native controls][E06] |
| 2026-10-30 | Completed the identical six-control comparison on Windows: geometry, native/public target-descendant hits and state preservation PASS. Corrected replay selection to use the already-prescribed role with the label; original rejected replay and raw data retained, no live retry. Both platforms' full intervals pass passive input/focus audit. No production edits. | [Native controls][E06] |
| 2026-10-30 | Ran the original native sheet/dialog workflow once per platform with semantic Open/Confirm actions, independent window graphs, modal-button geometry and reviewed CU captures. Added I11 Windows owner/background omission and I12 shared missing application focus lookup. Preserved the Windows background-readback qualification gap and replay corrections. No production edits. | [Modal comparison][E05] |
| 2026-10-30 | Ran original native secure-field 11→9 setters on both VMs with safe native/public reads and monitored intervals. Closed Mac initial-mask uncertainty. Added I13 after proving Windows TextPattern supplies masks omitted publicly; retained correct empty ValuePattern mapping and client/pixel limitations. No production edits. | [Secure-field comparison][E08] |
| 2026-10-30 | Ran the identical original HTML secure-field edit on both guests. Proved Windows native mask count and ValuePattern readback, retained pixel/native-privacy qualifications, and added I14 after independently proving the Mac viewer lost its tree after resize. A separate Escape recovered it; no production edits. Viewer switching now retains right-half placement. | [HTML comparison][E07] |
| 2026-10-30 | Added paired disabled-control metadata/no-effect and read-only progress comparisons, with monitored quiet intervals and separate native action attribution. Mac AXPress success/no-effect matches native; Windows client route does not exercise native Invoke/error parity. Corrected an AXValue-only status matcher from recorded evidence; no product failure inferred from it. Documented the original HTML disabled-button oracle gap. No production edits. | [Native controls comparison][E01] |
| 2026-10-30 | Superseded clean interaction claims from that batch after the user confirmed desktop interference. Post-click audits do not cover the entire click/capture interval. Marked both client workflows provisional; retained narrowly audited read-only comparisons and separate native diagnostics. No mouse-input defect inferred; future rerun must monitor the complete sequence. | [Interference notice][E13] |
| 2026-10-31 | Published this chart alongside the reusable fixtures. Made Mac-host scope explicit and retained unpublished evidence as local artifact references. Recorded verdicts are unchanged; Windows-to-Windows remains unvalidated. | Documentation publication only. |

## Evidence index

Paths in this table are relative to the repository root and refer to local, unpublished artifacts.

| Reference | Content | Local artifact path |
|---|---|---|
| <a id="evidence-m1"></a>M1 | Recorded macOS workflow results | `.claude/accessibility-validation/runs/20261029/tester/macos-r1-traced/summary.json` |
| <a id="evidence-w1"></a>W1 | Recorded Windows workflow results | `.claude/accessibility-validation/runs/20261029/tester/windows-r1/summary.json` |
| <a id="evidence-n1"></a>N1 | Matched Windows native attribution | `.claude/accessibility-validation/runs/20261029/root/windows-r1-native-attribution-1.json` |
| <a id="evidence-f1"></a>F1 | Current refresh follow-up on macOS | `.claude/accessibility-validation/runs/20261030-refresh-fix/tester/mac-summary.md` |
| <a id="evidence-f2"></a>F2 | Current refresh follow-up on Windows | `.claude/accessibility-validation/runs/20261030-refresh-fix/tester/windows-summary.md` |
| <a id="evidence-f3"></a>F3 | Refresh fix and stale-update defect | `.claude/accessibility-validation/runs/20261030-refresh-fix/root/fix-and-remaining-issue.json` |
| <a id="evidence-r1"></a>R1 | Paired scheduling comparison and supplements | `.claude/accessibility-validation/runs/20261030-semantic-settle-regression/root/supplemental-results.json` |
| <a id="evidence-l1"></a>L1 | Large-page native versus mirror observations | `.claude/accessibility-validation/runs/20261030-large-page/tester/summary.md` |
| <a id="evidence-s1"></a>S1 | Snapshot availability observations | `.claude/accessibility-validation/runs/20261030-snapshot-stability/tester/summary.md` |
| <a id="evidence-q1"></a>Q1 | Attributed-text query timing audit | `.claude/accessibility-validation/runs/20261030-semantic-settle-regression/root/windows-checkbox-query-audit.json` |
| <a id="evidence-a1"></a>A1 | Paired source and evidence audit | `.claude/accessibility-validation/support-audit-20261030.json` |
| <a id="evidence-plan"></a>PLAN | Existing suite plan | `.claude/accessibility-validation/suite-plan.json` |
| <a id="evidence-catalog"></a>CATALOG | Conformance catalog | `.claude/accessibility-validation/capability-cases.json` |
| <a id="evidence-review"></a>REVIEW | Original case requirements | `docs/accessibility-validation/model-v1/CASE-REVIEW.md` |
| <a id="evidence-p1"></a>P1 | Controlled paired results, independently replayed | `docs/accessibility-validation/runs/20261030-pipeline-updates/clean-controls/verified-results.json` |
| <a id="evidence-p2"></a>P2 | Superseding input-audit notice for the initial batch | `docs/accessibility-validation/runs/20261030-pipeline-updates/root/input-audit-superseded.json` |
| <a id="evidence-harness"></a>HARNESS | Executable recorder, verifier and limits | `docs/accessibility-validation/harness/README.md` |
| <a id="evidence-t1"></a>T1 | Current native text-chain assertions, evidence and interruption limits | `docs/accessibility-validation/runs/20261030-chart-text/PART1.md` |
| <a id="evidence-t2"></a>T2 | Full native text chain, query comparisons and presentation-error attribution | `docs/accessibility-validation/runs/20261030-chart-text/SUMMARY.md` |
| <a id="evidence-e01"></a>E01 | Native controls comparison | `docs/accessibility-validation/runs/20261030-chart-negative/SUMMARY.md` |
| <a id="evidence-e02"></a>E02 | Disclosure attribution | `docs/accessibility-validation/runs/20261030-chart-disclosure/SUMMARY.md` |
| <a id="evidence-e03"></a>E03 | Table comparison | `docs/accessibility-validation/runs/20261030-chart-tables/SUMMARY.md` |
| <a id="evidence-e04"></a>E04 | Geometry comparison | `docs/accessibility-validation/runs/20261030-chart-geometry/SUMMARY.md` |
| <a id="evidence-e05"></a>E05 | Modal comparison | `docs/accessibility-validation/runs/20261030-chart-modal/SUMMARY.md` |
| <a id="evidence-e06"></a>E06 | Native controls | `docs/accessibility-validation/runs/20261030-chart-native-geometry/SUMMARY.md` |
| <a id="evidence-e07"></a>E07 | HTML comparison | `docs/accessibility-validation/runs/20261030-chart-html-secure/SUMMARY.md` |
| <a id="evidence-e08"></a>E08 | Native comparison | `docs/accessibility-validation/runs/20261030-chart-secure/SUMMARY.md` |
| <a id="evidence-e09"></a>E09 | production run and inconclusive-check repeats | `docs/accessibility-validation/runs/20261031-uia-host-refactor/SUMMARY.md` |
| <a id="evidence-e10"></a>E10 | Source review | `docs/accessibility-validation/runs/20261030-chart-negative/root/html-disabled-oracle-review.json` |
| <a id="evidence-e11"></a>E11 | the existing validation design | `.claude/accessibility-validation/DESIGN.md` |
| <a id="evidence-e12"></a>E12 | tester instructions | `.claude/accessibility-validation/TESTER-EXECUTION.md` |
| <a id="evidence-e13"></a>E13 | Interference notice | `docs/accessibility-validation/runs/20261030-chart-negative/root/disclosed-desktop-interruption.json` |

[M1]: #evidence-m1
[W1]: #evidence-w1
[N1]: #evidence-n1
[F1]: #evidence-f1
[F2]: #evidence-f2
[F3]: #evidence-f3
[R1]: #evidence-r1
[L1]: #evidence-l1
[S1]: #evidence-s1
[Q1]: #evidence-q1
[A1]: #evidence-a1
[PLAN]: #evidence-plan
[CATALOG]: #evidence-catalog
[REVIEW]: #evidence-review
[MAC]: ../../tools/agent_mac/appsandbox-accessibility.m
[UIA]: ../../tools/agent/accessibility_uia.c
[CODEC]: ../../tools/transport/accessibility_value_mac.h
[ATTR]: ../../tools/transport/accessibility_attributes_mac.h
[PROTOCOL]: ../../tools/transport/accessibility_protocol.h
[SESSION]: ../../src/backend_mac/vm_accessibility_session.m
[HOST]: ../../tools/accessibility_host/main.m
[ELEMENT]: ../../tools/accessibility_host/accessibility_element.m
[P1]: #evidence-p1
[P2]: #evidence-p2
[HARNESS]: #evidence-harness
[T1]: #evidence-t1
[T2]: #evidence-t2
[E01]: #evidence-e01
[E02]: #evidence-e02
[E03]: #evidence-e03
[E04]: #evidence-e04
[E05]: #evidence-e05
[E06]: #evidence-e06
[E07]: #evidence-e07
[E08]: #evidence-e08
[E09]: #evidence-e09
[E10]: #evidence-e10
[E11]: #evidence-e11
[E12]: #evidence-e12
[E13]: #evidence-e13
[UIA_HOST]: ../../src/backend_mac/vm_accessibility_uia.m
