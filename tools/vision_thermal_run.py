"""Run a local render test with a fail-closed NVIDIA temperature watchdog.

Only the child process tree started here is stopped. No GPU/OS settings change.
Samples are written continuously, including short runs aborted by the guard.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import queue
import threading
import time
from profile_process import _Job


def _smi_temperatures():
    result = subprocess.run(
        ["nvidia-smi", "--query-gpu=index,temperature.gpu,utilization.gpu,power.draw,clocks.sm,clocks.mem,memory.used",
         "--format=csv,noheader,nounits"],
        capture_output=True, text=True, check=True, timeout=2,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    rows = list(csv.reader(result.stdout.strip().splitlines()))
    if not rows or any(len(row) != 7 for row in rows):
        raise RuntimeError("Temperature telemetry unavailable")
    return [[int(row[0]), float(row[1]), *[x.strip() for x in row[2:]]] for row in rows]


class _Nvml:
    """Read telemetry without spawning nvidia-smi during every measured frame."""
    class Utilization(ctypes.Structure):
        _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]

    class Memory(ctypes.Structure):
        _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong), ("used", ctypes.c_ulonglong)]

    def __init__(self):
        self.library = ctypes.WinDLL("nvml.dll")
        self.check(self.library.nvmlInit_v2())
        count = ctypes.c_uint()
        self.check(self.library.nvmlDeviceGetCount_v2(ctypes.byref(count)))
        self.devices = []
        for index in range(count.value):
            handle = ctypes.c_void_p()
            self.check(self.library.nvmlDeviceGetHandleByIndex_v2(ctypes.c_uint(index), ctypes.byref(handle)))
            self.devices.append(handle)
        if not self.devices:
            raise RuntimeError("NVML reports no GPUs")

    @staticmethod
    def check(code):
        if code != 0:
            raise RuntimeError(f"NVML telemetry failed ({code})")

    def read(self):
        rows = []
        for index, device in enumerate(self.devices):
            temperature, power, clock = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint()
            memory_clock, memory = ctypes.c_uint(), self.Memory()
            utilization = self.Utilization()
            self.check(self.library.nvmlDeviceGetTemperature(device, 0, ctypes.byref(temperature)))
            self.check(self.library.nvmlDeviceGetUtilizationRates(device, ctypes.byref(utilization)))
            self.check(self.library.nvmlDeviceGetPowerUsage(device, ctypes.byref(power)))
            self.check(self.library.nvmlDeviceGetClockInfo(device, 1, ctypes.byref(clock)))
            self.check(self.library.nvmlDeviceGetClockInfo(device, 2, ctypes.byref(memory_clock)))
            self.check(self.library.nvmlDeviceGetMemoryInfo(device, ctypes.byref(memory)))
            rows.append([index, float(temperature.value), utilization.gpu, power.value / 1000,
                         clock.value, memory_clock.value, round(memory.used / 1048576, 1)])
        return rows


_monitor = None


def temperatures():
    global _monitor
    if _monitor is None:
        try:
            _monitor = _Nvml()
        except (OSError, AttributeError):
            _monitor = False
    if not _monitor:
        return _smi_temperatures()
    # A driver call can stall. A daemon reader lets the watchdog still stop its
    # child if fresh telemetry has not arrived within one second.
    result = queue.Queue(maxsize=1)
    def read():
        try:
            result.put(_monitor.read())
        except BaseException as error:
            result.put(error)
    threading.Thread(target=read, daemon=True).start()
    try:
        value = result.get(timeout=1)
    except queue.Empty:
        raise RuntimeError("NVML telemetry timed out") from None
    if isinstance(value, BaseException):
        raise value
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cwd", type=Path, required=True)
    parser.add_argument("--backend", choices=["cuda", "vulkan-compute"], required=True)
    parser.add_argument("--limit", type=float, default=54)
    parser.add_argument("--seconds", type=float, default=600)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not 40 <= args.limit <= 60:
        parser.error("Use a limit between 40 and 60 C")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("Missing command")
    initial = temperatures()
    if max(row[1] for row in initial) >= args.limit - 3:
        raise RuntimeError("GPU needs cooling before launch")
    args.output.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, CORONA_VISION_BACKEND=args.backend)
    # Preserve the identity of the actual deployed binaries with every result.
    # Source state alone cannot establish which incremental-build DLL was tested.
    executable = Path(command[0])
    if not executable.is_absolute():
        executable = args.cwd / executable
    candidates = [executable, args.cwd / f"ocarina-backend-{args.backend}.dll"]
    binary_fingerprints = {
        str(path.resolve()): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in candidates if path.is_file()
    }
    start = time.monotonic()
    reason = "completed"
    process = None
    job = _Job()
    maximum = max(row[1] for row in initial)
    try:
        with (args.output / "stdout.log").open("w", encoding="utf-8") as out, \
             (args.output / "stderr.log").open("w", encoding="utf-8") as err, \
             (args.output / "temperature.csv").open("w", newline="", encoding="utf-8") as samples:
            writer = csv.writer(samples)
            writer.writerow(["elapsed_s", "gpu", "temperature_c", "utilization_pct", "power_w", "sm_mhz", "memory_mhz", "memory_used_mib"])
            process = subprocess.Popen(command, cwd=args.cwd, env=environment, stdout=out, stderr=err,
                                       stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NO_WINDOW)
            job.attach(process)
            (args.output / "pid.txt").write_text(str(process.pid))
            print(f"Started PID {process.pid}, backend={args.backend}, cutoff={args.limit} C", flush=True)
            while process.poll() is None:
                rows = temperatures()  # Failure also terminates the child in finally.
                elapsed = time.monotonic() - start
                for row in rows:
                    writer.writerow([round(elapsed, 3), *row])
                samples.flush()
                maximum = max(maximum, max(row[1] for row in rows))
                if maximum >= args.limit:
                    reason = "thermal_cutoff"
                    break
                if elapsed >= args.seconds:
                    reason = "time_limit"
                    break
                if (args.output / "stop").exists():
                    reason = "requested_stop"
                    break
                time.sleep(0.1)
            if reason == "completed" and process.poll() not in (None, 0):
                reason = "nonzero_exit"
    except BaseException:
        reason = "watchdog_error"
        raise
    finally:
        cleanup_ok = job.cleanup()
        job.close()
        cleanup_error = None
        try:
            if process is not None and process.poll() is None:
                subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                               capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW, timeout=10)
                process.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired) as error:
            cleanup_ok = False
            cleanup_error = str(error)
        summary = dict(backend=args.backend, command=command, reason=reason,
                       binary_fingerprints=binary_fingerprints,
                       telemetry="nvml" if _monitor else "nvidia-smi",
                       max_temperature_c=maximum, cutoff_temperature_c=args.limit,
                       elapsed_s=time.monotonic() - start,
                       returncode=process.returncode if process else None, cleanup_ok=cleanup_ok,
                       cleanup_error=cleanup_error)
        (args.output / "run.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(json.dumps(summary), flush=True)


if __name__ == "__main__":
    main()
