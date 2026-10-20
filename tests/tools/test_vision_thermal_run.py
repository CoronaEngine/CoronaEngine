"""CPU-only regression coverage for failures that must stop a render matrix."""
from contextlib import ExitStack
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import vision_thermal_run as guard


def telemetry(temperature=51, memory=1000):
    return [[0, temperature, 0, 40, 1200, 7000, memory]]


class ThermalGuardTests(unittest.TestCase):
    def test_driver_snapshot_keeps_exited_renderer_and_watched_unknown_pid(self):
        output = ('111, C:\\Windows\\explorer.exe\n'
                  '222, E:\\build\\corona_engine.exe\n'
                  '333, [Insufficient Permissions]\n')
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout=output)):
            self.assertEqual([row["pid"] for row in guard.renderer_gpu_processes([333])], [222, 333])

    def test_invalid_driver_telemetry_fails_closed(self):
        with patch.object(guard.subprocess, "run", return_value=Mock(stdout="driver unavailable")):
            with self.assertRaises(RuntimeError):
                guard.renderer_gpu_processes()

    def test_waits_for_driver_release_after_process_exit(self):
        remaining = [{"pid": 222}]
        with patch.object(guard, "renderer_gpu_processes", side_effect=[remaining, []]), \
             patch.object(guard.time, "sleep") as sleep:
            self.assertEqual(guard.wait_for_gpu_release(222), [])
            sleep.assert_called_once()

    def test_driver_residue_survives_cleanup_deadline(self):
        remaining = [{"pid": 222}]
        with patch.object(guard, "renderer_gpu_processes", return_value=remaining):
            self.assertEqual(guard.wait_for_gpu_release(222, timeout=0), remaining)

    def run_guard(self, *, existing=(), residue=(), rows=None, returncode=0, polls=None):
        with tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
            process = Mock(pid=222, returncode=returncode)
            process.poll.side_effect = polls
            process.poll.return_value = returncode
            launcher = stack.enter_context(patch.object(guard.subprocess, "Popen", return_value=process))
            stack.enter_context(patch.object(guard, "renderer_gpu_processes", return_value=list(existing)))
            stack.enter_context(patch.object(guard, "wait_for_gpu_release", return_value=list(residue)))
            stack.enter_context(patch.object(guard, "temperatures", side_effect=rows or [telemetry(), telemetry()]))
            job = Mock()
            job.cleanup.return_value = True
            stack.enter_context(patch.object(guard, "_Job", return_value=job))
            stack.enter_context(patch("builtins.print"))
            stack.enter_context(patch.object(sys, "argv", ["guard", "--output", directory, "--cwd", directory,
                                                          "--limit", "60",
                                                          "--", "missing-cpu-test.exe"]))
            code = guard.main()
            result = json.loads((Path(directory) / "run.json").read_text())
            return code, result, launcher.call_count, job.cleanup.call_count

    def test_existing_gpu_context_prevents_launch(self):
        code, result, launches, _ = self.run_guard(existing=[{"pid": 222, "executable": "corona_engine.exe"}])
        self.assertEqual(code, 1)
        self.assertEqual(result["reason"], "preflight_blocked")
        self.assertEqual(launches, 0)

    def test_success_records_memory_and_accepts_temperature_below_exact_limit(self):
        code, result, launches, cleanups = self.run_guard(rows=[telemetry(59, 1000), telemetry(59, 1004)])
        self.assertEqual((code, launches, cleanups), (0, 1, 1))
        self.assertEqual(result["memory_delta_mib"], {"0": 4})
        self.assertTrue(result["gpu_cleanup_ok"])

    def test_signaled_process_with_driver_residue_is_failure(self):
        code, result, _, _ = self.run_guard(residue=[{"pid": 222}])
        self.assertEqual(code, 1)
        self.assertEqual(result["reason"], "cleanup_failed")
        self.assertFalse(result["gpu_cleanup_ok"])

    def test_nonzero_child_exit_propagates(self):
        code, result, _, _ = self.run_guard(returncode=7)
        self.assertEqual(code, 1)
        self.assertEqual(result["reason"], "nonzero_exit")

    def test_thermal_cutoff_stops_at_60(self):
        code, result, _, cleanups = self.run_guard(
            rows=[telemetry(), telemetry(60), telemetry(59)], polls=[None, 0])
        self.assertEqual((code, cleanups), (1, 1))
        self.assertEqual(result["reason"], "thermal_cutoff")
        self.assertEqual(result["max_temperature_c"], 60)

    def test_missing_live_telemetry_still_cleans_up_and_fails(self):
        code, result, _, cleanups = self.run_guard(
            rows=[telemetry(), RuntimeError("NVML timed out"), telemetry()], polls=[None, 0])
        self.assertEqual((code, cleanups), (1, 1))
        self.assertEqual(result["reason"], "watchdog_error")


if __name__ == "__main__":
    unittest.main()
