# VM accessibility support

**Canonical support chart for macOS and Windows guests.** Reference this file for support claims, known failures and testing priorities. Update this same file after relevant testing, attribution or implementation changes.

Last updated: **2026-10-31**. Branch: `mac-accessibility`. Latest scoped production test: `f23d89fe0872686e4b74533e5a27596dfafbbc9c`. Earlier broad source audit: `a0d0960a61f653804233b812de2b451ca199ffc6`.

**Release qualification: NOT VALIDATED on either platform.** Common interactions have observed passes, but complete accessibility support and Mac/Windows parity are not established. Small trees can also fail. There is no defensible percentage of all realistic application support.

The capability chart is the current assessment. The workflow table preserves the recorded results from the 20261029 run, whose observations extend into October 30. Later follow-ups refine scope and attribution without rewriting those results. No new live testing was performed to initialize this chart. The 17 capability groups, 51 workflows and 45 planned conformance families are different inventories and must not be combined into a pass percentage.

Both guest columns describe tests through a **macOS host**. **Windows-to-Windows is not validated**; the Windows host UIA proxy is separate work. The [test fixtures](../../tools/accessibility-fixtures/README.md) are available for that testing.

This chart is tracked. Raw captures, test harnesses and local reports are not published. Evidence references below identify those local artifacts without linking to absent repository files.

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
| C01 Metadata and hierarchy | Common roles/labels observed; disabled enabled=false and two progress value/range/read-only comparisons PASS. Complete property/type fidelity unverified. | Matching disabled/progress comparisons PASS; status text preserved in AXDescription. Full UIA property coverage absent. | [Native controls comparison][E01], [M1], [W1], [A1] |
| C02 Buttons and toggle states | Existing semantic passes retained. Latest disabled client workflow PROVISIONAL after disclosed desktop interference; separate native AXPress returns success with no effect. | Existing button/toggle passes retained. Latest disabled client workflow PROVISIONAL; no AXPress advertised/traced, native Invoke throws ElementNotEnabled. Equivalent semantic error path unqualified. | [Native controls comparison][E01], [M1], [W1], [F1], [F2] |
| C03 Text values and numeric setters | Unicode/text setters and native slider cases pass. Read-only progress 0.65/0..1 and level 40/0..100 match native. | Same progress values/ranges/read-only flags PASS, alongside existing text/slider passes. | [Native controls comparison][E01], [M1], [W1], [R1] |
| C04 Increment and decrement | Native stepper passes; prescribed HTML action client blocked. | Native stepper no-op reproduced locally; HTML action client blocked despite exposed capability. | [M1], [W1], [N1] |
| C05 Menus tabs dialogs and disclosure | Partial: standard disclosure has functional passes; semantic route unproved. **Custom expansion no-op reproduced with both native AXDisclosing and AXExpanded setters; viewer state matches native.** | Partial: working paths; popup completion failures reproduced natively. | [Disclosure attribution][E02], [M1], [W1], [N1], [R1] |
| C06 Text selection replacement and formatting | **All four native text-chain workflows observed PASS**, including selected replacement, inherited bold, menu italic and Command+B. Repetitions/timing qualification incomplete; HTML selection client blocked. | **The same four native workflows observed PASS**, confirming historical passes. Windows readbacks meet the batch deadline; full repetition/qualification remains incomplete. | [T2], [M1], [W1] |
| C07 Parameterized queries and text markers | Six valid string ranges and attributed text/font runs match native. **Three invalid-range errors are lost in host presentation.** Geometry, other types and markers remain unqualified. | Nine valid table-cell queries match native. **Three invalid-coordinate errors reach the host and lose their meaning in presentation.** Text-query, geometry and marker equivalence remain unqualified. | [T2], [Table comparison][E03], [A1], [MAC], [UIA], [CODEC] |
| C08 Table and grid relationships | **Original 3×3 HTML table:** relationships PASS; all nine cell bounds also PASS at the user's resized viewer pose. Other variants and complete side-effect qualification remain incomplete. | **Same table:** matching relationships PASS; native column information preserved without structural column objects. Invalid requests reject, but error meaning is lost. Other variants and full bounds remain unqualified. | [Table comparison][E03], [Geometry comparison][E04], [M1], [W1], [A1] |
| C09 Explicit offscreen reveal before selection | Client blocked in examined list/outline cases; native/proxy capabilities exposed. | Same gap; automatic reveal-and-press does not prove the explicit sequence. | [M1], [W1] |
| C10 Multiselection and virtualization | Selection forwarding implemented where advertised; prescribed multiselection prerequisite blocked. | Known-item selection/realization implemented; multiselection unverified; ItemContainer discovery unimplemented. | [M1], [W1], [UIA] |
| C11 Focus modal return geometry and hit testing | Six native controls **and the modal Confirm button** pass bounds/frame/window identity and precise center hits. Sheet ownership and focused flags match native. **Application focused-element lookup fails** despite a correctly focused mirrored control. Original nine HTML cell bounds pass. | Same native geometry and hit assertions pass. **The modal's owner window and background controls are missing from the public tree. Application focused-element lookup also fails**, while individual focused flags match native in foreground observations. | [Modal comparison][E05], [Native controls][E06], [HTML geometry][E04] |
| C12 Secure input | Original native 11→9 workflow PASS. **HTML native/public/CU masks and pixels also pass after explicit recovery from I14.** Tested public reads expose no plaintext; native HTML RTF privacy remains unqualified. | **HTML 11→9 native/public/CU masks PASS through ValuePattern.** The native fixture still loses TextPattern-only mask readback (I13). Secure/writable flags retained; exact pixel counts unqualified. | [HTML comparison][E07], [Native comparison][E08] |
| C13 Guest-originated updates | Checkbox value update: **3/3 monitored PASS**. Other event-update variants remain unrun. | Same checkbox value update: **3/3 monitored PASS**. Other event-update variants remain unrun. | [P1], [P2] |
| C14 Large trees and availability during refresh | **Bridge failures:** native content can be absent publicly, including small controls during refresh. **After a viewer resize, the page tree remained absent in repeated observations despite native controls and a non-busy public root; one Escape restored it (I14).** | All 1000 fixture targets exposed; general completeness, latency and truncation behavior unqualified. Matching I14 resize sequence unrun. | [HTML comparison][E07], [T2], [L1], [S1], [W1] |
| C15 Events and announcements | Selected snapshot-derived notifications; full native event stream not relayed. Announcement parity unqualified. | Same host notification subset; complete UIA event semantics not relayed/qualified. | [HOST], [ELEMENT], [A1] |
| C16 Recovery expiry and concurrent actions | Recovery observed; timeout/error, no-late-action and overlap guarantees incomplete. | Recovery observed; retained client target rejected before bridge timeout; other race guarantees incomplete. | [M1], [W1], [R1] |
| C17 Refresh latency and performance | Selected setters improve after shared scheduling change; no general latency guarantee. | Selected setters improve; repeated slow attributed-text query unattributed; no general latency guarantee. | [F1], [F2], [R1], [Q1] |

## Production refactor follow-up — 2026-10-31

The [production run and inconclusive-check repeats][E09] used the rebuilt, restarted Release headless daemon, normal Windows helper, IVSHMEM and public Mac viewer. Six independent native/public text-state comparisons PASS after repeating an interrupted selection check. A supplemental button/disabled-button retry produced the expected count changes without a click-away interruption. Lists recovered after an updating interval, but explicit reveal-before-select remains client blocked and exact latency is unqualified.

Common native controls and five HTML control cases also produced their expected observed results. Existing stepper, popup, modal-owner and native secure-mask limitations remain. This is a scoped Windows-guest functional follow-up, not a full rerun of the historical 51 workflows; it does not promote unrun browser, macOS-guest, stress, Hyper-V or release-distribution cases to current-build passes. The capability and historical workflow tables retain their previously documented scopes.

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
| Protected contents | Masked AXValue preserved; secure text/character metadata and parameterized reads restricted. Some unadvertised native Safari range queries return masks but are denied publicly. | Masked/empty ValuePattern preserved; secure TextPattern readback excluded. Native-provided masks available only through TextPattern are lost (I13). |
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
| I10 Native query errors discarded | **Observed on both guest platforms** | **Our presentation-layer defect** | Native Mac invalid-range `-25201` and Windows invalid-cell `-2147024809` errors reach the host, but its parameterized getter returns nil and public AX reports `-25212`. Three invalid requests fail error fidelity on each platform; six valid Mac ranges and nine valid Windows cell lookups pass. [T2], [Table comparison][E03] |
| I11 Owned modal drops background window | Windows | **Our bridge boundary mismatch; loss stage unisolated** | Native UIA exposes the visible main window with its modal child; Win32 independently confirms ownership. The complete public tree contains only the dialog. The same Mac sheet preserves owner/background hierarchy. Open/Confirm and modal geometry still pass. [Modal comparison][E05] |
| I12 Application focused-element lookup missing | Both | **Our public focus relationship fails** | In monitored foreground observations, native AX/UIA identifies a focused element and the public element carries the matching focused flag, but host application `AXFocusedUIElement` returns `-25212`/no value. Data reaches the mirrored element; the exact failure in the public focus-query route remains unisolated. [Modal comparison][E05] |
| I13 Secure TextPattern masks omitted | Windows | **Our secure-text mapping gap** | Original native control supplies 11→9 masks through TextPattern while ValuePattern stays empty; public masked readback is missing. The HTML control supplies masks through ValuePattern too, and that path passes. Current secure TextPattern exclusion remains the gap; the HTML pass does not close it. [Native comparison][E08], [HTML comparison][E07] |
| I14 Guest tree lost after viewer resize | macOS; Windows matched case unrun | **Our end-to-end bridge availability failure; exact loss stage unisolated** | Native AX supplies the field before/after a complete public scan with no target. Foreground/input audit passes; a later public root reports not busy. One separate Escape restores the tree without changing the field. Preserve/recover snapshots across geometry changes and qualify the same resize sequence on both guests; do not treat later setter success as closing this failure. [HTML comparison][E07] |

## Next testing and harness work

The checkbox-update row and four native text workflows have passing observations on **both** guest platforms. The original HTML table now has paired passing evidence for counts, contents, nine cell queries, indices, spans, headers, aliases and containing-table relationships. Query error loss is proven on both guest paths. The active goal is to fill remaining chart gaps; more table variants, bounds, query families and unresolved failures take priority over repeating known passes. Do not keep retesting a passing row instead of advancing coverage.

Reuse the existing fixtures, native observers, suite plan and result schema. The [recorder][HARNESS] preserves command output, exact code/tool bytes, binary identities, bounded scheduled reads and input/focus evidence; its checkers derive scoped results from raw artifacts. OpenAI computer use remains the scored workflow driver. Independent native/public AX observations establish bridge fidelity; matched native diagnostic actions establish attribution and never count as client activation passes.

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
| `cap/secure-text` | 4 | PARTIAL: native workflow PASS; HTML 11→9 masks/pixels PASS after I14 recovery; tested public reads contain no plaintext; native RTF and broader privacy variants unqualified | PARTIAL: HTML 11→9 masks/readback PASS through ValuePattern; native TextPattern-only public readback FAIL; pixel counts and broader privacy variants unqualified |
| `cap/session-lifecycle` | 4 | UNRUN | UNRUN |
| `cap/provider-errors` | 4 | PARTIAL: invalid-range error preservation FAIL; other variants pending | PARTIAL: invalid-cell error preservation FAIL; native invalid-argument error arrives at host then becomes no-value. Other variants pending. |
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
8. **Retain operational preferences.** Built-in OpenAI computer use drives scored workflows; independent native observations establish attribution. Production fixes remain general purpose. When switching VMs, position the viewer on the right half of the host screen so the terminal stays visible; account for any later user resize in the next observation. This chart does not require VM shutdown, production edits or committing diagnostics.

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
