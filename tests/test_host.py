import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import threading
import time
import unittest
import ctypes
import re
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'build' / 'neo-term-host.exe'
FIXTURE = ROOT / 'build' / 'console-fixture.exe'
BACKENDS = ('system-conpty', 'classic') + (('bundled-conpty',) if (HOST.parent / 'runtime' / 'conpty.dll').exists() else ())
KERNEL = ctypes.WinDLL('kernel32', use_last_error=True)
KERNEL.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
KERNEL.OpenProcess.restype = ctypes.c_void_p
KERNEL.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
KERNEL.CloseHandle.argtypes = [ctypes.c_void_p]

class Session:
    def __init__(self, backend, program, extra=(), host=HOST):
        self.process = subprocess.Popen([str(host), '--backend', backend, *extra, '--', *map(str, program)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.events = queue.Queue()
        self.rows = {}
        self.screen = {}
        self.history = []
        self.frames = []
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            while True:
                header = self.process.stdout.read(4)
                if not header:
                    break
                if len(header) != 4:
                    raise AssertionError('truncated frame header')
                length, = struct.unpack('<I', header)
                if not 0 < length <= 4 * 1024 * 1024:
                    raise AssertionError('invalid frame size')
                data = self.process.stdout.read(length)
                if len(data) != length:
                    raise AssertionError('truncated frame payload')
                self.events.put(json.loads(data))
        except Exception as error:
            self.events.put(error)
        finally:
            self.events.put(None)

    def send(self, value):
        payload = value.encode('utf-8')
        self.process.stdin.write(struct.pack('<I', len(payload)) + payload)
        self.process.stdin.flush()

    def until(self, predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            event = self.events.get(timeout=max(.01, deadline - time.monotonic()))
            if isinstance(event, Exception):
                raise event
            if event is None:
                summary = [{k: v for k, v in frame.items() if k not in ('rows', 'history')} for frame in self.frames[-3:]]
                raise AssertionError(f'host ended before expected event; events={summary} screen={self.text()[:300]!r} stderr={self.process.stderr.read()!r}')
            self.frames.append(event)
            if event['type'] == 'screen':
                if (event['cols'], event['height']) != (self.screen.get('cols'), self.screen.get('height')):
                    self.rows.clear()
                for row in event['rows']:
                    self.rows[row[0]] = row[1]
                self.screen = event
                self.history.extend(event.get('history', []))
            if event['type'] == 'error':
                raise AssertionError(event)
            if predicate(event):
                return event
        raise AssertionError('timed out')

    def text(self):
        return '\n'.join(''.join(cell[0] for cell in self.rows[y]) for y in sorted(self.rows))

    def close(self):
        if self.process.poll() is None:
            try:
                self.send('C')
            except (BrokenPipeError, OSError):
                pass
            self.process.stdin.close()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError('host failed to stop')
        self.thread.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()

class HostSpecifications(unittest.TestCase):
    def run_session(self, backend, program, extra=()):
        session = Session(backend, program, extra)
        self.addCleanup(session.close)
        ready = session.until(lambda e: e['type'] == 'ready')
        return session, ready

    def test_cli_output_is_unicode_and_exit_code_is_preserved(self):
        for backend in (*BACKENDS, 'auto'):
            with self.subTest(backend=backend):
                session, ready = self.run_session(backend, [FIXTURE, 'unicode'])
                self.assertIn(ready['backend'], ('system-conpty', 'classic', 'bundled-conpty'))
                expected = '日本語�' if backend == 'classic' else '日本語😀'
                session.until(lambda e: expected in session.text())
                end = session.until(lambda e: e['type'] == 'exit')
                self.assertEqual(end['code'], 7)

    def test_windows_arguments_survive_without_an_extra_shell(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, 'arguments', 'a b\\', 'a"b', '日本語'])
                session.until(lambda e: 'ARG2=a b\\' in session.text() and 'ARG3=a"b' in session.text() and 'ARG4=日本語' in session.text())

    def test_resize_keys_and_interrupt_reach_the_cli(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, 'interactive'])
                session.until(lambda e: 'FIXTURE_READY' in session.text())
                session.send('R100,30')
                session.until(lambda e: e.get('cols') == 100 and e.get('height') == 30)
                session.send('K5,0')
                session.until(lambda e: 'KEY=38' in session.text())
                session.send('T日本語')
                session.until(lambda e: 'CHAR=26085' in session.text())
                session.send('U99,4')
                session.until(lambda e: 'INTERRUPTED' in session.text())

    def test_conpty_prohibition_selects_classic(self):
        session, ready = self.run_session('auto', [FIXTURE, 'unicode'], ['--no-conpty'])
        self.assertEqual(ready['backend'], 'classic')

    def test_pipe_disconnect_releases_owned_processes(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, ready = self.run_session(backend, [FIXTURE, 'interactive'])
                session.until(lambda e: 'FIXTURE_READY' in session.text())
                handle = KERNEL.OpenProcess(0x100000, False, ready['pid'])
                self.assertTrue(handle)
                try:
                    session.process.stdin.close()
                    self.assertEqual(session.process.wait(timeout=8), 0)
                    self.assertEqual(KERNEL.WaitForSingleObject(handle, 3000), 0)
                finally:
                    KERNEL.CloseHandle(handle)

    def test_closing_session_kills_descendants(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, 'descendant'])
                session.until(lambda e: re.search(r'DESCENDANT_PID=\d+', session.text()))
                pid = int(re.search(r'DESCENDANT_PID=(\d+)', session.text())[1])
                handle = KERNEL.OpenProcess(0x100000, False, pid)
                self.assertTrue(handle)
                try:
                    session.close()
                    self.assertEqual(KERNEL.WaitForSingleObject(handle, 3000), 0)
                finally:
                    KERNEL.CloseHandle(handle)

    def test_close_releases_helper_when_screen_output_is_not_read(self):
        for backend in BACKENDS:
            for delay in (.002, .01):
                with self.subTest(backend=backend, delay=delay):
                    cols, rows = ('80', '24') if backend == 'classic' else ('300', '200')
                    process = subprocess.Popen([str(HOST), '--backend', backend, '--cols', cols, '--rows', rows,
                                                '--', str(FIXTURE), 'sleeper'], stdin=subprocess.PIPE,
                                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                               creationflags=subprocess.CREATE_NO_WINDOW)
                    try:
                        length, = struct.unpack('<I', process.stdout.read(4))
                        ready = json.loads(process.stdout.read(length))
                        self.assertEqual(ready['type'], 'ready')
                        time.sleep(delay)
                        process.stdin.write(struct.pack('<I', 1) + b'C')
                        process.stdin.flush()
                        self.assertEqual(process.wait(timeout=3), 0)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                        for stream in (process.stdin, process.stdout, process.stderr):
                            stream.close()

    def test_auto_prefers_bundled_runtime_when_installed(self):
        session, ready = self.run_session('auto', [FIXTURE, 'unicode'])
        expected = 'bundled-conpty' if 'bundled-conpty' in BACKENDS else 'system-conpty'
        self.assertEqual(ready['backend'], expected)

    def test_missing_bundled_runtime_falls_back_before_cli_launch(self):
        with tempfile.TemporaryDirectory(prefix='日本語 helper ', dir=HOST.parent) as directory:
            helper = Path(directory) / 'host.exe'
            shutil.copy2(HOST, helper)
            session = Session('auto', [FIXTURE, 'unicode'], host=helper)
            try:
                ready = session.until(lambda e: e['type'] == 'ready')
                self.assertEqual(ready['backend'], 'system-conpty')
                self.assertEqual([attempt['backend'] for attempt in ready['attempts']], ['bundled-conpty'])
                session.until(lambda e: e['type'] == 'exit')
            finally:
                session.close()

    def test_unicode_executable_path_is_not_passed_through_a_shell(self):
        with tempfile.TemporaryDirectory(prefix='日本語 cli ', dir=HOST.parent) as directory:
            executable = Path(directory) / '検証 CLI.exe'
            shutil.copy2(FIXTURE, executable)
            for backend in BACKENDS:
                with self.subTest(backend=backend):
                    session, _ = self.run_session(backend, [executable, 'unicode'])
                    session.until(lambda e: '日本語' in session.text())
                    session.until(lambda e: e['type'] == 'exit')
                    session.close()

    def test_real_cmd_and_powershell_are_interactive(self):
        for backend in BACKENDS:
            for program in (['cmd.exe', '/Q'], ['powershell.exe', '-NoLogo', '-NoProfile']):
                with self.subTest(backend=backend, program=program):
                    session, _ = self.run_session(backend, program)
                    session.until(lambda e: e['type'] == 'screen')
                    session.send('Techo NEO^_SHELL_OK' if program[0] == 'cmd.exe' else "T'NEO_' + 'SHELL_OK'")
                    session.send('K1,0')
                    session.until(lambda e: 'NEO_SHELL_OK' in session.text())
                    session.send('Texit')
                    session.send('K1,0')
                    session.until(lambda e: e['type'] == 'exit')

    def test_vt_colors_alternate_screen_and_burst_output(self):
        for backend in (kind for kind in BACKENDS if kind != 'classic'):
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, 'vt'])
                session.until(lambda e: e.get('alt') is True and 'ALT_SCREEN' in session.text())
                cells = [cell for row in session.rows.values() for cell in row]
                self.assertTrue(any(cell[2] == 0x12ab34 for cell in cells))
                session.until(lambda e: 'BURST_DONE' in session.text())
                session.until(lambda e: e['type'] == 'exit')

if __name__ == '__main__':
    unittest.main(verbosity=2)
