import json
import os
import sys
import time
import urllib.request
import urllib.error
import urllib.parse
import re
import base64


def _support_dir():
    """Per-platform AppSandbox data dir (discovery file, ssh keypair, logs)."""
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Application Support/AppSandbox")
    base = os.environ.get("ProgramData", r"C:\ProgramData")
    return os.path.join(base, "AppSandbox")


def _discovery_path():
    return os.path.join(_support_dir(), "host.json")


def daemon_launch_hint():
    if sys.platform == "darwin":
        return "AppSandbox.app/Contents/MacOS/AppSandbox --headless"
    return "appsandbox.exe --headless (from an elevated shell)"


def key_path():
    """Path to the AppSandbox SSH private key. VMs created with sshDeployKey=True
    get the matching public key in their authorized_keys, so you can connect with:
        ssh -i <key_path()> -p <sshInfo['port']> <sshInfo['user']>@127.0.0.1
    The daemon generates the keypair (under <support dir>/ssh/) the first time a
    key-deploy VM is created."""
    return os.path.join(_support_dir(), "ssh", "id_appsandbox")


# VM lifecycle states the daemon's derived `state` can report.
RUNNING_STATES = {"booting", "online"}   # powered on (agent maybe not up yet)


class VmId(str):
    pass


class TemplateId(str):
    pass


class ApiError(RuntimeError):
    def __init__(self,status,body):
        self.status=status
        self.body=body
        detail=body.get('error',body) if isinstance(body,dict) else body
        super().__init__('AppSandbox API returned HTTP %s: %s' % (status,detail))


class ApiNotFound(ApiError,KeyError):
    pass


def _is_vm_id(value):
    return (isinstance(value,str) and len(value)==19 and
            value.startswith('vm-') and
            all(c in '0123456789abcdef' for c in value[3:]) and
            value!='vm-0000000000000000')


def _vm_identity_fields(value):
    identity = value.get("id")
    if _is_vm_id(identity):
        value["id"] = VmId(identity)
    elif _is_template_id(identity):
        value['id']=TemplateId(identity)
    return value


def _is_template_id(value):
    return (isinstance(value,str) and len(value)==25 and value.startswith('template-') and
            all(c in '0123456789abcdef' for c in value[9:]) and value!='template-0000000000000000')


class Client:
    def __init__(self, endpoint, token):
        self.base = endpoint.rstrip("/") + "/v1"
        self.token = token

    def _req(self, method, path, body=None, timeout=60):
        url = self.base + path
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")   # parser tolerates whitespace
        elif method in ("POST", "PUT"):
            data = b"{}"   # http.sys returns 411 on a body-less POST/PUT
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + self.token)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read().decode("utf-8")
                return r.status, (json.loads(raw, object_hook=_vm_identity_fields) if raw else {})
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8")
            return e.code, (json.loads(raw, object_hook=_vm_identity_fields) if raw else {})

    def vm_id(self, target):
        body=self._query('GET','/vms/'+urllib.parse.quote(target,safe=''))
        if not body.get('id'): raise RuntimeError('VM response has no ID')
        return body["id"]

    def _vm_path(self, target=None, *, vm_id=None):
        if vm_id is not None:
            if target is not None: raise ValueError('Specify either name or vm_id, not both')
            if not _is_vm_id(vm_id): raise ValueError('Invalid vm_id')
            identity=vm_id
        else:
            if target is None: raise ValueError('Specify name or vm_id')
            if not isinstance(target,str) or not target: raise ValueError('Invalid VM name')
            return '/vms/'+urllib.parse.quote(target,safe='')
        return '/vm-ids/'+urllib.parse.quote(identity,safe='')

    def _query(self,method,path):
        status,body=self._req(method,path)
        if status>=400: raise (ApiNotFound if status==404 else ApiError)(status,body)
        return body

    @staticmethod
    def _index(value,minimum=0):
        if type(value) is not int or value<minimum:
            raise ValueError('Index must be an integer >= %s' % minimum)
        return str(value)

    @staticmethod
    def _request_timeout(milliseconds):
        return max(60,min(milliseconds,60000)/1000+5) if type(milliseconds) is int and milliseconds>=0 else 60

    # ---- queries ----
    def version(self):
        return self._query("GET", "/version")

    def list(self):
        return self._query("GET", "/vms").get("vms", [])

    def status(self, name=None, *, vm_id=None):
        return self._query("GET",self._vm_path(name,vm_id=vm_id))

    # ---- lifecycle (return (http_status, body)) ----
    def start(self, name=None, snap_index=-1, branch_index=-1, branch_name=None, open_display=None, *, vm_id=None):
        """Start the VM. Optionally boot from a snapshot/branch (mirrors the GUI's
        startVm). Passing snap_index + branch_name boots from that snapshot on a new
        branch -- the GUI's only way to create a branch. No args -> current state."""
        body = {}
        if type(snap_index) is not int or snap_index != -1: body["snapIndex"] = snap_index
        if type(branch_index) is not int or branch_index != -1: body["branchIndex"] = branch_index
        if branch_name is not None: body["branchName"] = branch_name
        if open_display is not None: body['openDisplay'] = open_display
        return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/start"), body or None)
    def shutdown(self, name=None, *, vm_id=None): return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/shutdown"))
    def stop(self, name=None, *, vm_id=None):     return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/stop"))
    def delete_vm(self, name=None, *, vm_id=None):return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/delete"))

    def create(self, **cfg):
        return self._req("POST", "/vms", cfg)

    def edit(self, name=None, *, vm_id=None, **fields):
        """Change config on a STOPPED VM. Honors ramMb/cpuCores/gpuMode/networkMode
        (same ranges as create); name is fixed at create and any other key is ignored.
        Returns (status, body) -- 409 if the VM is running."""
        return self._req("PUT", self._vm_path(name,vm_id=vm_id), fields)

    def create_template(self,**cfg):
        return self._req('POST','/templates',cfg)

    def operations(self):
        return self._query('GET','/operations')['operations']

    def operation(self,operation_name,**arguments):
        operation=next((item for item in self.operations() if item['name']==operation_name),None)
        if operation is None: raise ValueError('Unknown API operation: '+operation_name)
        body=dict(arguments)
        def substitute(match):
            key=match.group(1)
            if key not in body: raise ValueError('Missing '+key)
            value=body.pop(key)
            if key=='vm_id' and not _is_vm_id(value): raise ValueError('Invalid vm_id')
            if key=='template_id' and not _is_template_id(value): raise ValueError('Invalid template_id')
            if key=='ssh_session_id': self._ssh_id(value)
            if key.endswith('_index') and type(value) is not int: raise ValueError('Index must be an integer')
            return urllib.parse.quote(str(value),safe='')
        path=re.sub(r'\{([a-z_]+)\}',substitute,operation['path'])
        if operation['method']=='GET' and body: raise ValueError('Unexpected query arguments')
        return self._req(operation['method'],path,body or None,timeout=self._request_timeout(arguments.get('timeoutMs',0)))

    def screenshot(self,name=None,timeout_ms=5000,*,vm_id=None):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/screenshot',{'timeoutMs':timeout_ms})

    def input_status(self,name=None,*,vm_id=None):
        return self._query('GET',self._vm_path(name,vm_id=vm_id)+'/input')

    def input_request_control(self,name=None,*,vm_id=None,identity,vendor,model,purpose):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/input/control',
            dict(identity=identity,vendor=vendor,model=model,purpose=purpose))

    def input(self,name=None,actions=None,*,vm_id=None):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/input',{'actions':actions})

    def input_finish(self,name=None,*,vm_id=None):
        return self._req('DELETE',self._vm_path(name,vm_id=vm_id)+'/input/control')

    def input_wait_for_control(self,name=None,timeout_ms=30000,*,vm_id=None):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/input/wait',{'timeoutMs':timeout_ms},timeout=self._request_timeout(timeout_ms))

    @staticmethod
    def _ssh_id(value):
        if not isinstance(value,str) or not re.fullmatch(r'ssh-(?!0000000000000000$)[0-9a-f]{16}',value):
            raise ValueError('Invalid SSH session ID')
        return value

    def ssh_open(self,name=None,command='',*,vm_id=None):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/ssh/sessions',{'command':command})

    def ssh_read(self,name=None,ssh_session_id=None,timeout_ms=1000,*,vm_id=None):
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/ssh/sessions/'+self._ssh_id(ssh_session_id)+'/read',{'timeoutMs':timeout_ms})

    def ssh_write(self,name=None,ssh_session_id=None,data='',close_stdin=False,*,vm_id=None):
        body={'closeStdin':close_stdin}
        if isinstance(data,bytes): body['stdinBase64']=base64.b64encode(data).decode('ascii')
        else: body['stdin']=data
        return self._req('POST',self._vm_path(name,vm_id=vm_id)+'/ssh/sessions/'+self._ssh_id(ssh_session_id),body)

    def ssh_close(self,name=None,ssh_session_id=None,*,vm_id=None):
        return self._req('DELETE',self._vm_path(name,vm_id=vm_id)+'/ssh/sessions/'+self._ssh_id(ssh_session_id))

    # ---- host / templates / ssh / snapshots ----
    def host(self):              return self._query("GET", "/host")
    def templates(self):         return self._query("GET", "/templates").get("templates", [])
    def _template_path(self,name=None,*,template_id=None):
        if template_id is not None:
            if name is not None: raise ValueError('Specify either name or template_id, not both')
            if not _is_template_id(template_id): raise ValueError('Invalid template_id')
            return '/template-ids/'+urllib.parse.quote(template_id,safe='')
        if not isinstance(name,str) or not name: raise ValueError('Specify a template name or template_id')
        return '/templates/'+urllib.parse.quote(name,safe='')

    def template(self,name=None,*,template_id=None):
        return self._query('GET',self._template_path(name,template_id=template_id))

    def delete_template(self, name=None, *, template_id=None):
        return self._req('DELETE',self._template_path(name,template_id=template_id))
    def ssh_info(self, name=None, *, vm_id=None): return self._query("GET", (self._vm_path(name,vm_id=vm_id) + "/sshInfo"))
    def open_display(self, name=None, *, vm_id=None):
        """Open the VM's display window on the daemon's local desktop (the GUI's
        Connect view). The VM must be running, and the daemon must be in an
        interactive session that can show a window (a headless/SSH/service daemon
        returns 409 "no_display"). Returns (status, body); status() reports
        displayOpen, which also goes false if the user closes the window."""
        return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/display"))
    def close_display(self, name=None, *, vm_id=None): return self._req("DELETE", (self._vm_path(name,vm_id=vm_id) + "/display"))
    def display_status(self, name=None, *, vm_id=None):
        """{'open': bool, 'ready': bool}. 'ready' is the agent's own report that the
        display driver is up (running + agentOnline + idd_status) -- a passive flag,
        NOT a probe: polling it touches no window and no frame channel, so it can't
        disturb the display. Wait for ready, then call open_display()."""
        return self._query("GET", (self._vm_path(name,vm_id=vm_id) + "/display"))
    def display_ready(self, name=None, *, vm_id=None): return bool(self.display_status(name,vm_id=vm_id).get("ready"))
    def snapshots(self, name=None, *, vm_id=None): return self._query("GET", (self._vm_path(name,vm_id=vm_id) + "/snapshots")).get("snapshots", [])
    def snapshots_full(self, name=None, *, vm_id=None): return self._query("GET", (self._vm_path(name,vm_id=vm_id) + "/snapshots"))
    def snap_take(self, name=None, snapname=None, *, vm_id=None):
        """Take a snapshot -- the VM must be STOPPED (409 vm_running otherwise)."""
        return self._req("POST", (self._vm_path(name,vm_id=vm_id) + "/snapshots"), {"name": snapname} if snapname is not None else {})
    def snap_delete(self, name=None, index=None, *, vm_id=None):
        return self._req("DELETE", self._vm_path(name,vm_id=vm_id) + "/snapshots/"+self._index(index))
    def snap_rename(self, name=None, index=None, new_name=None, branch_index=-1, *, vm_id=None):
        self._index(branch_index,-1)
        return self._req("PUT", self._vm_path(name,vm_id=vm_id) + "/snapshots/"+self._index(index,-2),
                         {"name": new_name, "branchIndex": branch_index})
    def snap_delete_branch(self, name=None, index=None, branch_index=None, *, vm_id=None):
        return self._req("DELETE", self._vm_path(name,vm_id=vm_id) + "/snapshots/"+self._index(index,-2)+"/branches/"+self._index(branch_index))

    def shutdown_daemon(self, force=False):
        # Refused (409) while VMs run unless force=True (which terminates them).
        return self._req("POST", "/shutdown", {"force": force})

    # ---- events (Server-Sent Events) ----
    def events(self):
        """Stream daemon events as parsed dicts, e.g. {"event":"vmStateChanged",
        "name":..., "running":...}. Blocks -- iterate it in a background thread;
        reconnect by calling again. Events are a convenience: status/wait() are the
        miss-proof source of truth."""
        req = urllib.request.Request(self.base + "/events",
                                     headers={"Authorization": "Bearer " + self.token})
        with urllib.request.urlopen(req) as r:
            for line in r:
                s = line.decode("utf-8", "replace").strip()
                if s.startswith("data:"):
                    payload = s[5:].strip()
                    try:
                        yield json.loads(payload, object_hook=_vm_identity_fields)
                    except ValueError:
                        yield {"raw": payload}

    # ---- waiting (poll-first: level signals, miss-proof) ----
    def wait(self, name=None, target_states=None, timeout=300, interval=2, *, vm_id=None):
        path=self._vm_path(name,vm_id=vm_id)
        target=vm_id if vm_id is not None else name
        if target_states is None: raise ValueError('Specify target_states')
        if isinstance(target_states, str):
            target_states = {target_states}
        deadline = time.monotonic() + timeout
        while True:
            try:
                s = self._query('GET',path)
            except KeyError:
                raise RuntimeError("%s vanished (404)" % target)
            if s["state"] in target_states:
                return s
            if time.monotonic() > deadline:
                raise TimeoutError(
                    "%s did not reach %s within %ss (last state=%s)"
                    % (target, sorted(target_states), timeout, s["state"]))
            time.sleep(interval)

    def wait_online(self, name=None, timeout=900, *, vm_id=None):
        return self.wait(name, {"online"}, timeout=timeout,vm_id=vm_id)


def connect(endpoint=None, token=None, check=True):
    """Read the discovery file and return a Client.

    By default (check=True) it pings GET /v1/version to confirm a live AppSandbox
    daemon is actually there: the discovery file is removed only on a CLEAN
    shutdown, so after a crash it is stale and may point at a dead pid (or a port
    another process has since taken). The ping raises a clear error instead of
    letting the first real call fail confusingly. Pass check=False to skip it."""
    if endpoint is None or token is None:
        try:
            with open(_discovery_path(), encoding="utf-8") as f:
                info = json.load(f)
        except FileNotFoundError:
            raise RuntimeError(
                "no daemon discovery file at %s -- is the daemon running? "
                "start it with: %s" % (_discovery_path(), daemon_launch_hint()))
        endpoint = endpoint or info["endpoint"]
        token = token or info["token"]
    c = Client(endpoint, token)
    if check:
        try:
            v = c._req("GET", "/version", timeout=3)[1]
        except Exception as e:
            raise RuntimeError(
                "AppSandbox daemon at %s is not responding (stale host.json from an "
                "unclean shutdown?): %s" % (endpoint, e))
        if v.get("product") != "AppSandbox":
            raise RuntimeError(
                "%s is not an AppSandbox daemon -- stale host.json (the port was "
                "taken by another process?)" % endpoint)
    return c
