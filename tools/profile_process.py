"""Bounded unattended Windows launches, with logs and a private cleanup job."""
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time


class _Job:
    def __init__(self):
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        for name, args, result in (
            ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            ("SetInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            ("TerminateJobObject", [wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            ("QueryInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
            ("OpenProcess", [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            ("WaitForSingleObject", [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            ("SetErrorMode", [wintypes.UINT], wintypes.UINT),
        ):
            fn = getattr(self.api, name)
            fn.argtypes, fn.restype = args, result
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        # JOBOBJECT_EXTENDED_LIMIT_INFORMATION, Windows x64 ABI.
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            self.api.CloseHandle(self.handle)
            raise RuntimeError("The profiler launcher requires 64-bit Python")
        limits = ctypes.create_string_buffer(144)
        ctypes.c_uint32.from_buffer(limits, 16).value = 0x2000  # KILL_ON_JOB_CLOSE
        if not self.api.SetInformationJobObject(self.handle, 9, limits, 144):
            error = ctypes.WinError(ctypes.get_last_error())
            self.api.CloseHandle(self.handle)
            raise error
        self.old_mode = self.api.SetErrorMode(0x8003)

    def attach(self, process):
        if not self.api.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def cleanup(self):
        ids = ctypes.create_string_buffer(8 + 1024 * 8)
        queried = self.api.QueryInformationJobObject(self.handle, 3, ids, len(ids), None)
        handles = []
        if queried:
            count = ctypes.c_uint32.from_buffer(ids, 4).value
            for index in range(count):
                pid = ctypes.c_size_t.from_buffer(ids, 8 + index * 8).value
                handle = self.api.OpenProcess(0x100000, False, pid)
                if handle:
                    handles.append(handle)
        terminated = self.api.TerminateJobObject(self.handle, 124)
        deadline = time.monotonic() + 10
        try:
            for handle in handles:
                remaining = max(0, int((deadline - time.monotonic()) * 1000))
                if self.api.WaitForSingleObject(handle, remaining) != 0:
                    return False
            while time.monotonic() < deadline:
                info = ctypes.create_string_buffer(48)
                if not self.api.QueryInformationJobObject(self.handle, 1, info, 48, None):
                    return False
                if ctypes.c_uint32.from_buffer(info, 40).value == 0:
                    return bool(queried and terminated)
                time.sleep(0.02)
            return False
        finally:
            for handle in handles:
                self.api.CloseHandle(handle)

    def close(self):
        self.api.CloseHandle(self.handle)
        self.api.SetErrorMode(self.old_mode)


def run_bounded(command, cwd: Path, output: Path, timeout: float, env=None):
    """Return an explicit outcome even for missing DLLs, crashes and timeouts.

    Logs stream directly to disk. No console input or modal error UI is needed.
    Descendants belong to this launch's Job Object, never a process-name filter.
    """
    output.mkdir(parents=True, exist_ok=True)
    result = dict(command=list(map(str, command)), cwd=str(cwd), pid=None,
                  timeout_seconds=timeout, started_utc=datetime.now(timezone.utc).isoformat(),
                  status="start_failed", exit_code=None, cleanup_ok=True,
                  stdout=str(output / "stdout.log"), stderr=str(output / "stderr.log"))
    started = time.monotonic()
    job = process = None
    try:
        if os.name != "nt":
            raise RuntimeError("This renderer profiler currently requires Windows")
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        job = _Job()
        with open(result["stdout"], "wb") as stdout, open(result["stderr"], "wb") as stderr:
            process = subprocess.Popen(result["command"], cwd=cwd, env=env,
                                       stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            result["pid"] = process.pid
            job.attach(process)
            try:
                result["exit_code"] = process.wait(timeout=timeout)
                result["status"] = "success" if result["exit_code"] == 0 else "nonzero_exit"
            except subprocess.TimeoutExpired:
                result["status"] = "timeout"
    except Exception as error:
        result["error"] = str(error)
    finally:
        if job:
            result["cleanup_ok"] = job.cleanup()
            job.close()
        if process:
            try:
                if process.poll() is None:
                    process.kill()  # Also covers failure to attach to the Job Object.
                result["exit_code"] = process.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired) as error:
                result["cleanup_ok"] = False
                result["cleanup_error"] = str(error)
        if not result["cleanup_ok"]:
            result["status"] = "cleanup_failed"
            result.setdefault("cleanup_error", "Could not confirm all Job Object processes exited within the cleanup deadline")
        result["elapsed_ms"] = (time.monotonic() - started) * 1000
        (output / "process.json").write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    return result
