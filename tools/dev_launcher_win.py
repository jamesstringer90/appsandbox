import ctypes
from ctypes import wintypes as w
import os
from pathlib import Path
import subprocess
import sys
import time


kernel = ctypes.WinDLL('kernel32', use_last_error=True)
security = ctypes.WinDLL('advapi32', use_last_error=True)
shell = ctypes.WinDLL('shell32', use_last_error=True)


def api(library, name, result, *arguments):
    function = getattr(library, name)
    function.restype, function.argtypes = result, arguments
    return function


close_handle = api(kernel, 'CloseHandle', w.BOOL, w.HANDLE)
open_process = api(kernel, 'OpenProcess', w.HANDLE, w.DWORD, w.BOOL, w.DWORD)
open_token = api(security, 'OpenProcessToken', w.BOOL, w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE))
token_info = api(security, 'GetTokenInformation', w.BOOL, w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD))
sid_string = api(security, 'ConvertSidToStringSidW', w.BOOL, w.LPVOID, ctypes.POINTER(w.LPWSTR))
local_free = api(kernel, 'LocalFree', w.HANDLE, w.HANDLE)
current_process = api(kernel, 'GetCurrentProcess', w.HANDLE)
process_image = api(kernel, 'QueryFullProcessImageNameW', w.BOOL, w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD))
wait_process = api(kernel, 'WaitForSingleObject', w.DWORD, w.HANDLE, w.DWORD)
convert_descriptor = api(security, 'ConvertStringSecurityDescriptorToSecurityDescriptorW', w.BOOL, w.LPCWSTR, w.DWORD, ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.DWORD))
get_file_security = api(security, 'GetFileSecurityW', w.BOOL, w.LPCWSTR, w.DWORD, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD))
descriptor_owner = api(security, 'GetSecurityDescriptorOwner', w.BOOL, w.LPVOID, ctypes.POINTER(w.LPVOID), ctypes.POINTER(w.BOOL))
set_file_security = api(security, 'SetFileSecurityW', w.BOOL, w.LPCWSTR, w.DWORD, w.LPVOID)
file_attributes = api(kernel, 'GetFileAttributesW', w.DWORD, w.LPCWSTR)


def checked(result):
    if not result:
        raise ctypes.WinError(ctypes.get_last_error())
    return result


def sid_text(sid):
    text = w.LPWSTR()
    checked(sid_string(sid, ctypes.byref(text)))
    try:
        return text.value
    finally:
        local_free(ctypes.cast(text, w.HANDLE))


def process_identity(pid=None, elevated=False):
    process = checked(open_process(0x1000, False, pid)) if pid is not None else current_process()
    token = w.HANDLE()
    try:
        checked(open_token(process, 8, ctypes.byref(token)))
        size = w.DWORD()
        token_info(token, 1, None, 0, ctypes.byref(size))
        buffer = ctypes.create_string_buffer(size.value)
        checked(token_info(token, 1, buffer, len(buffer), ctypes.byref(size)))
        identity = sid_text(ctypes.cast(buffer, ctypes.POINTER(w.LPVOID))[0])
        if elevated:
            value = w.DWORD()
            checked(token_info(token, 20, ctypes.byref(value), ctypes.sizeof(value), ctypes.byref(size)))
            if not value.value:
                raise PermissionError('The launcher process is not elevated.')
        return identity
    finally:
        if token:
            close_handle(token)
        if pid is not None:
            close_handle(process)


class SecurityAttributes(ctypes.Structure):
    _fields_ = [('length', w.DWORD), ('descriptor', w.LPVOID), ('inherit', w.BOOL)]


class Descriptor:
    def __init__(self, text):
        self.pointer = w.LPVOID()
        checked(convert_descriptor(text, 1, ctypes.byref(self.pointer), None))
        self.attributes = SecurityAttributes(ctypes.sizeof(SecurityAttributes), self.pointer, False)

    def close(self):
        local_free(self.pointer)


class ProcessWatch:
    def __init__(self, pid):
        self.handle = checked(open_process(0x101000, False, pid))

    def exited(self):
        result = wait_process(self.handle, 0)
        if result == 0xFFFFFFFF:
            raise ctypes.WinError(ctypes.get_last_error())
        return result == 0

    def close(self):
        close_handle(self.handle)


read_file = api(kernel, 'ReadFile', w.BOOL, w.HANDLE, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID)
write_file = api(kernel, 'WriteFile', w.BOOL, w.HANDLE, w.LPCVOID, w.DWORD, ctypes.POINTER(w.DWORD), w.LPVOID)
peek_pipe = api(kernel, 'PeekNamedPipe', w.BOOL, w.HANDLE, w.LPVOID, w.DWORD, w.LPVOID, ctypes.POINTER(w.DWORD), w.LPVOID)
disconnect_pipe = api(kernel, 'DisconnectNamedPipe', w.BOOL, w.HANDLE)


class Connection:
    def __init__(self, handle, server=False):
        self.handle, self.server = handle, server
        self.read_deadline = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        if self.server:
            self.read_deadline = time.monotonic() + 5
            try:
                self.recv(1)
            except OSError:
                pass
            finally:
                disconnect_pipe(self.handle)
        else:
            close_handle(self.handle)

    def sendall(self, data):
        if len(data) > 16384:
            raise ValueError('The launcher message is too large.')
        count = w.DWORD()
        checked(write_file(self.handle, data, len(data), ctypes.byref(count), None))
        if count.value != len(data):
            raise OSError('The launcher pipe did not write the entire message.')

    def recv(self, maximum):
        if self.read_deadline is None:
            self.read_deadline = time.monotonic() + (5 if self.server else 180)
        available = w.DWORD()
        while True:
            if time.monotonic() >= self.read_deadline:
                raise TimeoutError('Timed out waiting for the launcher pipe.')
            checked(peek_pipe(self.handle, None, 0, None, ctypes.byref(available), None))
            if available.value:
                break
            time.sleep(0.01)
        buffer = ctypes.create_string_buffer(min(maximum, available.value))
        count = w.DWORD()
        checked(read_file(self.handle, buffer, len(buffer), ctypes.byref(count), None))
        return buffer.raw[:count.value]


class Listener:
    def __init__(self, identity):
        self.identity = identity
        descriptor = Descriptor('D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;%s)' % identity)
        try:
            create = api(kernel, 'CreateNamedPipeW', w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, w.DWORD, ctypes.POINTER(SecurityAttributes))
            self.handle = create(Host.endpoint, 3 | 0x80000, 8, 1, 65536, 65536, 5000, ctypes.byref(descriptor.attributes))
            if self.handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            descriptor.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        close_handle(self.handle)

    def accept(self):
        connect = api(kernel, 'ConnectNamedPipe', w.BOOL, w.HANDLE, w.LPVOID)
        if not connect(self.handle, None):
            error = ctypes.get_last_error()
            if error == 232:
                disconnect_pipe(self.handle)
                return None
            if error != 535:
                raise ctypes.WinError(error)
        pid = w.DWORD()
        get_pid = api(kernel, 'GetNamedPipeClientProcessId', w.BOOL, w.HANDLE, ctypes.POINTER(w.DWORD))
        try:
            checked(get_pid(self.handle, ctypes.byref(pid)))
            if process_identity(pid.value) != self.identity:
                raise PermissionError('Only the user who authorized this session can control it.')
        except OSError:
            disconnect_pipe(self.handle)
            return None
        return Connection(self.handle, server=True)


class Host:
    endpoint = r'\\.\pipe\AppSandboxDevLauncher'

    def identity(self):
        return process_identity()

    def session_dir(self, identity):
        return Path(os.environ['ProgramData']) / 'AppSandboxDevLauncher'

    def support_dir(self, identity):
        return Path(os.environ['ProgramData']) / 'AppSandbox'

    def require_elevation(self, identity):
        process_identity(elevated=True)
        if not identity.startswith('S-1-') or any(c not in 'S-0123456789' for c in identity):
            raise ValueError('Invalid Windows user SID.')
        if not self.session_active(identity):
            raise RuntimeError('Start the launcher in an interactive Windows user session.')
        directory = self.session_dir(identity)
        descriptor = Descriptor('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;%s)' % identity)
        try:
            create = api(kernel, 'CreateDirectoryW', w.BOOL, w.LPCWSTR, ctypes.POINTER(SecurityAttributes))
            if not create(str(directory), ctypes.byref(descriptor.attributes)) and ctypes.get_last_error() != 183:
                raise ctypes.WinError(ctypes.get_last_error())
            if file_attributes(str(directory)) & 0x400:
                raise RuntimeError('The launcher directory cannot be a reparse point.')
            size = w.DWORD()
            get_file_security(str(directory), 1, None, 0, ctypes.byref(size))
            buffer = ctypes.create_string_buffer(size.value)
            checked(get_file_security(str(directory), 1, buffer, len(buffer), ctypes.byref(size)))
            owner, defaulted = w.LPVOID(), w.BOOL()
            checked(descriptor_owner(buffer, ctypes.byref(owner), ctypes.byref(defaulted)))
            if sid_text(owner) not in ('S-1-5-32-544', 'S-1-5-18'):
                raise RuntimeError('The launcher directory must be owned by Administrators or SYSTEM.')
            checked(set_file_security(str(directory), 4 | 0x80000000, descriptor.pointer))
        finally:
            descriptor.close()

    def session_active(self, identity):
        session = w.DWORD()
        query = api(kernel, 'ProcessIdToSessionId', w.BOOL, w.DWORD, ctypes.POINTER(w.DWORD))
        checked(query(os.getpid(), ctypes.byref(session)))
        return session.value != 0

    def resolve_binary(self, binary):
        return binary.expanduser().resolve()

    def validate_binary(self, binary):
        if binary.resolve() != binary or binary.suffix.lower() != '.exe':
            raise RuntimeError('Specify a fixed Windows executable path.')
        with binary.open('rb') as stream:
            if stream.read(2) != b'MZ':
                raise RuntimeError('The configured executable is not a Windows binary.')

    def process_path(self, pid):
        process = open_process(0x1000, False, pid)
        if not process:
            return None
        try:
            buffer, size = ctypes.create_unicode_buffer(32768), w.DWORD(32768)
            if not process_image(process, 0, buffer, ctypes.byref(size)):
                return None
            return Path(buffer.value).resolve()
        finally:
            close_handle(process)

    def watch(self, pid):
        return ProcessWatch(pid)

    def child_options(self, identity):
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith(('PYTHON', 'COR_', 'CORECLR_', 'COMPLUS_'))}
        return {'env': environment, 'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS}

    def connect(self, identity):
        create = api(kernel, 'CreateFileW', w.HANDLE, w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE)
        wait = api(kernel, 'WaitNamedPipeW', w.BOOL, w.LPCWSTR, w.DWORD)
        deadline = time.monotonic() + 180
        while True:
            handle = create(self.endpoint, 0xC0000000, 0, None, 3, 0, None)
            if handle != ctypes.c_void_p(-1).value:
                break
            error = ctypes.get_last_error()
            if error in (2, 3):
                raise FileNotFoundError('No launcher session is running.')
            if error == 231 and time.monotonic() < deadline:
                wait(self.endpoint, 500)
                continue
            raise ctypes.WinError(error)
        try:
            pid = w.DWORD()
            get_pid = api(kernel, 'GetNamedPipeServerProcessId', w.BOOL, w.HANDLE, ctypes.POINTER(w.DWORD))
            checked(get_pid(handle, ctypes.byref(pid)))
            process_identity(pid.value, elevated=True)
            return Connection(handle)
        except BaseException:
            close_handle(handle)
            raise

    def listen(self, identity):
        return Listener(identity)

    def elevate(self, command):
        try:
            process_identity(elevated=True)
            elevated = True
        except PermissionError:
            elevated = False
        if elevated:
            subprocess.Popen(command, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS)
            return
        execute = api(shell, 'ShellExecuteW', ctypes.c_ssize_t, w.HWND, w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, ctypes.c_int)
        result = execute(None, 'runas', command[0], subprocess.list2cmdline(command[1:]), None, 0)
        if result <= 32:
            raise RuntimeError('Windows did not authorize the development launcher (ShellExecute result %d).' % result)

    def detach(self, identity):
        descriptor = os.open(self.session_dir(identity) / 'launcher.log', os.O_CREAT | os.O_WRONLY | os.O_APPEND, 0o600)
        os.dup2(descriptor, 1)
        os.dup2(descriptor, 2)
        os.close(descriptor)
        with open(os.devnull, 'rb') as stream:
            os.dup2(stream.fileno(), 0)
