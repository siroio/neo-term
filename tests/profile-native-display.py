"""Sample the instruction pointer of an isolated GUI Emacs rendering workload.

Windows x64 only. Suspend only the owned test process's busiest thread, always
resume it in finally. Counts are elapsed-time samples, not CPU percentages;
DLL wait instructions cannot by themselves establish CPU cost or caller stacks.
Requires the Emacs executable's DWARF symbols and MinGW addr2line on PATH.
"""
import collections
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
K = c.WinDLL("kernel32", use_last_error=True)
HANDLE = w.HANDLE
for name, args, result in [
    ("CreateToolhelp32Snapshot", [w.DWORD, w.DWORD], HANDLE),
    ("OpenThread", [w.DWORD, w.BOOL, w.DWORD], HANDLE),
    ("SuspendThread", [HANDLE], w.DWORD),
    ("ResumeThread", [HANDLE], w.DWORD),
    ("GetThreadContext", [HANDLE, c.c_void_p], w.BOOL),
    ("CloseHandle", [HANDLE], w.BOOL),
    ("GetThreadTimes", [HANDLE] + [c.POINTER(w.FILETIME)] * 4, w.BOOL),
]:
    getattr(K, name).argtypes = args
    getattr(K, name).restype = result


class Thread(c.Structure):
    _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("tid", w.DWORD),
                ("pid", w.DWORD), ("priority", w.LONG), ("delta", w.LONG),
                ("flags", w.DWORD)]


class Module(c.Structure):
    _fields_ = [("size", w.DWORD), ("id", w.DWORD), ("pid", w.DWORD),
                ("global_usage", w.DWORD), ("usage", w.DWORD),
                ("base", c.c_void_p), ("length", w.DWORD), ("handle", HANDLE),
                ("name", w.WCHAR * 256), ("path", w.WCHAR * 260)]


def entries(pid, flags, cls, first, next_):
    snap = K.CreateToolhelp32Snapshot(flags, pid)
    if snap == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    try:
        entry = cls()
        entry.size = c.sizeof(cls)
        for fn in (first, next_):
            getattr(K, fn).argtypes = [HANDLE, c.POINTER(cls)]
            getattr(K, fn).restype = w.BOOL
        ok = getattr(K, first)(snap, c.byref(entry))
        while ok:
            yield cls.from_buffer_copy(entry)
            ok = getattr(K, next_)(snap, c.byref(entry))
    finally:
        K.CloseHandle(snap)


def cpu(handle):
    values = [w.FILETIME() for _ in range(4)]
    if not K.GetThreadTimes(handle, *(c.byref(v) for v in values)):
        raise c.WinError(c.get_last_error())
    return sum((v.dwHighDateTime << 32) | v.dwLowDateTime for v in values[2:]) / 1e7


def run():
    ready = ROOT / "build/native-profile.ready"
    ready.unlink(missing_ok=True)
    env = dict(os.environ, NEO_TERM_PROFILE_NATIVE="1")
    exe = env.get("NEO_TERM_EMACS", "C:/Users/horiuchi/scoop/apps/emacs/current/bin/emacs.exe")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    handles = []
    process = subprocess.Popen([exe, "-Q", "-l", str(ROOT / "tests/profile-display.el")],
                               env=env, startupinfo=startup)
    try:
        deadline = time.monotonic() + 45
        while not ready.exists():
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Emacs did not start the rendering workload")
            time.sleep(0.01)
        if int(ready.read_text()) != process.pid:
            raise RuntimeError("Unexpected workload process")
        modules = list(entries(process.pid, 0x8 | 0x10, Module, "Module32FirstW", "Module32NextW"))
        for thread in entries(0, 0x4, Thread, "Thread32First", "Thread32Next"):
            if thread.pid == process.pid:
                handle = K.OpenThread(0x2 | 0x8 | 0x40, False, thread.tid)
                if handle:
                    handles.append((cpu(handle), thread.tid, handle))
        _, tid, handle = max(handles)
        start_cpu = cpu(handle)
        start = time.monotonic()
        samples = collections.Counter()
        # AMD64 CONTEXT is 1232 bytes, aligned to 16 bytes. CONTROL captures RIP.
        storage = c.create_string_buffer(1232 + 15)
        address = (c.addressof(storage) + 15) & ~15
        while process.poll() is None and time.monotonic() < deadline:
            struct.pack_into("I", storage, address - c.addressof(storage) + 48, 0x100001)
            if K.SuspendThread(handle) == 0xFFFFFFFF:
                break
            try:
                if K.GetThreadContext(handle, address):
                    samples[c.c_uint64.from_address(address + 248).value] += 1
            finally:
                K.ResumeThread(handle)
            time.sleep(0.001)
        # Read before process teardown where possible; sampling is intentionally
        # perturbing, so do not compare these timings with uninstrumented runs.
        process.wait(timeout=5)
        mapped = collections.Counter()
        emacs_addresses = {}
        system_calls = collections.Counter()
        win32u = next((m for m in modules if m.name.lower() == "win32u.dll"), None)
        exports = []
        if win32u:
            listing = subprocess.run(["objdump", "-p", win32u.path], capture_output=True,
                                     text=True, check=True).stdout
            rvas = {int(ordinal): int(rva, 16) for ordinal, rva in
                    re.findall(r"\+base\[\s*(\d+)\]\s+([0-9a-f]+) Export RVA", listing)}
            exports = [(rvas[int(ordinal)], name) for ordinal, name in
                       re.findall(r"\+base\[\s*(\d+)\]\s+[0-9a-f]+\s+(Nt\w+)", listing)
                       if int(ordinal) in rvas]
        for ip, count in samples.items():
            module = next((m for m in modules if m.base <= ip < m.base + m.length), None)
            name = module.name if module else "unknown"
            mapped[name] += count
            if module and name.lower().startswith("emacs"):
                emacs_addresses[ip] = 0x400000000 + ip - module.base
            if module and module.name.lower() == "win32u.dll":
                # Accept only an address inside a short exported syscall stub.
                syscall = next((name for rva, name in exports
                                if 0 <= ip - module.base - rva < 32), None)
                system_calls[syscall or "unresolved"] += count
        symbols = {}
        addresses = list(emacs_addresses)
        if addresses:
            result = subprocess.run(["addr2line", "-f", "-C", "-e", exe] +
                                    [hex(emacs_addresses[ip]) for ip in addresses],
                                    capture_output=True, text=True, check=True)
            lines = result.stdout.splitlines()
            for index, ip in enumerate(addresses):
                symbols[ip] = lines[index * 2:index * 2 + 2]
        functions = collections.Counter()
        for ip, symbol in symbols.items():
            functions[symbol[0]] += samples[ip]
        report = dict(pid=process.pid, tid=tid, thread_cpu_at_start=start_cpu,
                      elapsed=time.monotonic() - start, samples=sum(samples.values()),
                      modules=mapped.most_common(), functions=functions.most_common(),
                      system_calls=system_calls.most_common(),
                      addresses=[dict(ip=hex(ip), hits=n, symbol=symbols.get(ip))
                                 for ip, n in samples.most_common()], exit=process.returncode)
        (ROOT / "build/native-display-profile.json").write_text(json.dumps(report, indent=2))
        summary = {k: v for k, v in report.items() if k not in ("addresses", "functions")}
        summary["functions"] = report["functions"][:20]
        print(json.dumps(summary, indent=2))
        if process.returncode:
            raise RuntimeError("Emacs rendering workload failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        for _, _, handle in handles:
            K.CloseHandle(handle)


if __name__ == "__main__":
    run()
