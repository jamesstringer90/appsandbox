import ctypes
import fcntl
import json
import os
from pathlib import Path
import pwd
import select
import shlex
import socket
import stat
import subprocess


def peer_uid(connection):
    library = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
    uid, gid = ctypes.c_uint(), ctypes.c_uint()
    if library.getpeereid(connection.fileno(), ctypes.byref(uid), ctypes.byref(gid)):
        raise OSError(ctypes.get_errno(), 'getpeereid failed')
    return uid.value


def check_directory(path):
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise RuntimeError('The launcher directory must be owned by root and not writable by other users.')


class ProcessWatch:
    def __init__(self, pid):
        self.queue = select.kqueue()
        try:
            self.queue.control([select.kevent(pid, filter=select.KQ_FILTER_PROC,
                                             flags=select.KQ_EV_ADD | select.KQ_EV_ONESHOT,
                                             fflags=select.KQ_NOTE_EXIT)], 0, 0)
        except OSError:
            self.queue.close()
            raise

    def exited(self):
        return bool(self.queue.control([], 1, 0))

    def close(self):
        self.queue.close()


class Listener:
    def __init__(self, host, identity):
        self.identity = int(identity)
        self.directory = host.session_dir(identity)
        try:
            self.directory.mkdir(mode=0o755)
        except FileExistsError:
            pass
        check_directory(self.directory)
        self.lock = os.open(self.directory / 'lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(self.lock)
            raise RuntimeError('A development launcher session is already running.')
        self.endpoint = self.directory / 'control.sock'
        self.endpoint.unlink(missing_ok=True)
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.bind(str(self.endpoint))
        self.endpoint.chmod(0o600)
        os.chown(self.endpoint, self.identity, pwd.getpwuid(self.identity).pw_gid)
        self.socket.listen(8)
        self.socket.settimeout(1)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.socket.close()
        self.endpoint.unlink(missing_ok=True)
        os.close(self.lock)

    def accept(self):
        try:
            connection, _ = self.socket.accept()
        except socket.timeout:
            return None
        connection.settimeout(5)
        if peer_uid(connection) not in (0, self.identity):
            connection.close()
            return None
        return connection


class Host:
    def identity(self):
        if os.getuid():
            return str(os.getuid())
        user = os.environ.get('SUDO_USER')
        if not user:
            raise RuntimeError('Run this tool as your desktop user, or through sudo with SUDO_USER preserved.')
        return str(pwd.getpwnam(user).pw_uid)

    def session_dir(self, identity):
        return Path('/private/tmp/appsandbox-dev-launcher-%s' % identity)

    def support_dir(self, identity):
        return Path(pwd.getpwuid(int(identity)).pw_dir) / 'Library/Application Support/AppSandbox'

    def require_elevation(self, identity):
        if os.geteuid() != 0 or not self.session_active(identity):
            raise RuntimeError('The supervisor needs administrator authorization in the logged-in desktop user session.')

    def session_active(self, identity):
        return int(identity) != 0 and os.stat('/dev/console').st_uid == int(identity)

    def resolve_binary(self, binary):
        binary = binary.expanduser().resolve()
        if binary.suffix == '.app':
            binary /= 'Contents/MacOS/AppSandbox'
        return binary.resolve()

    def validate_binary(self, binary):
        if binary.resolve() != binary or not binary.is_file() or not os.access(binary, os.X_OK):
            raise RuntimeError('The configured executable is missing, not executable, or its path was replaced by a symlink.')
        target = next((parent for parent in binary.parents if parent.suffix == '.app'), binary)
        result = subprocess.run(['/usr/bin/codesign', '--verify', '--deep', '--strict', str(target)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or 'Code signature verification failed.')

    def process_path(self, pid):
        buffer = ctypes.create_string_buffer(4096)
        library = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
        if library.proc_pidpath(pid, buffer, len(buffer)) <= 0:
            return None
        return Path(os.fsdecode(buffer.value)).resolve()

    def watch(self, pid):
        return ProcessWatch(pid)

    def child_options(self, identity):
        account = pwd.getpwuid(int(identity))
        return {'env': {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LANG': 'en_US.UTF-8',
                        'SUDO_USER': account.pw_name, 'SUDO_UID': identity, 'SUDO_GID': str(account.pw_gid)}}

    def connect(self, identity):
        directory = self.session_dir(identity)
        check_directory(directory)
        endpoint = directory / 'control.sock'
        info = endpoint.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != int(identity) or info.st_mode & 0o077:
            raise RuntimeError('The launcher socket has unexpected ownership or permissions.')
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            connection.settimeout(180)
            connection.connect(str(endpoint))
            if peer_uid(connection) != 0:
                raise RuntimeError('The launcher is not running as root.')
            return connection
        except BaseException:
            connection.close()
            raise

    def listen(self, identity):
        return Listener(self, identity)

    def elevate(self, command):
        if os.geteuid() == 0:
            result = subprocess.run(command, capture_output=True, text=True)
        else:
            script = 'do shell script %s with administrator privileges' % json.dumps('exec ' + shlex.join(command), ensure_ascii=False)
            result = subprocess.run(['/usr/bin/osascript', '-e', script], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or 'The launcher could not be authorized.')

    def detach(self, identity):
        if os.fork():
            os._exit(0)
        os.setsid()
        os.umask(0o077)
        account = pwd.getpwuid(int(identity))
        for name in ('launcher.log', 'core.log'):
            descriptor = os.open(self.session_dir(identity) / name, os.O_CREAT | os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW, 0o600)
            os.fchown(descriptor, account.pw_uid, account.pw_gid)
            if name == 'launcher.log':
                os.dup2(descriptor, 1)
                os.dup2(descriptor, 2)
            os.close(descriptor)
        with open(os.devnull, 'rb') as stream:
            os.dup2(stream.fileno(), 0)
