# AppSandbox API, MCP, and Python SDK

Create and manage desktop VMs, capture screenshots, send mouse and keyboard input,
run commands over SSH, and subscribe to status events. The local HTTP/JSON API,
MCP server, and dependency-free Python SDK (`asb.py`) are available with the GUI
or in headless mode. They operate on the same VMs and viewers.

```python
import asb

c = asb.connect()
print(c.version())
for vm in c.list():
    print(vm["id"], vm["name"], vm["state"])
```

## Start AppSandbox

**The regular AppSandbox GUI automatically starts both the API and MCP server.**
Open AppSandbox normally to use either service. Headless mode is optional and
runs the same services without the management window.

To run in headless mode, launch the host from an elevated terminal.

Windows: open PowerShell or Windows Terminal with **Run as administrator**, then
run the command below. The headless host must run with administrator privileges
to create and run VMs and serve the local API/MCP endpoints.

```powershell
appsandbox.exe --headless
```

macOS:

```sh
sudo /Applications/AppSandbox.app/Contents/MacOS/AppSandbox --headless
```

| Host | Requirements |
|---|---|
| Windows | Windows 11 build 22000 or newer, the matching x64 or ARM64 AppSandbox build, administrator privileges, and the Virtual Machine Platform feature |
| macOS | macOS 27 or newer on Apple silicon, virtualization support, and elevation through `sudo` for a direct headless launch |

On macOS, a `sudo` launch uses the invoking user's VM registry and restore-image
cache. For native macOS VMs, launch from the logged-in user's session with their
login keychain unlocked. A separate root security session can prevent macOS
installation or startup. Headless mode does not remove this requirement.

VMs run graphical guest desktops; screenshots and desktop automation do not
require an open viewer window.

## Connect and discover capabilities

The services listen on `127.0.0.1`. AppSandbox writes the selected endpoint and a
per-run bearer token to `host.json`:

| Host | Discovery file |
|---|---|
| Windows | `%ProgramData%\AppSandbox\host.json` |
| macOS | `~/Library/Application Support/AppSandbox/host.json` |

The file contains `endpoint`, `port`, `token`, `pid`, `version`, `apiVersion`,
`mcpEndpoint`, and `mcpTransport`. Read it after starting AppSandbox so clients
use the current connection details.

### Python

Copy `asb.py` next to your script or add `tools/headless-api` to your Python
path. It uses only the Python standard library.

```python
import asb

c = asb.connect()
print(c.host())
print(c.version()["capabilities"])
```

`asb.connect()` discovers the running host and checks that it responds. An
explicit connection is also supported with `asb.connect(endpoint=..., token=...)`.

Query methods return parsed JSON and raise `asb.ApiError` for HTTP failures.
The exception exposes `status` and `body`. A missing resource raises
`asb.ApiNotFound`, which also inherits from `KeyError`. Action methods return
`(http_status, body)`; callers should check the status before using the result.

### HTTP

Use the discovery file's `endpoint` with routes under `/v1`. Send
`Authorization: Bearer <token>` on authenticated requests and
`Content-Type: application/json` when sending a JSON body. `GET /v1/version`
is available without authentication. Send `{}` for POST or PUT operations
that have no options.

`GET /v1/operations` returns the operation catalogue, including each operation's
name, HTTP method, path, and input schema. Python exposes it through
`c.operations()` and can invoke an operation with
`c.operation(operation_name, **arguments)`.

### MCP

Configure an MCP client for Streamable HTTP using the discovery file's
`mcpEndpoint` and an `Authorization: Bearer <token>` header. The MCP endpoint is
`/mcp` on the same listener as the HTTP API.

| Tool group | Tools |
|---|---|
| Discovery | `api_version`, `host_status`, `vm_list` |
| VMs | `vm_status`, `vm_create`, `vm_edit`, `vm_start`, `vm_shutdown`, `vm_stop`, `vm_delete` |
| Templates | `template_list`, `template_status`, `template_create`, `template_delete` |
| Viewer | `display_status`, `display_open`, `display_close` |
| Screenshots | `screenshot` |
| Desktop input | `input_status`, `input_request_control`, `input_wait_for_control`, `input`, `input_finish` |
| SSH | `ssh_info`, `ssh_open`, `ssh_read`, `ssh_write`, `ssh_close` |
| Snapshots | `snapshot_list`, `snapshot_take`, `snapshot_delete`, `snapshot_rename`, `snapshot_delete_branch` |
| Events | `events_subscribe`, `events_read`, `events_unsubscribe` |
| Host | `host_shutdown` |

MCP tools use the API's operations and validation. Results include structured
JSON and text. Screenshots include an MCP image content block with structured
metadata. SSH reads include readable stdout/stderr text as well as the
base64-encoded bytes in `structuredContent`. Operation failures set `isError`
and include an error object; malformed MCP requests use JSON-RPC errors.

### Port configuration

The default shared API/MCP port is `51152`. To select a port, edit `ApiPort` in
the existing `[Settings]` section of `vms.cfg`, next to `host.json`, while
AppSandbox is closed:

```ini
[Settings]
ApiPort=51152
```

The value must be an integer from `1` to `65535`. It is read at startup. If the
port is occupied, AppSandbox increments it by one until it finds an available
port or reaches `65535`. The selected port stays fixed for that run. The API
and MCP URLs are printed in the application log and written to `host.json`.

### Platform support

`version()["capabilities"]` advertises `snapshots`, `templates`, `screenshots`,
`input`, `sshSessions`, `gpuSelection`, `guestOs`, `templateGuestOs`, and
`networkModes`. `guestOsNames` maps the guest identifiers to display labels.
Choose operations and guest types using these capabilities.

| Feature | Windows host | macOS host |
|---|---|---|
| Guest operating systems | Windows 11, Ubuntu 26.04 LTS | Windows 11, macOS 27 |
| Templates | Windows guests | Windows and macOS guests |
| Snapshots and branches | Supported | Unsupported |
| Screenshots and desktop input | Supported | Supported |
| Streaming SSH and SSH-key deployment | Supported | Supported |
| Open/close VM viewer | Supported | Supported |
| GPU selection by `gpuId` | Supported | Unsupported |
| Network modes | `0` None, `1` NAT, `2` External, `3` Internal | `1` NAT |

The OS identifiers are `windows11`, `ubuntu2604lts`, and `macos27`, selecting
Windows 11, Ubuntu 26.04 LTS, and macOS 27 respectively. No other Linux distribution
or Ubuntu version is supported. Use `osName` and `guestOsNames` for display labels.

Windows guests support direct Unicode input through the AppSandbox guest input
helper. Native macOS and Ubuntu guests support physical keys and text
representable by the host keyboard layout. Native macOS accepts the physical
keys covered by its key mapping. Unsupported host or guest features return
`501 not_supported`; malformed arguments return `400 invalid_arg`.

Windows and Ubuntu guest screenshots require their AppSandbox guest display
components. Native macOS screenshots use the VM display. Screenshot capture,
SSH, and desktop input can be used with the viewer open or closed.

## VM identity and status

VM IDs have the form `vm-0123456789abcdef`; template IDs have the form
`template-0123456789abcdef`. VM and template operations use their respective
resource types. `list()` returns VMs, and `templates()` returns completed
templates. A template build is queried using its template ID.

Python VM methods accept a name or an explicit `vm_id=` keyword. Template
methods accept a name or `template_id=`. MCP operations use explicit `vm_id`
or `template_id` arguments. For example:

```python
vm_id = c.vm_id("dev")
print(c.status(vm_id=vm_id))
```

Names are case-insensitive, and ambiguous name lookups are rejected. Use IDs
when retaining a resource identity across calls.

A VM status object contains:

```text
id, name, osType, osName, diskDirectory, state, running, agentOnline,
installComplete, building, progress, sshState, sshPort, ramMb,
hddGb, cpuCores, gpuMode, networkMode, displayOpen, gpuId, gpuName
```

Both hosts derive `state` using the same order:

| State | Meaning |
|---|---|
| `building` | Preparing the guest disk or installation |
| `booting` | VM start is in progress, or an installed running guest is waiting for its agent |
| `stopped` | Powered off |
| `stopping` | Running VM has a shutdown request in progress |
| `installing` | Powered on, with installation incomplete |
| `online` | Installation complete and guest agent online |

Use `running` to check whether the VM is powered on and `agentOnline` for guest
agent availability. `building` and `progress` report provisioning progress.
`displayOpen` reports viewer presence. Desktop input ownership is reported
separately by `input_status()`.

## Create and manage VMs

`create()` provisions and automatically starts a VM asynchronously. A `202`
response accepts the request; use status polling or events to follow its
progress. Installation time depends on the guest OS and host resources.

This example creates a Windows guest on a Windows host. Use an installer image
matching the guest architecture; Windows guests on macOS require an ARM image.

```python
code, created = c.create(
    name="dev",
    osType="windows11",
    imagePath=r"C:\ISOs\Windows.iso",
    ramMb=8192,
    cpuCores=4,
    networkMode=1,
    testMode=True,
    adminUser="user",
    adminPass="test123",
    sshEnabled=True,
    sshDeployKey=True,
)
if code != 202:
    raise RuntimeError(created)
vm_id = created["id"]
print(c.wait_online(vm_id=vm_id, timeout=2400))
```

For a native macOS VM, omit `imagePath`. AppSandbox automatically downloads the
macOS restore image from Apple and reuses its cache on subsequent installations.
Select a ready macOS template with `templateId` or `templateName` to clone it instead.

### Creation settings

Paths refer to files and directories on the host running AppSandbox.

| Key | Meaning |
|---|---|
| `name` | Required guest hostname. Letters, digits, and hyphens; not all digits; no leading/trailing hyphen. Windows names are at most 15 characters; Ubuntu/macOS names at most 63. Ubuntu names must be lowercase. Names must be unique across VMs and templates. |
| `osType` | Required. Choose a value from `capabilities.guestOs`, or `templateGuestOs` when creating a template. Use `windows11`, `ubuntu2604lts`, or `macos27` as advertised by the host. |
| `imagePath` | Host path to a Windows or Ubuntu 26.04 LTS installer ISO, required when creating from an image. Omit for `macos27` to automatically download and cache the macOS restore image. Omit when using a template. |
| `templateName` / `templateId` | Select one ready template matching the guest OS. `templateId` uses the `template-…` identity. Specify only one selector. |
| `diskDirectory` | Parent directory for the VM's storage; omitted or empty uses the host's `defaultDiskDirectory`. |
| `ramMb` | At least 4096 MB and an even integer. Omitted or zero uses half the host's RAM, with a 4096 MB floor and a 12288 MB cap. The cap applies only to the automatic default; explicit allocations can be larger. |
| `hddGb` | Ubuntu 26.04 LTS: at least 40 GB, default 40 GB. Windows/macOS: at least 64 GB, default 64 GB. Omitted or zero selects the default. Template clones default to the larger of the guest minimum and the template's disk capacity, and cannot be smaller than their template. |
| `cpuCores` | At least 1. Omitted or zero uses 4. |
| `gpuMode` | `0` None, `1` Default GPU mode. Acceleration depends on the host and guest platform. |
| `gpuId` | Select a GPU from `host()["gpus"]` on Windows hosts. |
| `networkMode` | Defaults to `1` NAT. Supported values are listed in `capabilities.networkModes`. |
| `netAdapter` | Host network adapter name for External mode on Windows hosts. |
| `adminUser` | Required guest account name. Ubuntu: at most 32 characters, starts with `[a-z_]`, then `[a-z0-9_-]`. Windows: at most 20 characters, subject to Windows account-name rules; for a VM, the account name must differ from its VM name. Native macOS: at most 63 UTF-8 bytes, no spaces or reserved names. |
| `adminPass` | Required, nonempty guest account password. Ubuntu: at least 6 characters and at most 255 UTF-8 bytes. Windows: at most 127 UTF-16 code units. Native macOS: at least 4 characters and at most 127 UTF-8 bytes. |
| `testMode` | Windows 11 guests only. Disable Secure Boot and enable Windows test-signing. |
| `sshEnabled` | Provision SSH and forward a loopback port. |
| `sshDeployKey` | Deploy the AppSandbox public key for passwordless SSH; requires `sshEnabled: true`. |
| `isTemplate` | Create a template through the VM creation route. `create_template()` provides a dedicated template creation method. Templates cannot be created from another template. |

Requests use JSON integers, booleans, and strings of the declared types. Unknown
fields, duplicate JSON keys, malformed JSON, invalid Unicode, NUL characters,
and oversized bodies are rejected. The JSON request-body limit is 65,535 bytes.
The SDK sends RAM values as supplied; use an even number of megabytes.

### Python methods

VM-specific methods below also accept `vm_id=...` in place of `name`.

| Method | Result or effect |
|---|---|
| `version()` | `{product, version, apiVersion, hostOs, capabilities}` |
| `host()` | Host capacity, VM resource totals, `defaultDiskDirectory`, and `gpus` |
| `list()` | VM status objects |
| `vm_id(name)` | Resolve a name to its persistent VM ID |
| `status(name)` | One VM status object |
| `create(**cfg)` | `(status, body)`; create and auto-start a VM |
| `edit(name, **fields)` | `(status, body)`; edit a stopped VM |
| `start(name, snap_index=-1, branch_index=-1, branch_name=None, open_display=None)` | `(status, body)`; start from the current disk or a supported snapshot/branch |
| `shutdown(name)` | `(status, body)`; request graceful shutdown |
| `stop(name)` | `(status, body)`; force power-off |
| `delete_vm(name)` | `(status, body)`; delete a VM and its storage |
| `wait(name, target_states, timeout=300, interval=2)` | Poll until `state` matches a string or set of states; raise `TimeoutError` on expiry |
| `wait_online(name, timeout=900)` | Wait for `online` |
| `shutdown_daemon(force=False)` | `(status, body)`; stop AppSandbox; active VMs prevent shutdown unless forced |

`host()` returns `hostCores`, `hostRamMb`, `freeGb`, `vmCores`, `vmRamMb`,
`vmHddGb`, `defaultDiskDirectory`, and `gpus`. GPU entries contain `id`,
`name`, and `location`; macOS returns an empty GPU-selection list.

`edit()` accepts `ramMb`, `cpuCores`, `gpuMode`, `gpuId`, and `networkMode`.
The VM must be stopped and its configuration unlocked. Names are fixed at
creation. Query failures raise exceptions; action failures are returned in
the `(status, body)` result.

## Templates, snapshots, and branches

| Python method | Result or effect |
|---|---|
| `templates()` | Completed templates with `id`, `name`, `osType`, `osName`, `hddGb`, `ready`, `state`, `building`, and `progress` |
| `template(name)` / `template(template_id=...)` | Template status, including a build in progress |
| `create_template(**cfg)` | `(status, body)`; create a template using creation settings |
| `delete_template(name)` / `delete_template(template_id=...)` | `(status, body)`; delete a template or cancel its build where supported |
| `snapshots(name)` | Snapshot list, including branch information |
| `snapshots_full(name)` | Snapshots, `baseBranches`, and `current: {snapIndex, branchIndex}` |
| `snap_take(name, snapname=None)` | `(status, body)`; take a snapshot while stopped |
| `snap_rename(name, index, new_name, branch_index=-1)` | `(status, body)`; rename a snapshot or a selected branch |
| `snap_delete(name, index)` | `(status, body)`; delete a snapshot |
| `snap_delete_branch(name, index, branch_index)` | `(status, body)`; delete a branch |

Templates support Windows guests on both hosts and macOS guests on macOS hosts.
Snapshots and branches require a Windows host. Template deletion can be refused
while a VM or active copy depends on it.

On macOS hosts, deletion is refused while the disk installer or template-copy
phase is active. Wait for that phase to finish before deleting the resource.

Create a branch by starting from a snapshot with a branch name:

```python
c.start("dev", snap_index=2, branch_name="experiment", open_display=False)
```

Use `snap_index=-2` for branches of the base disk. The base disk itself is not
a deletable snapshot; its branches can be renamed or deleted. Snapshot and
branch indices must be integers within the operation's supported range.

## Viewers and screenshots

| Python method | Result or effect |
|---|---|
| `display_status(name)` | `{open, ready}` without opening a viewer |
| `display_ready(name)` | Whether the display is ready to open |
| `open_display(name)` | `(status, body)`; open or focus the local viewer |
| `close_display(name)` | `(status, body)`; close the local viewer |
| `screenshot(name, timeout_ms=5000)` | `(status, body)`; capture a native-resolution PNG |

All methods accept `vm_id=...`. The viewer is a window on the host showing
the VM's graphical desktop. The guest desktop continues running when that
window is closed. Opening the window requires a ready guest display and a
host session that can show windows. If AppSandbox itself runs in a
non-interactive session, `open_display` returns `409 no_display`.

Use `start(name, open_display=False)` in Python, or `openDisplay: false` with
HTTP/MCP, to start without automatically opening the viewer. Closing the
viewer does not stop the VM or finish an agent's input task.

Screenshots work with the viewer open or closed and do not claim desktop
input. A screenshot returns:

```text
metadata: vmId, vm, osType, osName, width, height, stride, frameSeq,
          gpuSharingActive, elapsedMs
image:    mimeType, data
```

`image.mimeType` is `image/png` and `image.data` is base64-encoded PNG data.
MCP presents the PNG as image content and the metadata as structured content.
The API returns images in memory; a client can save one explicitly:

```python
import base64
from pathlib import Path

code, capture = c.screenshot("dev")
if code != 200:
    raise RuntimeError(capture)
Path("desktop.png").write_bytes(base64.b64decode(capture["image"]["data"]))
```

`timeout_ms` accepts `1` through `10000`. Display changes can temporarily
prevent capture; callers may retry a failed screenshot.

## Desktop input and user control

Desktop input uses an exclusive human/agent control session. The agent supplies
its `identity`, model `vendor`, `model`, and task `purpose` when requesting
control. Use `unknown` when vendor or model information is unavailable.
Identity, vendor, and model are limited to 80 characters each; purpose is
limited to 300. Each must be nonempty.

| Python method | Result or effect |
|---|---|
| `input_status(name)` | Current ownership, task state, agent details, cursor, and viewer presence |
| `input_request_control(name, identity=..., vendor=..., model=..., purpose=...)` | `(status, body)`; begin desktop work |
| `input_wait_for_control(name, timeout_ms=30000)` | `(status, body)`; wait for control or task completion without forcing takeover |
| `input(name, actions=[...])` | `(status, body)`; send a batch of actions |
| `input_finish(name)` | `(status, body)`; finish work or acknowledge cancellation, release input, and hide its overlay |

All methods accept `vm_id=...`. Check `agent_has_control` before sending input.
While an agent works, the viewer displays its identity, vendor, model, purpose,
and cursor. The viewer's mouse controls let the user:

- **Take control:** pause agent input and use the guest directly.
- **Return control:** resume the unfinished agent task.
- **Stop agent:** cancel the task.

On `409 user_has_control`, stop sending input and wait with
`input_wait_for_control`; the user must choose Return control. A wait accepts
`1` through `60000` milliseconds and may return without control when the wait
expires, so inspect the returned flags. On `410 agent_cancelled`, acknowledge
with `input_finish` and end the task. Start another task only on a new user
instruction. Call `input_finish` when work finishes or is abandoned.

Input status includes `state` (`agent`, `user`, or `available`), `vmId`, `vm`,
`generation`, `work_id`, `agent_has_control`, `agent_waiting_for_control`,
`user_has_control`, `agent_finished`, `agent_cancelled`, `revoked`,
`viewer_open`, and `viewer_focused`. Active task details include `identity`,
`vendor`, `model`, and `purpose`. `cursor` is null when unknown; otherwise it
contains `xNormalized`, `yNormalized`, and `range: 65535`.

### Input actions

Each request contains `actions`, a nonempty array of at most 128 actions.
Send coordinates in the native screenshot's pixel space, supplying its
`width` and `height` with each move or click. Buttons are `0` left, `1` right,
and `2` middle. Keys use PC set-1 scan codes; `down` defaults to true and
`extended` defaults to false.

| Action | Fields |
|---|---|
| `move` | `x`, `y`, `width`, `height` |
| `click` | `x`, `y`, `width`, `height`, optional `button`; sends a press and release |
| `button` | `button`, `down`; sends a button press or release at the current position |
| `scroll` | `delta`, from `-12000` through `12000` |
| `key` | `scan` from `1` through `255`, `down`, `extended` |
| `text` | `text`; types physical keys using the host keyboard layout |
| `unicode` | `text`; direct Unicode text input for supported Windows guests |

`unicode` requires Unicode support in the Windows guest input helper. Native
macOS and Ubuntu guests reject this action with `501 not_supported`; use `key`
or layout-representable `text` instead.

Coordinates must fall within the supplied dimensions, which support widths
from `2` through `7680` and heights from `2` through `4320`. A batch supports
at most 4096 text characters across its `text` and `unicode` actions.

For example, after acquiring control, this batch presses and releases Enter:

```python
code, result = c.input("dev", actions=[
    {"type": "key", "scan": 28, "down": True},
    {"type": "key", "scan": 28, "down": False},
])
```

Successful input returns `handledActions`, `acknowledgement`, and `control`.
The acknowledgement is `injector_processed` for the guest input-helper path
or `virtual_device_dispatched` for native macOS. It reports input dispatch,
not completion of the guest application's response; inspect a screenshot or
the guest's output to verify the result.

## SSH and key deployment

Create VMs with `sshEnabled=True` and `sshDeployKey=True` to provision SSH and
deploy the AppSandbox public key. Key deployment requires SSH to be enabled.
The keypair is stored under `ssh/` next to the discovery file; `asb.key_path()`
returns the private key path.

`ssh_info(name)` returns `host`, `port`, `user`, `sshState`, `enabled`, and
`keyDeployed`. SSH state values are `0` inactive, `1` installing, `2` ready,
`3` failed, and `4` ready with the key deployed. The loopback-forwarded
connection does not require guest networking.

A regular SSH client can use these details:

```python
import subprocess

info = c.ssh_info("dev")
subprocess.run([
    "ssh", "-i", asb.key_path(), "-p", str(info["port"]),
    "%s@%s" % (info["user"], info["host"]), "hostname",
], check=True)
```

### Streaming SSH sessions

MCP, HTTP, and Python can open host-owned SSH sessions with independent stdin,
stdout, and stderr. Sessions do not claim desktop input or activate its overlay.
The host needs its system OpenSSH client and the AppSandbox key deployed in
the guest. Close each session when finished.

| Python method | Result |
|---|---|
| `ssh_open(name, command="")` | `(status, body)` with `vmId`, `sshSessionId`, `closed: false` |
| `ssh_read(name, ssh_session_id, timeout_ms=1000)` | `(status, body)` with IDs, `running`, `exitCode`, `stdinError`, `stdoutBase64`, `stderrBase64` |
| `ssh_write(name, ssh_session_id, data="", close_stdin=False)` | `(status, body)` with IDs, `queuedBytes`, and `stdinClosing` |
| `ssh_close(name, ssh_session_id)` | `(status, body)` with IDs and `closed: true` |

All methods accept `vm_id=...`. `ssh_read` accepts waits from `0` through
`10000` milliseconds. Empty output with `running: true` means the command is
still working. Once it exits, drain both output streams before closing.

`ssh_write` accepts Python `str` as UTF-8 text or `bytes` as binary stdin.
HTTP/MCP use either `stdin` or `stdinBase64`, never both. `closeStdin: true`
closes stdin after queued data is written. An empty write alone sends no
data; use `close_stdin=True` to signal EOF without additional input:

```python
c.ssh_write("dev", ssh_session_id, data=b"", close_stdin=True)
```

Writes are queued asynchronously; inspect `stdinError` in read results for
stdin delivery failures. Keep requests within the JSON body limit and split
large input into chunks. The host supports up to 16 SSH sessions, with bounded
input queues and up to 65,536 output bytes per stream in each read.

This example runs `hostname` on a running VM and reads its output:

```python
import base64
import sys

code, opened = c.ssh_open("dev", command="hostname")
if code != 200:
    raise RuntimeError(opened)
session_id = opened["sshSessionId"]
try:
    while True:
        code, output = c.ssh_read("dev", session_id)
        if code != 200:
            raise RuntimeError(output)
        stdout = base64.b64decode(output["stdoutBase64"])
        stderr = base64.b64decode(output["stderrBase64"])
        sys.stdout.buffer.write(stdout)
        sys.stderr.buffer.write(stderr)
        if not output["running"] and not stdout and not stderr:
            print("Exit code:", output["exitCode"])
            break
finally:
    c.ssh_close("dev", session_id)
```

## Status events

HTTP exposes Server-Sent Events at `/v1/events`. Python's `events()` yields
parsed dictionaries and blocks while waiting; iterate it in a background
thread when the rest of the client must continue working.

Event names include `vmListChanged`, `vmStateChanged`, `vmProgress`,
`templateStateChanged`, `templateProgress`, and `alert`. State and progress
events carry the VM or template ID. `vmListChanged` signals that lists should
be refreshed; `alert` carries a message. For example:

```json
{"event": "vmStateChanged", "id": "vm-0123456789abcdef", "name": "dev", "running": true}
```

MCP uses `events_subscribe`, `events_read`, and `events_unsubscribe` within
the client session. `events_read` accepts `timeoutMs` from `0` through `30000`
and `maxEvents` from `1` through `64`. Its response includes `events`,
`subscribed`, `connected`, `streamEnded`, `streamError`, `droppedEvents`, and
`pendingEvents`. Only one event read can be pending per session.

Events have no replay guarantee. Refresh current VM or template status after
reconnecting, dropped events, or stream end. Use `status()`/`wait()` for
lifecycle control and `input_status()` for desktop input ownership.

## HTTP routes

Paths below are relative to `/v1`. VM subroutes also accept `/vms/{name}` in
place of `/vm-ids/{vm_id}`. Template resources accept `/templates/{name}` in
place of `/template-ids/{template_id}`.

| Method | Path | Operation |
|---|---|---|
| GET | `/version`, `/host`, `/operations` | Version/capabilities, host capacity, operation catalogue |
| GET | `/vms` | List VMs |
| POST | `/vms` | Create VM |
| GET, PUT | `/vm-ids/{vm_id}` | VM status or edit |
| POST | `/vm-ids/{vm_id}/start` | Start; optional snapshot/branch and `openDisplay` settings |
| POST | `/vm-ids/{vm_id}/shutdown` | Graceful shutdown |
| POST | `/vm-ids/{vm_id}/stop` | Force-stop |
| POST | `/vm-ids/{vm_id}/delete` | Delete VM |
| GET, POST | `/templates` | List completed templates or create one |
| GET, DELETE | `/template-ids/{template_id}` | Read/delete a template, or cancel a build where supported |
| GET, POST, DELETE | `/vm-ids/{vm_id}/display` | Read viewer status, open/focus, or close |
| POST | `/vm-ids/{vm_id}/screenshot` | PNG screenshot; optional `timeoutMs` |
| GET, POST | `/vm-ids/{vm_id}/input` | Input status or an `actions` batch |
| POST, DELETE | `/vm-ids/{vm_id}/input/control` | Request control or finish work |
| POST | `/vm-ids/{vm_id}/input/wait` | Wait for control; optional `timeoutMs` |
| GET | `/vm-ids/{vm_id}/sshInfo` | SSH connection details |
| POST | `/vm-ids/{vm_id}/ssh/sessions` | Open SSH session; optional `command` |
| POST | `/vm-ids/{vm_id}/ssh/sessions/{ssh_session_id}/read` | Read stdout/stderr and exit state; optional `timeoutMs` |
| GET, POST, DELETE | `/vm-ids/{vm_id}/ssh/sessions/{ssh_session_id}` | Read, write stdin, or close session |
| GET, POST | `/vm-ids/{vm_id}/snapshots` | List or take snapshots |
| PUT, DELETE | `/vm-ids/{vm_id}/snapshots/{snapshot_index}` | Rename or delete snapshot |
| DELETE | `/vm-ids/{vm_id}/snapshots/{snapshot_index}/branches/{branch_index}` | Delete a branch |
| GET | `/events` | SSE event stream |
| POST | `/shutdown` | Stop AppSandbox; optional `force` |

## HTTP status and errors

Successful VM and template lifecycle actions return `202` with `ok`,
`accepted`, `action`, `id`, and `name`. Status queries, edits, screenshots,
input, and SSH use `200` on success. Wait for lifecycle status to confirm
an asynchronous operation's outcome.

Errors use this shape:

```json
{"error": {"code": "invalid_arg", "message": "Invalid operation arguments or guest response."}}
```

| Status | Meaning |
|---|---|
| `200` | Successful query or synchronous operation |
| `202` | Lifecycle operation accepted asynchronously |
| `400` | Invalid arguments, JSON, field types, indices, or request size |
| `401` | Missing or invalid bearer token |
| `403` | Requested guest feature has not granted access (`permission_denied`) |
| `404` | Resource, route, or session not found |
| `405` | Unsupported HTTP method for the route |
| `409` | Resource conflict or not ready; examples include `vm_running`, `vm_starting`, `vm_building`, `vms_active`, `no_display`, `display_not_ready`, `not_ready`, and `user_has_control` |
| `410` | Agent work cancelled by the user (`agent_cancelled`) |
| `500` | Operation failed (`operation_failed`, `automation_failed`, or an operation-specific code) |
| `501` | Unsupported host/guest feature, key, or layout text (`not_supported`) |
| `503` | Service busy or unavailable |
| `504` | Operation timed out (`timeout`) |

## Examples and product tests

Runnable Python scripts are in [`examples/`](examples/):

| Script | Purpose |
|---|---|
| `list_vms.py` | Read host information, VMs, and templates |
| `start_vm.py` | Start a VM and wait for it to be online |
| `stop_vm.py` | Graceful or forced VM shutdown |
| `full_lifecycle.py` | Create, wait, edit, snapshot, branch, and delete |
| `stream_events.py` | Read SSE events |
| `admin.py` | Host and template administration |

The product test suite and its setup instructions are in [`tests/`](tests/).
It creates disposable VMs and needs the corresponding installation images.
