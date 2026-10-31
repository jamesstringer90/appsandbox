# Accessibility fixtures

Standalone applications used to exercise the accessibility tree proxy. The Windows
fixture uses WPF and WinForms UI Automation providers; the macOS fixture uses native
AppKit controls. The HTML pages work on either platform. Their control behavior and
test data are preserved from the fixtures used for Mac-host validation.

## Windows

Build on Windows with PowerShell and .NET Framework 4.8; no NuGet packages or Visual
Studio installation are needed. Run these commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\accessibility-fixtures\windows\build.ps1
& .\tools\accessibility-fixtures\windows\build\NativeControlLab.exe
```

The output is AnyCPU. Copy both `NativeControlLab.exe` and
`NativeControlLab.exe.config` to the test machine, or build there. Launch it in the
logged-in desktop session. An SSH or service session does not provide that desktop.

For Windows-to-Windows testing, use the same executable on the Windows host for a
direct UIA baseline and inside the guest for testing through the host's proxy.
Compare control names, values, supported patterns, action results, focus, and
updates with the direct baseline. Use matching display scaling and browser versions
when comparing bounds or HTML controls. The fixture itself needs no Mac component.

## macOS

Build with Xcode Command Line Tools on macOS 13 or later:

```sh
tools/accessibility-fixtures/macos/build.sh
open tools/accessibility-fixtures/macos/build/ASBNativeControlLab.app
```

Both native fixtures accept `--background` to avoid taking focus at launch. Close
and reopen the application to reset it.

## Browser

Open `web/asb-control-lab.html` in the test machine's browser. It contains buttons,
toggles, text fields, menus, tabs, a dialog, a table, long lists and nested scrolling.
Open `web/asb-stress-1000-grouped.html` for the static 1,000-target tree. Both pages
work from disk without a server or network access. Reload to reset their state.

## Checks

| Fixture | Operation and expected result |
|---|---|
| Native Controls | Press **Count press** once; the status reports `Count press 1`. Disabled **Unavailable** must not change it. |
| Native Controls | Change Meal included, Notifications and seat choice; compare their state and status with direct native behavior. |
| Native Controls | Set Volume from 4 to 7; read progress as 65% and disk usage as 40%. |
| Native Controls | Open and confirm the sheet/dialog; the status reports `Trip confirmed`. |
| Native Controls | Press **Delayed update**; after three seconds the status changes without another input. |
| Native Lists and outline | Reveal/select Reservation 072; expand Day 24 and select an activity. Test multiselection separately. There are 120 reservations and 30 days with three activities each. |
| Native Text editing | Set a Unicode title. In Trip notes, select `quiet morning`, apply bold, replace it with `calm afternoon`, then apply italic through Format > Font. Only the selected phrase should change. |
| HTML controls | Count press increments once; the mixed checkbox starts indeterminate; test nested and offscreen controls without manually scrolling first. |
| HTML table | Accommodation table has three rows including its header, and three columns. Compare cell contents, indices and header relationships. |
| HTML stress | Check for all targets `00001` through `01000`; activate **STRESS TARGET 01000** and read `STRESS RESULT 01000`. |

These are manual fixtures, not a pass/fail runner. A control can expose a pattern
that its native provider fails to execute: the Windows stepper and popup completion
have shown this in direct tests. Check the native baseline before attributing a
failure to the proxy. Native text formatting is the reference case; HTML slider and
rich-text formatting were excluded from the Mac comparison because of native Safari
limitations. Explicit reveal-before-select and automatic reveal-and-press are separate
checks. Builds are local test artifacts and are not included in release packages.
