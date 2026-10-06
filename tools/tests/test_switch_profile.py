import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from switch_profile_report import parse_log, write_report
from profile_process import run_bounded
from profile_render_switch import runtime_environment


def event(ph, name, ts, cat="prepare", tid=1):
    return "CORONA_PROFILE " + json.dumps(dict(ph=ph, name=name, ts=ts, cat=cat, tid=tid))


class SwitchProfileTests(unittest.TestCase):
    def test_function_summary_counts_calls_and_separates_child_time(self):
        report = parse_log("\n".join([
            event("B", "PT_to_ReSTIR", 0, "switch"),
            event("B", "CUDADevice::create_shader", 0, "backend"),
            event("B", "optixModuleCreate", 1000, "optix_module"),
            event("E", "optixModuleCreate", 5000, "optix_module"),
            event("E", "CUDADevice::create_shader", 6000, "backend"),
            event("B", "optixModuleCreate", 6000, "optix_module"),
            event("E", "optixModuleCreate", 8000, "optix_module"),
            event("E", "PT_to_ReSTIR", 8000, "switch"),
        ]))
        functions = {entry["name"]: entry for entry in report["phases"][0]["functions"]}
        self.assertEqual(functions["optixModuleCreate"]["calls"], 2)
        self.assertEqual(functions["optixModuleCreate"]["total_ms"], 6)
        self.assertEqual(functions["CUDADevice::create_shader"]["self_ms"], 2)

    def test_runtime_paths_come_from_ctest_and_selected_runtime_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp)
            tests = build / "tests" / "integration"
            tests.mkdir(parents=True)
            (tests / "CTestTestfile.cmake").write_text('set_tests_properties([=[VisionEmbeddedModeSwitchTests]=] PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:C:/dep/a;PATH=path_list_prepend:C:/dep/b")')
            selected = build / "editor"
            env = runtime_environment(build, selected)
            self.assertEqual(env["PATH"].split(os.pathsep)[:3], [str(selected), "C:/dep/b", "C:/dep/a"])
            self.assertEqual(env["CORONA_SWITCH_PROFILE"], "1")

    def test_nested_time_is_not_counted_twice(self):
        report = parse_log("\n".join([
            event("B", "PT_to_ReSTIR", 0, "switch"),
            event("B", "scene", 0), event("E", "scene", 1000),
            event("B", "compile", 1000, "compile"),
            event("B", "NVRTC", 2000, "nvrtc"), event("E", "NVRTC", 5000, "nvrtc"),
            event("E", "compile", 6000, "compile"),
            event("E", "PT_to_ReSTIR", 10000, "switch"),
        ]))
        phase = report["phases"][0]
        self.assertEqual(phase["duration_ms"], 10)
        self.assertEqual(phase["breakdown_ms"], {"switch": 4, "prepare": 1, "compile": 2, "nvrtc": 3})

    def test_abort_retains_incomplete_stage_without_inventing_a_duration(self):
        report = parse_log("\n".join([
            event("B", "PT_to_ReSTIR", 0, "switch"),
            event("B", "scene", 0), event("E", "scene", 1000),
            event("B", "compile", 1000, "compile"),
            "NVRTC compilation failed",
        ]))
        phase = report["phases"][0]
        self.assertIsNone(phase["duration_ms"])
        self.assertEqual(phase["children"][0]["duration_ms"], 1)
        self.assertEqual(phase["children"][1]["name"], "compile")
        self.assertIn("incomplete", " ".join(report["warnings"]))

    def test_invalid_and_missing_profile_data_are_not_success(self):
        self.assertTrue(parse_log("old executable output")["warnings"])
        self.assertTrue(parse_log("CORONA_PROFILE {broken")["warnings"])

    def test_shader_cache_and_nvrtc_time_stay_with_the_switch(self):
        report = parse_log("\n".join([
            event("B", "PT_to_ReSTIR", 0, "switch"),
            "shader PTX cache hit: kernel_gamma.ptx",
            "shader PTX cache miss: __raygen__DI temporal.ptx",
            "task NVRTC compile __raygen__DI temporal.cu is take 123.45 ms",
            event("E", "PT_to_ReSTIR", 200000, "switch"),
        ]))
        shaders = report["shaders"]
        self.assertEqual(shaders[0]["nvrtc_ms"], 0)
        self.assertEqual(shaders[1]["nvrtc_ms"], 123.45)
        self.assertEqual(shaders[1]["phase"], "PT_to_ReSTIR")

    def test_report_escapes_scene_and_log_content(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = write_report(Path(tmp), {"scene": "<script>alert(1)</script>", "runs": []})
            html = path.read_text(encoding="utf-8")
            self.assertNotIn("<script>alert(1)</script>", html)
            self.assertIn("&lt;script&gt;", html)
            self.assertEqual(json.loads((Path(tmp) / "report.json").read_text(encoding="utf-8"))["runs"], [])

    def test_nvrtc_seconds_are_converted_to_milliseconds(self):
        report = parse_log("\n".join([
            event("B", "PT_to_ReSTIR", 0, "switch"),
            "shader PTX cache miss: __raygen__DI temporal.ptx",
            "task NVRTC compile __raygen__DI temporal.cu is take 22.80 s",
            event("E", "PT_to_ReSTIR", 23000000, "switch"),
        ]))
        self.assertEqual(report["shaders"][0]["nvrtc_ms"], 22800)


class BoundedProcessTests(unittest.TestCase):
    def test_captures_output_and_closed_stdin(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_bounded([sys.executable, "-c", "import sys; print(repr(sys.stdin.read())); print('err', file=sys.stderr)"], Path(tmp), Path(tmp), 10)
            self.assertEqual(result["status"], "success")
            self.assertEqual(result["exit_code"], 0)
            self.assertEqual(Path(result["stdout"]).read_text().strip(), "''")
            self.assertEqual(Path(result["stderr"]).read_text().strip(), "err")

    def test_start_failure_is_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = run_bounded([str(Path(tmp) / "missing.exe")], Path(tmp), Path(tmp), 1)
            self.assertEqual(result["status"], "start_failed")
            self.assertIsNone(result["exit_code"])

    @unittest.skipUnless(os.name == "nt", "Windows Job Object cleanup")
    def test_timeout_kills_descendant_and_records_failure(self):
        import ctypes
        with tempfile.TemporaryDirectory() as tmp:
            code = "import subprocess,sys,time,pathlib; p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)']); pathlib.Path('child.pid').write_text(str(p.pid)); time.sleep(60)"
            result = run_bounded([sys.executable, "-c", code], Path(tmp), Path(tmp), 2)
            self.assertEqual(result["status"], "timeout")
            self.assertTrue(result["cleanup_ok"])
            pid = int((Path(tmp) / "child.pid").read_text())
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x100000, False, pid)
            if handle:
                self.assertEqual(kernel.WaitForSingleObject(handle, 0), 0)
                kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
