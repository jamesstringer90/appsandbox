#!/usr/bin/env python3

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))

class LauncherError(Exception):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def failure(error):
    return {'ok': False, 'error': {'code': getattr(error, 'code', 'io_error'), 'message': str(error)}}


class ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        print(json.dumps(failure(LauncherError('invalid_arguments', message))))
        raise SystemExit(2)


def platform_host():
    if sys.platform == 'darwin':
        from dev_launcher_mac import Host
    elif sys.platform == 'win32':
        from dev_launcher_win import Host
    else:
        raise LauncherError('unsupported_platform', 'Supported hosts are Windows and macOS.')
    return Host()


def request(host, identity, command):
    with host.connect(identity) as connection:
        connection.sendall(json.dumps({'command': command}).encode() + b'\n')
        return receive(connection)


def receive(connection):
    data = bytearray()
    while not data.endswith(b'\n'):
        part = connection.recv(4096)
        if not part:
            raise LauncherError('protocol', 'The launcher connection closed before completing its message.')
        data.extend(part)
        if len(data) > 16384:
            raise LauncherError('protocol', 'The launcher message is too large.')
    return json.loads(data)


class Supervisor:
    def __init__(self, host, identity, binary, arguments):
        self.host, self.identity = host, identity
        self.binary, self.arguments = binary, arguments
        self.discovery = host.support_dir(identity) / 'host.json'
        self.child = None
        self.watcher = None
        self.pid = None
        self.last_exit = None
        self.last_error = None
        self.stopping = False
        sys.path.insert(0, str(Path(__file__).resolve().parent / 'headless-api'))
        from asb import Client
        self.client_type = Client

    def alive(self):
        if self.child is not None and self.child.poll() is not None:
            self.last_exit = self.child.returncode
            self.child = None
            self.pid = None
        if self.watcher is not None and self.watcher.exited():
            self.watcher.close()
            self.watcher = None
            self.pid = None
        return self.pid is not None

    def discover(self):
        data = json.loads(self.discovery.read_text())
        if not isinstance(data, dict):
            raise LauncherError('api_unavailable', 'AppSandbox discovery must contain an object.')
        pid = data.get('pid')
        port = data.get('port')
        token = data.get('token')
        if (type(pid) is not int or not 0 < pid <= 0x7fffffff or type(port) is not int or not 1 <= port <= 65535 or
                data.get('endpoint') != 'http://127.0.0.1:%d' % port or not isinstance(token, str) or not token):
            raise LauncherError('api_unavailable', 'AppSandbox discovery does not describe a process with a loopback API.')
        return pid, self.client_type(data['endpoint'], token)

    def client(self):
        pid, client = self.discover()
        if pid != self.pid:
            raise LauncherError('api_unavailable', 'AppSandbox discovery does not match the supervised process.')
        return client

    def ready(self, client=None):
        try:
            if client is None:
                client = self.client()
            status, body = client._req('GET', '/version', timeout=2)
            return status == 200 and isinstance(body, dict) and body.get('product') == 'AppSandbox'
        except (OSError, ValueError, LauncherError):
            return False

    def status(self):
        running = self.alive()
        return {'ok': True, 'session': not self.stopping, 'launcherPid': os.getpid(),
                'binary': str(self.binary), 'arguments': self.arguments, 'pid': self.pid,
                'running': running, 'ready': running and self.ready(), 'lastExit': self.last_exit,
                'lastError': self.last_error, 'log': str(self.host.session_dir(self.identity) / 'core.log')}

    def launch(self):
        self.host.validate_binary(self.binary)
        if not self.alive():
            self.adopt_running_core()
            if not self.alive():
                with (self.host.session_dir(self.identity) / 'core.log').open('ab', buffering=0) as output:
                    self.child = subprocess.Popen([str(self.binary), *self.arguments], stdin=subprocess.DEVNULL,
                                                  stdout=output, stderr=output, cwd=str(self.binary.parent),
                                                  **self.host.child_options(self.identity))
                self.pid = self.child.pid
                self.last_exit = None
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            if not self.alive():
                raise LauncherError('core_exited', 'AppSandbox exited during startup; inspect core.log.')
            if self.ready():
                self.last_error = None
                return
            time.sleep(0.25)
        raise LauncherError('startup_timeout', 'The process remains running, but the AppSandbox API is not ready; inspect core.log.')

    def adopt_running_core(self):
        try:
            pid, client = self.discover()
        except (OSError, ValueError, LauncherError):
            return
        if self.host.process_path(pid) == self.binary:
            self.watcher = self.host.watch(pid)
            self.pid = pid
        elif self.ready(client):
            raise LauncherError('different_binary', 'Another AppSandbox binary owns the API; stop it before switching binaries.')

    def stop_core(self):
        if not self.alive():
            return
        status, body = self.client().shutdown_daemon(force=True)
        if status != 200 or not isinstance(body, dict) or body.get('stopping') is not True:
            error = body.get('error') if isinstance(body, dict) else None
            if isinstance(error, dict) and isinstance(error.get('code'), str) and isinstance(error.get('message'), str):
                raise LauncherError(error['code'], error['message'])
            raise LauncherError('shutdown_failed', 'AppSandbox did not accept shutdown (HTTP %d).' % status)
        deadline = time.monotonic() + 45
        while self.alive():
            if time.monotonic() >= deadline:
                raise LauncherError('shutdown_timeout', 'AppSandbox is still shutting down. Retry when it has exited.')
            time.sleep(0.1)
        self.last_error = None

    def handle(self, message):
        if not isinstance(message, dict) or set(message) != {'command'}:
            raise LauncherError('invalid_request', 'Expected a command field.')
        command = message['command']
        if command == 'start':
            self.launch()
        elif command == 'restart':
            self.host.validate_binary(self.binary)
            self.stop_core()
            self.launch()
        elif command == 'stop':
            self.stop_core()
        elif command == 'close':
            self.stop_core()
            self.stopping = True
        elif command != 'status':
            raise LauncherError('invalid_request', 'Supported commands are start, status, restart, stop and close.')
        return self.status()


def serve(host, identity, binary, arguments):
    host.require_elevation(identity)
    host.validate_binary(binary)
    with host.listen(identity) as listener:
        host.detach(identity)
        supervisor = Supervisor(host, identity, binary, arguments)
        try:
            try:
                supervisor.launch()
            except (OSError, ValueError, RuntimeError, LauncherError) as error:
                supervisor.last_error = str(error)
            while not supervisor.stopping and host.session_active(identity):
                connection = listener.accept()
                if connection is None:
                    continue
                with connection:
                    try:
                        response = supervisor.handle(receive(connection))
                    except (OSError, ValueError, RuntimeError, TypeError, LauncherError) as error:
                        supervisor.last_error = str(error)
                        response = failure(error)
                    try:
                        connection.sendall(json.dumps(response).encode() + b'\n')
                    except OSError:
                        pass
        finally:
            if supervisor.watcher is not None:
                supervisor.watcher.close()


def start(host, identity, binary, arguments):
    if binary is None:
        if arguments:
            raise LauncherError('invalid_arguments', 'Specify --binary when configuring binary arguments.')
        return request(host, identity, 'start')
    binary = host.resolve_binary(binary)
    host.validate_binary(binary)
    try:
        result = request(host, identity, 'status')
        if result.get('binary') != str(binary) or result.get('arguments') != arguments:
            raise LauncherError('different_binary', 'This session is authorized for another binary or arguments; close it before changing the target.')
        return request(host, identity, 'start')
    except (FileNotFoundError, ConnectionRefusedError):
        pass
    command = [sys.executable, '-I', '-B', str(Path(__file__).resolve()), '_serve', identity, str(binary), json.dumps(arguments)]
    host.elevate(command)
    deadline = time.monotonic() + 15
    while True:
        try:
            result = request(host, identity, 'status')
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.monotonic() >= deadline:
                raise LauncherError('launcher_timeout', 'The authorized launcher did not start; inspect launcher.log.')
            time.sleep(0.1)
    if result.get('lastError'):
        raise LauncherError('startup_failed', result['lastError'])
    return result


def main():
    parser = ArgumentParser(description='Authorize one AppSandbox binary once, then relaunch it programmatically on Windows or macOS.',
                            epilog='JSON output; exit codes 0 success, 1 failure, 2 invalid arguments. Stop keeps authorization alive for a subsequent start. Restart, stop and close shut down any VMs owned by the core. Close, logout or reboot ends the session.')
    commands = parser.add_subparsers(dest='command', required=True)
    create = commands.add_parser('start', help='Authorize a session and launch or adopt the configured binary.')
    create.add_argument('--binary', '--app', type=Path, help='AppSandbox executable, or a macOS .app bundle. Required only when opening a new session.')
    create.add_argument('arguments', nargs=argparse.REMAINDER, help='Fixed binary arguments, following --; for example -- --headless.')
    for name in ('status', 'restart', 'stop', 'close'):
        commands.add_parser(name)
    args = parser.parse_args()
    host = platform_host()
    identity = host.identity()
    if args.command == 'start':
        arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
        try:
            result = start(host, identity, args.binary, arguments)
        except (FileNotFoundError, ConnectionRefusedError):
            raise LauncherError('no_session', 'No launcher session is running; use start --binary first.')
    else:
        try:
            result = request(host, identity, args.command)
        except (FileNotFoundError, ConnectionRefusedError):
            if args.command != 'status':
                raise LauncherError('no_session', 'No launcher session is running; use start --binary first.')
            result = {'ok': True, 'session': False, 'running': False, 'ready': False, 'pid': None}
    print(json.dumps(result))
    return 0 if result.get('ok') else 1


if __name__ == '__main__':
    try:
        if len(sys.argv) == 5 and sys.argv[1] == '_serve':
            serve(platform_host(), sys.argv[2], Path(sys.argv[3]), json.loads(sys.argv[4]))
            sys.exit(0)
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, LauncherError) as error:
        print(json.dumps(failure(error)))
        sys.exit(1)
