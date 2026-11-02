# macOS Code Signing & Notarization

This document explains how the macOS app (`AppSandbox.app`) is signed, for two
audiences:

1. **Anyone cloning the repo** who just wants to build and run locally — this
   works out of the box with **no Apple Developer account and no certificates**
   (ad-hoc signing).
2. **The maintainer producing a distributable release** — this requires a
   **Developer ID Application** certificate and Apple notarization.

A human or an AI agent should be able to follow this end to end.

---

## 1. Background: what signing modes exist

| Mode | Who needs it | Cert required | Runs on other Macs? | Gatekeeper |
|------|-------------|---------------|---------------------|------------|
| **Ad-hoc** (`-`) | Any contributor | none | only the build machine | bypassed locally |
| **Apple Development** | Contributor with a free Apple ID | auto-created by Xcode | only registered/dev Macs | dev only |
| **Developer ID Application** | Maintainer shipping releases | paid Apple Developer Program | yes, any Mac | passes after notarization |

The repo is configured so the **default committed state produces an ad-hoc or
Development build** — no secrets, no team ID, nothing private is committed.
Production settings live in a **gitignored** local override file that only the
maintainer has.

---

## 2. How signing is wired into the project

Signing settings are NOT hard-coded into `project.pbxproj` (that file is
public). Instead each build configuration uses an **xcconfig base
configuration**:

```
src/app_mac/xcconfig/
  Local.xcconfig        # committed — contains only an optional include
  LocalUser.xcconfig    # gitignored — the maintainer's private settings
```

`Local.xcconfig` (committed) is just:

```
#include? "LocalUser.xcconfig"
```

The `#include?` (note the `?`) is an **optional include** — if
`LocalUser.xcconfig` does not exist, Xcode silently ignores it. This is what
lets a fresh clone build without the file present.

`project.pbxproj` deliberately omits these keys so xcconfig can supply them:

- `DEVELOPMENT_TEAM` — absent (would otherwise embed the maintainer's team ID)
- `CODE_SIGN_IDENTITY` / `CODE_SIGN_STYLE` — absent (Xcode defaults apply)
- `ENABLE_HARDENED_RUNTIME`, `OTHER_CODE_SIGN_FLAGS`,
  `CODE_SIGN_INJECT_BASE_ENTITLEMENTS` — absent

Build-setting precedence is: **`project.pbxproj` > xcconfig > Xcode defaults.**
Because the keys above are removed from `project.pbxproj`, the xcconfig (when
present) wins; when absent, Xcode's defaults (Automatic signing) take over.

### Entitlements files (committed — always present)

Two entitlements files are part of the signature and are referenced by
`CODE_SIGN_ENTITLEMENTS` in `project.pbxproj`. They are committed to the repo
(they contain no secrets) and must exist for signing to succeed:

| File | Applied to | Key entitlements |
|------|-----------|------------------|
| `src/app_mac/AppSandbox.entitlements` | main app + `AppSandboxCore` | `com.apple.security.virtualization`, `com.apple.security.hypervisor`, `network.client`/`server`, `files.user-selected.read-write`, `files.downloads.read-write`, `app-sandbox = false` |
| `tools/iso-patch-mac/iso-patch-mac.entitlements` | `iso-patch-mac` helper | `com.apple.security.virtualization`, `com.apple.security.hypervisor` |

These entitlements are **self-issued** — they require no approval or special
provisioning from Apple. Note that **no `get-task-allow`** appears in either
file; for Release it must stay absent (see
`CODE_SIGN_INJECT_BASE_ENTITLEMENTS` below), or notarization rejects the build.

---

## 3. Default path: building locally with NO Apple key

A contributor who clones the repo can build immediately.

### 3a. With Xcode (recommended for most people)

1. Install Xcode from the Mac App Store (once).
2. Open Xcode → Settings → Accounts → **+** → sign in with any Apple ID
   (free; no paid enrollment). Xcode auto-creates a "Personal Team".
3. `git clone <repo>` and double-click `AppSandbox.xcodeproj`.
4. Select the **AppSandbox** target → **Signing & Capabilities** → choose your
   Personal Team in the **Team** dropdown. (Automatic signing handles the rest.)
5. Press ▶ (⌘R). Builds, signs with your Apple Development cert, runs.

The app runs on the build machine. It will not pass Gatekeeper if copied to
another Mac — that requires the Developer ID + notarization path (section 4).

### 3b. Fully keyless (ad-hoc) — no Apple ID at all

The build also succeeds with **ad-hoc** signing (identity `-`), which needs no
certificate and no Apple ID:

```bash
xcodebuild -project AppSandbox.xcodeproj -scheme AppSandbox \
    -configuration Debug build \
    CODE_SIGN_IDENTITY="-" DEVELOPMENT_TEAM="" \
    CODE_SIGNING_REQUIRED=YES CODE_SIGNING_ALLOWED=YES
```

The resulting bundle reports:

```
Signature=adhoc
TeamIdentifier=not set
```

An ad-hoc binary runs only on the machine that built it (and others that
explicitly trust it). This is the lowest-friction way to verify a build
compiles and links correctly.

> The Virtualization entitlements (`com.apple.security.virtualization`,
> `com.apple.security.hypervisor`) are **self-issued** — they do not require
> any special grant from Apple. They are declared in
> `src/app_mac/AppSandbox.entitlements` and apply in every signing mode.

---

## 4. Production path: Developer ID + notarization (maintainer only)

This is what produces a build any user can download and run without warnings.

### Prerequisites (one-time)

1. **Paid Apple Developer Program** membership.
2. A **Developer ID Application** certificate installed in the login keychain.
   Confirm with:
   ```bash
   security find-identity -v -p codesigning
   # Should list:  "Developer ID Application: <Your Org> (TEAMID)"
   ```
3. An **app-specific password** for notarization, stored in the keychain as a
   named profile (so it never appears on a command line or in history):
   ```bash
   xcrun notarytool store-credentials appsandbox-notary \
       --apple-id "you@example.com" \
       --team-id  "YOURTEAMID" \
       --password "xxxx-xxxx-xxxx-xxxx"   # app-specific password from appleid.apple.com
   ```
   Generate the app-specific password at <https://appleid.apple.com> →
   Sign-In and Security → App-Specific Passwords. It is NOT your Apple ID
   password.

### Create the private override file

Create `src/app_mac/xcconfig/LocalUser.xcconfig` (gitignored — never commit it):

```
DEVELOPMENT_TEAM = YOURTEAMID

CODE_SIGN_STYLE[config=Release]                  = Manual
CODE_SIGN_IDENTITY[config=Release]               = Developer ID Application
ENABLE_HARDENED_RUNTIME[config=Release]          = YES
OTHER_CODE_SIGN_FLAGS[config=Release]            = --timestamp
CODE_SIGN_INJECT_BASE_ENTITLEMENTS[config=Release] = NO
```

What each line does and why notarization requires it:

- **`DEVELOPMENT_TEAM`** — selects which team's certs to use.
- **`CODE_SIGN_IDENTITY = Developer ID Application`** — the distribution cert
  type (not "Apple Development"). Only this type is accepted by Gatekeeper for
  downloaded apps.
- **`CODE_SIGN_STYLE = Manual`** — Developer ID signing outside the Mac App
  Store uses manual signing (no provisioning profile needed).
- **`ENABLE_HARDENED_RUNTIME = YES`** — **mandatory** for notarization. Adds
  the `runtime` flag (`flags=0x10000`) to the code signature.
- **`OTHER_CODE_SIGN_FLAGS = --timestamp`** — **mandatory**. Embeds an Apple
  secure timestamp. Without it notarization fails with
  *"The signature does not include a secure timestamp."*
- **`CODE_SIGN_INJECT_BASE_ENTITLEMENTS = NO`** — stops Xcode from
  auto-injecting `com.apple.security.get-task-allow`, a debug entitlement that
  notarization **rejects**. Debug builds keep it (so the debugger can attach);
  Release builds must not have it.

The `[config=Release]` qualifier means these only apply to Release. Debug
builds stay on Automatic + Apple Development so day-to-day development and
debugging are unaffected.

### Create the private notarization config

The release script reads notarization details from a git-ignored JSON file
(the macOS analogue of the Windows `*.local.json` convention). Create
`tools/sign/mac-signing.local.json` — it matches the `tools/sign/*.local.*`
ignore rule, so it is never committed:

```json
{
  "notaryKeychainProfile": "appsandbox-notary",
  "appleId": "you@example.com",
  "teamId": "YOURTEAMID",
  "signingIdentity": "Developer ID Application: Your Org (YOURTEAMID)"
}
```

- `notaryKeychainProfile` — the `notarytool` keychain profile created in the
  prerequisites. With it set, the app-specific password stays in the keychain
  and never touches the JSON.
- `appleId` / `teamId` / `signingIdentity` — informational; the script verifies
  the built app's identity against `expectedIdentity` from the committed config.
- To avoid the keychain entirely, omit `notaryKeychainProfile` and instead add
  `"appSpecificPassword": "xxxx-xxxx-xxxx-xxxx"` alongside `appleId`/`teamId` —
  the script will pass them to `notarytool` directly.

The committed, secret-free counterpart is `tools/sign/mac-release-config.json`
(project, scheme, paths, expected identity). You normally don't edit it.

### Build, notarize, staple — one command

```bash
tools/sign/make-release-mac.sh
```

This script runs the full pipeline:

1. **Build** Release (`xcodebuild`, signing from `LocalUser.xcconfig`). The build
   itself stamps the version from `Directory.Build.props` into the bundle (see §6).
2. **Verify** the signature (`codesign --verify --deep --strict`) and check the
   identity matches `expectedIdentity`.
3. **Zip** a working copy (`.notarize-submit.zip`) for submission — not shipped.
4. **Notarize** via `notarytool submit --wait`. This is **recursive**: Apple
   validates every nested Mach-O in the bundle in one submission — the framework
   plus the `iso-patch-mac`, `appsandbox-agent`, and `appsandbox-clipboard`
   helpers. They are not (and cannot be) notarized separately (see §5).
5. **Staple** the ticket to the `.app` (`stapler staple`) so it verifies offline.
6. **Gatekeeper check** (`spctl`) — expects `accepted / Notarized Developer ID`.
7. **Package** — wipe and recreate `packageDir` (default
   `bin/Release/release_package/`) and write only the stapled zip into it, named
   `<productName>-<version>-<os>-<platform>.zip` (e.g.
   `AppSandbox-0.1.1-mac-arm64.zip`), matching the Windows
   `make-release.ps1` convention (`AppSandbox-0.1.1-win-x64.zip`). The version
   comes from `Directory.Build.props`. That folder is the public deliverable and
   contains **nothing else** — no dSYMs, no loose binaries, no build scratch. The
   working submission zip is deleted.

For a quick local Developer ID build without contacting Apple:

```bash
tools/sign/make-release-mac.sh --no-notarize   # build + sign + verify + package (not notarized)
```

### What the script does under the hood (manual equivalent)

```bash
xcodebuild -project AppSandbox.xcodeproj -scheme AppSandbox -configuration Release build
ditto -c -k --keepParent bin/Release/AppSandbox.app /tmp/AppSandbox.zip
xcrun notarytool submit /tmp/AppSandbox.zip --keychain-profile appsandbox-notary --wait
xcrun stapler staple bin/Release/AppSandbox.app
spctl -a -t exec -vvv bin/Release/AppSandbox.app   # -> accepted / Notarized Developer ID
```

If notarization returns `Invalid`, read the detailed log:

```bash
xcrun notarytool log <submission-id> --keychain-profile appsandbox-notary
```

Notarization wall-time varies with Apple's queue: usually under a few minutes,
occasionally much longer. `--wait` polls until done; if the poll connection
times out, the submission is still processing — re-check with:

```bash
xcrun notarytool info <submission-id> --keychain-profile appsandbox-notary
```

---

## 5. What gets signed

The app embeds several binaries; **all** must carry the same identity, hardened
runtime, and timestamp or notarization fails. Xcode signs them in the correct
nested order automatically (inner items first, then the outer bundle):

```
AppSandbox.app/
  Contents/MacOS/AppSandbox                                  (main binary)
  Contents/Frameworks/AppSandboxCore.framework               (embedded framework)
  Contents/Resources/iso-patch-mac                           (host helper)
  Contents/Resources/agent_mac/appsandbox-agent              (guest helper)
  Contents/Resources/agent_mac/appsandbox-clipboard          (guest helper)
```

Verify every binary at once:

```bash
for t in Contents/MacOS/AppSandbox \
         Contents/Frameworks/AppSandboxCore.framework \
         Contents/Resources/iso-patch-mac \
         Contents/Resources/agent_mac/appsandbox-agent \
         Contents/Resources/agent_mac/appsandbox-clipboard; do
  echo "== $t =="
  codesign -dvvv "bin/Release/AppSandbox.app/$t" 2>&1 \
    | grep -E "Authority=Developer ID|flags=0x10000|Timestamp="
done
```

Each should show `Developer ID Application: <Org>`, `flags=0x10000(runtime)`,
and an Apple `Timestamp=`.

**Notarization covers all of these in one submission.** The notary service is
recursive — submitting the `.app` (zipped) validates every nested Mach-O above.
The three helpers and the framework are therefore notarized without any separate
submission. A notarization *ticket*, however, can only be **stapled to a
container** (`.app`, `.dmg`, `.pkg`), never to an individual nested executable:

- `iso-patch-mac` runs from inside the bundle, so the `.app`'s stapled ticket
  covers it.
- `appsandbox-agent` / `appsandbox-clipboard` are injected into a **guest VM**
  during install. What lets them run there is their **Developer ID signature**
  (the staging helper removes inherited `com.apple.quarantine` from each
  installed copy) — not a stapled ticket,
  which can't travel into the guest. So they need no separate notarization or
  stapling.

---

## 6. Versioning and project-wide build settings

### Version (single source of truth)

`Directory.Build.props` (the four `AsbVersion*` numbers) is the **only** place a
version is ever set — the same source the Windows build uses. The macOS build
stamps it in automatically at build time:

```
Directory.Build.props   (AsbVersionMajor/Minor/Patch/Revision)
   └─ "Stamp version" build phase (tools/sign/stamp-version.sh, every target,
      every build — GUI, xcodebuild, and Archive) reads the numbers via
      tools/sign/asb-version.sh and writes them into the BUILT bundle's Info.plist:
         CFBundleShortVersionString = <major.minor.patch>
         CFBundleVersion            = <major.minor.patch.revision>
```

A build phase (not an xcconfig) is used deliberately: an xcconfig is read
*before* build phases run, so a generated-xcconfig approach couldn't update the
current build. Stamping the built `Info.plist` in a phase runs on every build
and needs nothing committed but `Directory.Build.props`. `project.pbxproj` sets
no `MARKETING_VERSION` / `CURRENT_PROJECT_VERSION`; the source `Info.plist`s leave
those keys as `$(…)` placeholders that the stamp overwrites.

**To bump the version:** edit the four numbers in `Directory.Build.props` — and
nothing else. The next build (Xcode GUI or `make-release-mac.sh`) stamps it in.
`tools/sign/asb-version.sh [short|full]` prints the current version (the release
script uses it to name the zip).

### Project-wide build settings

These live in `project.pbxproj` (public — they contain no secrets):

- **`ARCHS = arm64`**, **`ONLY_ACTIVE_ARCH = YES`** (Release) — Apple Silicon
  only. Virtualization of macOS/Linux guests requires Apple Silicon, and the
  Mac-only VZ classes do not compile for x86_64.
- **`MACOSX_DEPLOYMENT_TARGET = 26.0`** (app, framework, host helper) — the app
  requires macOS 26 (Tahoe). Guest-side helpers stay at 13.0 since they target
  the guest OS, not the host.
- **`GCC_SYMBOLS_PRIVATE_EXTERN = NO`** on `AppSandboxCore` Release — keeps the
  framework's exported symbols visible so the main app can link them after
  Release strip. (Symbol names are not sensitive; Objective-C runtime metadata
  exposes them regardless.)

---

## 7. Security guarantees / what is NOT committed

The public repo never contains:

- The team ID — supplied only by the gitignored `LocalUser.xcconfig`.
- The Apple ID, app-specific password, or notary profile name — these live in
  the keychain, never in the repo.
- The Developer ID certificate or private key — keychain only.
- `get-task-allow` or other debug entitlements in Release.

`.gitignore` excludes both private files —
`src/app_mac/xcconfig/LocalUser.xcconfig` and `tools/sign/mac-signing.local.json`
(the latter via the `tools/sign/*.local.*` rule). Confirm at any time:

```bash
git check-ignore src/app_mac/xcconfig/LocalUser.xcconfig
git check-ignore tools/sign/mac-signing.local.json
git grep -i "YOURTEAMID\|your-apple-id\|app-specific-password"   # should return nothing
```

A notarized, stapled `.app` does expose, by design and unavoidably, the public
identity baked into any signed Mac app: the `TeamIdentifier`, the
`Developer ID Application: <Org>` authority name, and the bundle ID. These are
public information — Apple's signing model requires them to be verifiable.

---

## 8. Quick reference

```bash
# Contributor — build & run locally (Xcode handles signing)
open AppSandbox.xcodeproj      # then set Team, press Run

# Contributor — keyless ad-hoc build from CLI
xcodebuild -scheme AppSandbox -configuration Debug build \
    CODE_SIGN_IDENTITY="-" DEVELOPMENT_TEAM=""

# Maintainer — full production release (build, sign, notarize, staple, zip)
tools/sign/make-release-mac.sh

# Maintainer — local Developer ID build only (no notarization)
tools/sign/make-release-mac.sh --no-notarize

# Bump version (both platforms): edit Directory.Build.props — that's it.
# The next build stamps it in. (tools/sign/asb-version.sh prints the version.)
```

---

## 9. Required inputs at a glance (checklist)

### Files

| File | Source | Needed for | Present by default? |
|------|--------|-----------|---------------------|
| `AppSandbox.xcodeproj` | committed | every build | yes |
| `Directory.Build.props` | committed | version source of truth | yes |
| `src/app_mac/AppSandbox.entitlements` | committed | app + framework signature | yes |
| `tools/iso-patch-mac/iso-patch-mac.entitlements` | committed | helper signature | yes |
| `src/app_mac/xcconfig/Local.xcconfig` | committed | optional-include hook for signing overrides | yes |
| `tools/sign/asb-version.sh` | committed | reads version from Directory.Build.props | yes |
| `tools/sign/stamp-version.sh` | committed | build phase that stamps the bundle version | yes |
| `tools/sign/make-release-mac.sh` | committed | release pipeline | yes |
| `tools/sign/mac-release-config.json` | committed | release script settings (no secrets) | yes |
| `src/app_mac/xcconfig/LocalUser.xcconfig` | **maintainer creates** (gitignored) | production identity/team | **no — create for Release** |
| `tools/sign/mac-signing.local.json` | **maintainer creates** (gitignored) | notarization creds/profile | **no — create for Release** |

A contributor needs only the committed files. Ad-hoc and Personal-Team builds
work with neither `LocalUser.xcconfig` nor `mac-signing.local.json`.

### Apple account / keychain (production only)

| Requirement | How to obtain / verify |
|-------------|------------------------|
| Paid Apple Developer Program membership | <https://developer.apple.com> enrollment |
| **Developer ID Application** certificate in login keychain | `security find-identity -v -p codesigning` lists it |
| Team ID | shown in the cert name `(TEAMID)`; goes in `LocalUser.xcconfig` |
| Apple ID enrolled in that team | used by `notarytool store-credentials` |
| App-specific password | generate at <https://appleid.apple.com> → App-Specific Passwords |
| `notarytool` keychain profile (`appsandbox-notary`) | created once via `xcrun notarytool store-credentials …` |

### None of these are needed for a local build

A person or agent who only wants to compile/run the app needs **none** of the
account items and does **not** create `LocalUser.xcconfig`. Build with Xcode
(set a free Personal Team) or ad-hoc from the CLI (`CODE_SIGN_IDENTITY="-"`).
