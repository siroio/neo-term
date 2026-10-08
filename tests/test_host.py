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
import base64

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "build" / "neo-term-host.exe"
FIXTURE = ROOT / "build" / "console-fixture.exe"
BACKENDS = ("system-conpty", "classic") + (
    ("bundled-conpty",) if (HOST.parent / "runtime" / "conpty.dll").exists() else ()
)
KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
KERNEL.OpenProcess.restype = ctypes.c_void_p
KERNEL.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
KERNEL.CloseHandle.argtypes = [ctypes.c_void_p]


class Session:
    def __init__(self, backend, program, extra=(), host=HOST):
        self.process = subprocess.Popen(
            [str(host), "--backend", backend, *extra, "--", *map(str, program)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
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
                    raise AssertionError("truncated frame header")
                (length,) = struct.unpack("<I", header)
                if not 0 < length <= 4 * 1024 * 1024:
                    raise AssertionError("invalid frame size")
                data = self.process.stdout.read(length)
                if len(data) != length:
                    raise AssertionError("truncated frame payload")
                self.events.put(json.loads(data))
        except Exception as error:
            self.events.put(error)
        finally:
            self.events.put(None)

    def send(self, value):
        payload = value.encode("utf-8")
        self.process.stdin.write(struct.pack("<I", len(payload)) + payload)
        self.process.stdin.flush()

    def until(self, predicate, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            event = self.events.get(timeout=max(0.01, deadline - time.monotonic()))
            if isinstance(event, Exception):
                raise event
            if event is None:
                summary = [
                    {k: v for k, v in frame.items() if k not in ("rows", "history")}
                    for frame in self.frames[-3:]
                ]
                raise AssertionError(
                    f"host ended before expected event; events={summary} screen={self.text()[:300]!r} stderr={self.process.stderr.read()!r}"
                )
            self.frames.append(event)
            if event["type"] == "screen":
                if (event["cols"], event["height"]) != (
                    self.screen.get("cols"),
                    self.screen.get("height"),
                ):
                    self.rows.clear()
                for row in event["rows"]:
                    self.rows[row[0]] = row[1]
                self.screen = event
                if event.get("history_cleared"):
                    self.history.clear()
                self.history.extend(event.get("history", []))
            if event["type"] == "error":
                raise AssertionError(event)
            if predicate(event):
                return event
        raise AssertionError("timed out")

    def text(self):
        return "\n".join("".join(cell[0] for cell in self.rows[y]) for y in sorted(self.rows))

    def close(self):
        if self.process.poll() is None:
            try:
                self.send("C")
            except (BrokenPipeError, OSError):
                pass
            self.process.stdin.close()
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("host failed to stop")
        self.thread.join(timeout=2)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


class HostSpecifications(unittest.TestCase):
    def run_session(self, backend, program, extra=()):
        session = Session(backend, program, extra)
        self.addCleanup(session.close)
        ready = session.until(lambda e: e["type"] == "ready")
        return session, ready

    def test_cli_output_is_unicode_and_exit_code_is_preserved(self):
        for backend in (*BACKENDS, "auto"):
            with self.subTest(backend=backend):
                session, ready = self.run_session(backend, [FIXTURE, "unicode"])
                self.assertIn(ready["backend"], ("system-conpty", "classic", "bundled-conpty"))
                expected = "日本語�" if backend == "classic" else "日本語😀"
                session.until(lambda e: expected in session.text())
                end = session.until(lambda e: e["type"] == "exit")
                self.assertEqual(end["code"], 7)

    def test_windows_arguments_survive_without_an_extra_shell(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(
                    backend, [FIXTURE, "arguments", "a b\\", 'a"b', "日本語"]
                )
                session.until(
                    lambda e: "ARG2=a b\\" in session.text()
                    and 'ARG3=a"b' in session.text()
                    and "ARG4=日本語" in session.text()
                )

    def test_resize_keys_and_interrupt_reach_the_cli(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, "interactive"])
                session.until(lambda e: "FIXTURE_READY" in session.text())
                session.send("R100,30")
                session.until(lambda e: e.get("cols") == 100 and e.get("height") == 30)
                session.send("K5,0")
                session.until(lambda e: "KEY=38" in session.text())
                session.send("T日本語")
                session.until(lambda e: "CHAR=26085" in session.text())
                session.send("U99,4")
                session.until(lambda e: "INTERRUPTED" in session.text())

    def test_conpty_prohibition_selects_classic(self):
        session, ready = self.run_session("auto", [FIXTURE, "unicode"], ["--no-conpty"])
        self.assertEqual(ready["backend"], "classic")

    def test_screen_clear_preserves_the_cli_and_accepts_further_input(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, ready = self.run_session(backend, [FIXTURE, "interactive"])
                session.until(lambda e: "FIXTURE_READY" in session.text())
                session.send("L")
                session.until(lambda e: e["type"] == "screen" and not session.text().strip())
                self.assertIsNone(session.process.poll())
                self.assertGreater(ready["pid"], 0)
                session.send("Tz")
                session.until(lambda e: "CHAR=122" in session.text())
                session.send("H")
                session.until(lambda e: e.get("history_cleared") is True)
                self.assertIn("CHAR=122", session.text())

    def test_cmd_and_powershell_continue_after_native_and_shell_clear(self):
        programs = (("cmd.exe", "/Q"), ("powershell.exe", "-NoLogo", "-NoProfile"))
        for backend in BACKENDS:
            for program in programs:
                with self.subTest(backend=backend, program=program[0]):
                    session, _ = self.run_session(backend, program)
                    session.until(lambda e: e["type"] == "screen" and ">" in session.text())
                    session.send("L")
                    session.until(lambda e: e["type"] == "screen" and not session.text().strip())
                    session.send("Tcls")
                    session.send("K1,0")
                    session.until(lambda e: e["type"] == "screen" and ">" in session.text())
                    session.send(
                        "Techo CLEAR^_SHELL_OK"
                        if program[0] == "cmd.exe"
                        else "T'CLEAR_' + 'SHELL_OK'"
                    )
                    session.send("K1,0")
                    session.until(lambda e: "CLEAR_SHELL_OK" in session.text())
                    pending_input = "echo PENDING^_" if program[0] == "cmd.exe" else "'PENDING_' + "
                    session.send("T" + pending_input)
                    session.until(lambda e: pending_input in session.text())
                    session.send("L")
                    session.until(lambda e: e["type"] == "screen" and not session.text().strip())
                    session.send("TSHELL_OK" if program[0] == "cmd.exe" else "T'SHELL_OK'")
                    session.send("K1,0")
                    session.until(lambda e: "PENDING_SHELL_OK" in session.text())

    def test_populated_scrollback_survives_screen_clear_until_history_clear(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, "history"])
                session.until(lambda e: "FIXTURE_READY" in session.text())
                expected_history = 302 - session.screen["height"]
                if len(session.history) < expected_history:
                    session.until(lambda e: len(session.history) == expected_history)
                self.assertEqual(len(session.history), expected_history)
                previous = session.history.copy()
                session.send("L")
                session.until(lambda e: e["type"] == "screen" and not session.text().strip())
                self.assertEqual(session.history, previous)
                session.send("H")
                session.until(lambda e: e.get("history_cleared") is True)
                self.assertEqual(session.history, [])
                session.send("Tz")
                session.until(lambda e: "CHAR=122" in session.text())

    def test_scrollback_retains_color_and_distinguishes_wraps_from_line_breaks(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, _ = self.run_session(
                    backend,
                    [FIXTURE, "rich-history"],
                    ("--cols", "40" if backend == "classic" else "10", "--rows", "4"),
                )
                session.until(lambda e: "RICH_READY" in session.text())
                rows = [row for frame in session.frames for row in frame.get("history_rows", [])]
                self.assertTrue(rows, "history must carry colored cells")
                colored = [row for row in rows if any("A" == cell[0] for cell in row[0])]
                self.assertTrue(colored, "the first colored row must survive scrolling")
                self.assertNotEqual(colored[0][0][0][2], -1)
                if backend != "classic":
                    self.assertTrue(any(row[1] for row in rows), "soft wraps must be tracked")
                    self.assertTrue(any(not row[1] for row in rows), "hard breaks must remain")
                session.send("R60,4")
                session.until(lambda e: e.get("cols") == 60)
                session.send("R40,6")
                session.until(lambda e: e.get("cols") == 40 and e.get("height") == 6)

    def test_resizing_live_wrapped_text_preserves_characters_and_cursor(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(
                    backend, [FIXTURE, "reflow"], ("--cols", "10", "--rows", "6")
                )
                session.until(lambda e: "ABCDEFGHIJ" in session.text() and "KLMN" in session.text())
                self.assertTrue(session.screen["continuations"][1])
                session.send("R20,6")
                session.until(lambda e: e.get("cols") == 20 and "ABCDEFGHIJKLMN" in session.text())
                self.assertEqual(session.screen["y"], 0)
                session.send("R8,6")
                session.until(lambda e: e.get("cols") == 8 and "IJKLMN" in session.text())
                self.assertEqual(
                    "".join(line.rstrip() for line in session.text().splitlines()), "ABCDEFGHIJKLMN"
                )
                self.assertTrue(session.screen["continuations"][1])

    def test_resize_preserves_wrap_between_scrollback_and_live_screen(self):
        script = "[Console]::Write('ABCDEFGHIJKLMNOPQRST'); Start-Sleep -Seconds 30"
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(
                    backend,
                    ["powershell.exe", "-NoProfile", "-Command", script],
                    ("--cols", "4", "--rows", "2"),
                )
                session.until(lambda e: "MNOP" in session.text() and "QRST" in session.text())
                self.assertTrue(session.screen["continuations"][0])
                session.send("R8,2")
                session.until(lambda e: e.get("cols") == 8 and "MNOPQRST" in session.text())
                self.assertTrue(
                    session.screen["continuations"][0],
                    "screen must still continue its preceding history row",
                )

    def test_small_window_keeps_wrapped_text_in_history_and_live_screen(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(
                    backend, [FIXTURE, "reflow"], ("--cols", "10", "--rows", "6")
                )
                session.until(lambda e: "KLMN" in session.text())
                session.send("R2,2")
                session.until(lambda e: e.get("cols") == 2 and "MN" in session.text())
                self.assertEqual(
                    "".join(session.history)
                    + "".join(line.rstrip() for line in session.text().splitlines()),
                    "ABCDEFGHIJKLMN",
                )

    def test_input_after_repeated_resize_does_not_overwrite_the_last_character(self):
        if "bundled-conpty" not in BACKENDS:
            self.skipTest("bundled ConPTY is required for pending-wrap cursor reflow")
        session, _ = self.run_session(
            "bundled-conpty", [FIXTURE, "reflow-cursor"], ("--cols", "4", "--rows", "6")
        )
        session.until(lambda e: "A日B" in session.text())
        for width in (2, 3, 4, 7):
            session.send(f"R{width},6")
            session.until(lambda e: e.get("cols") == width)
        session.send("TZ")
        session.until(lambda e: "BZ" in session.text())
        self.assertIn("A日BZ", session.text())

    def test_classic_resize_does_not_collect_existing_history_twice(self):
        session, _ = self.run_session(
            "classic", [FIXTURE, "history"], ("--cols", "40", "--rows", "4")
        )
        session.until(lambda e: "FIXTURE_READY" in session.text())
        previous = session.history.copy()
        session.send("R60,4")
        session.until(lambda e: e.get("cols") == 60)
        session.send("Ty")
        session.until(lambda e: "CHAR=121" in session.text())
        self.assertEqual(session.history[: len(previous)], previous)
        self.assertLessEqual(sum(line.strip() == "history 0" for line in session.history), 1)

    def test_powershell_prompt_reports_directory_and_keeps_custom_prompt(self):
        script = ROOT / "shell" / "neo-term.ps1"
        destination = ROOT / "build"
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                startup = "function prompt { 'CUSTOM> ' }; " + script.read_text(encoding="utf-8")
                session, _ = self.run_session(
                    backend,
                    [
                        "powershell.exe",
                        "-NoLogo",
                        "-NoProfile",
                        "-NoExit",
                        "-EncodedCommand",
                        base64.b64encode(startup.encode("utf-16le")).decode("ascii"),
                    ],
                )
                event = session.until(lambda e: e.get("title", "").startswith("neo-term;"))
                metadata = json.loads(base64.b64decode(event["title"][9:]).decode("utf-8"))
                self.assertEqual(metadata["prompt"], "CUSTOM> ")
                if "CUSTOM>" not in session.text():
                    session.until(lambda e: "CUSTOM>" in session.text())
                session.send(f"TSet-Location '{destination.as_posix()}'")
                session.send("K1,0")

                def changed_directory(event):
                    title = event.get("title", "")
                    if not title.startswith("neo-term;"):
                        return False
                    data = json.loads(base64.b64decode(title[9:]).decode("utf-8"))
                    return Path(data["directory"]) == destination

                session.until(changed_directory)

    def test_powershell_custom_prompt_preserves_failed_status_and_native_exit_code(self):
        script = (ROOT / "shell" / "neo-term.ps1").read_text(encoding="utf-8")
        code = (
            "function prompt { 'status:' + $? + ':' + $global:LASTEXITCODE }; "
            + script
            + "\n$global:LASTEXITCODE = 7; "
            + "Get-Item -LiteralPath 'E:/neo-emacs/neo-term/build/does-not-exist' "
            + "-ErrorAction SilentlyContinue; prompt"
        )
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-EncodedCommand",
                base64.b64encode(code.encode("utf-16le")).decode("ascii"),
            ],
            capture_output=True,
            timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.assertIn(b"status:False:7", result.stdout)

    def test_osc52_unicode_clipboard_data_reaches_the_frontend(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, "clipboard"])
                event = session.until(lambda e: bool(e.get("clipboard")))
                self.assertEqual(event["clipboard"], ["日本語😀"])

    def test_mouse_input_reaches_a_requesting_console_application(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, "mouse"])
                session.until(lambda e: "FIXTURE_READY" in session.text())
                if session.screen.get("mouse", 0) == 0:
                    self.assertEqual(backend, "system-conpty")
                    continue
                session.send("M2,4,1,0")
                session.until(lambda e: "MOUSE=4,2 BUTTONS=1" in session.text())
                session.send("M2,4,-1,0")
                session.until(lambda e: "MOUSE=4,2 BUTTONS=0" in session.text())

    def test_vim_edits_and_saves_a_file(self):
        vim = os.environ.get("NEO_TERM_TEST_VIM") or shutil.which("vim.exe")
        if not vim:
            self.skipTest("set NEO_TERM_TEST_VIM to run a real Vim integration")
        for backend in BACKENDS:
            with self.subTest(backend=backend), tempfile.TemporaryDirectory(
                dir=HOST.parent
            ) as folder:
                path = Path(folder) / "vim-result.txt"
                session, _ = self.run_session(
                    backend, [vim, "-Nu", "NONE", "-n", "-i", "NONE", str(path)]
                )
                session.until(lambda e: "vim-result.txt" in session.text())
                session.send("TiNEO_VIM_OK")
                session.send("K4,0")
                session.send("T:wq")
                session.send("K1,0")
                session.until(lambda e: e["type"] == "exit")
                self.assertEqual(path.read_text(encoding="utf-8").strip(), "NEO_VIM_OK")

    def test_pipe_disconnect_releases_owned_processes(self):
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                session, ready = self.run_session(backend, [FIXTURE, "interactive"])
                session.until(lambda e: "FIXTURE_READY" in session.text())
                handle = KERNEL.OpenProcess(0x100000, False, ready["pid"])
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
                session, _ = self.run_session(backend, [FIXTURE, "descendant"])
                session.until(lambda e: re.search(r"DESCENDANT_PID=\d+", session.text()))
                pid = int(re.search(r"DESCENDANT_PID=(\d+)", session.text())[1])
                handle = KERNEL.OpenProcess(0x100000, False, pid)
                self.assertTrue(handle)
                try:
                    session.close()
                    self.assertEqual(KERNEL.WaitForSingleObject(handle, 3000), 0)
                finally:
                    KERNEL.CloseHandle(handle)

    def test_close_releases_helper_when_screen_output_is_not_read(self):
        for backend in BACKENDS:
            for delay in (0.002, 0.01):
                with self.subTest(backend=backend, delay=delay):
                    cols, rows = ("80", "24") if backend == "classic" else ("300", "200")
                    process = subprocess.Popen(
                        [
                            str(HOST),
                            "--backend",
                            backend,
                            "--cols",
                            cols,
                            "--rows",
                            rows,
                            "--",
                            str(FIXTURE),
                            "sleeper",
                        ],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        creationflags=subprocess.CREATE_NO_WINDOW,
                    )
                    try:
                        (length,) = struct.unpack("<I", process.stdout.read(4))
                        ready = json.loads(process.stdout.read(length))
                        self.assertEqual(ready["type"], "ready")
                        time.sleep(delay)
                        process.stdin.write(struct.pack("<I", 1) + b"C")
                        process.stdin.flush()
                        self.assertEqual(process.wait(timeout=3), 0)
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait()
                        for stream in (process.stdin, process.stdout, process.stderr):
                            stream.close()

    def test_auto_prefers_bundled_runtime_when_installed(self):
        session, ready = self.run_session("auto", [FIXTURE, "unicode"])
        expected = "bundled-conpty" if "bundled-conpty" in BACKENDS else "system-conpty"
        self.assertEqual(ready["backend"], expected)

    def test_missing_bundled_runtime_falls_back_before_cli_launch(self):
        with tempfile.TemporaryDirectory(prefix="日本語 helper ", dir=HOST.parent) as directory:
            helper = Path(directory) / "host.exe"
            shutil.copy2(HOST, helper)
            session = Session("auto", [FIXTURE, "unicode"], host=helper)
            try:
                ready = session.until(lambda e: e["type"] == "ready")
                self.assertEqual(ready["backend"], "system-conpty")
                self.assertEqual(
                    [attempt["backend"] for attempt in ready["attempts"]], ["bundled-conpty"]
                )
                session.until(lambda e: e["type"] == "exit")
            finally:
                session.close()

    def test_unicode_executable_path_is_not_passed_through_a_shell(self):
        with tempfile.TemporaryDirectory(prefix="日本語 cli ", dir=HOST.parent) as directory:
            executable = Path(directory) / "検証 CLI.exe"
            shutil.copy2(FIXTURE, executable)
            for backend in BACKENDS:
                with self.subTest(backend=backend):
                    session, _ = self.run_session(backend, [executable, "unicode"])
                    session.until(lambda e: "日本語" in session.text())
                    session.until(lambda e: e["type"] == "exit")
                    session.close()

    def test_real_cmd_and_powershell_are_interactive(self):
        for backend in BACKENDS:
            for program in (["cmd.exe", "/Q"], ["powershell.exe", "-NoLogo", "-NoProfile"]):
                with self.subTest(backend=backend, program=program):
                    session, _ = self.run_session(backend, program)
                    session.until(lambda e: e["type"] == "screen")
                    session.send(
                        "Techo NEO^_SHELL_OK" if program[0] == "cmd.exe" else "T'NEO_' + 'SHELL_OK'"
                    )
                    session.send("K1,0")
                    session.until(lambda e: "NEO_SHELL_OK" in session.text())
                    session.send("Texit")
                    session.send("K1,0")
                    session.until(lambda e: e["type"] == "exit")

    def test_vt_colors_alternate_screen_and_burst_output(self):
        for backend in (kind for kind in BACKENDS if kind != "classic"):
            with self.subTest(backend=backend):
                session, _ = self.run_session(backend, [FIXTURE, "vt"])
                session.until(lambda e: e.get("alt") is True and "ALT_SCREEN" in session.text())
                cells = [cell for row in session.rows.values() for cell in row]
                self.assertTrue(any(cell[2] == 0x12AB34 for cell in cells))
                session.until(lambda e: "BURST_DONE" in session.text())
                session.until(lambda e: e["type"] == "exit")


if __name__ == "__main__":
    unittest.main(verbosity=2)
