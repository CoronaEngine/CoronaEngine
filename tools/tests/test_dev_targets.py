"""`dev.py build A B C` 必须构建全部 target —— 曾经只构建第一个且静默丢弃其余。

回归背景（2026-10-07）：`dev.py` 的 `execute()` 里写的是 `target = targets[0]`，
而同一函数又用 `target_family_for_targets(targets)` 校验了**全部** target，
于是"传多个同族 target"会合法通过、然后只构建第一个。
这直接误导过一次验收：以为 3 个测试目标已重建，实际只有引擎被重建。
"""

import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dev  # noqa: E402
import workflow  # noqa: E402


class CmakeBuildForwardsEveryTargetTests(unittest.TestCase):
    def _record_cmake_command(self, *call_args):
        recorded = []

        def fake_run_command(command, **_kwargs):
            recorded.append(command)

        with mock.patch.object(workflow, "assert_cache_matches_repo"), \
                mock.patch.object(workflow, "load_conan_build_environment", return_value={}), \
                mock.patch.object(workflow, "run_command", side_effect=fake_run_command):
            workflow.cmake_build(*call_args)
        return recorded

    def test_multiple_targets_all_reach_cmake(self):
        recorded = self._record_cmake_command(
            Path("."), "RelWithDebInfo",
            ["corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests"],
            "examples",
        )
        self.assertEqual(1, len(recorded), recorded)
        command = recorded[0]
        self.assertIn("--target", command)
        for target in ("corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests"):
            self.assertIn(target, command, f"{target} 被丢弃了：{command}")

    def test_single_target_still_works(self):
        recorded = self._record_cmake_command(Path("."), "RelWithDebInfo", "corona_engine", "examples")
        self.assertEqual(1, len(recorded))
        self.assertIn("corona_engine", recorded[0])


class ExecuteForwardsEveryParsedTargetTests(unittest.TestCase):
    def test_parser_keeps_every_positional_target(self):
        args = dev.create_parser().parse_args([
            "build", "--target-family", "examples", "--configuration", "RelWithDebInfo",
            "corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests",
        ])
        self.assertEqual(
            ["corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests"],
            args.targets,
        )

    def test_execute_passes_every_target_to_cmake_build(self):
        args = dev.create_parser().parse_args([
            "build", "--target-family", "examples",
            "corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests",
        ])
        with mock.patch.object(dev, "install"), \
                mock.patch.object(dev, "cmake_configure"), \
                mock.patch.object(dev, "cmake_build") as cmake_build:
            dev.execute(args)
        cmake_build.assert_called_once()
        forwarded = cmake_build.call_args.args[2]
        self.assertEqual(
            ["corona_engine", "corona_cef_app_tests", "corona_ui_surface_lifecycle_tests"],
            list(forwarded),
            f"execute() 没有把全部 target 交给 cmake_build：{forwarded!r}",
        )

    def test_execute_falls_back_to_the_default_target(self):
        args = dev.create_parser().parse_args(["build-fast"])
        with mock.patch.object(dev, "ensure_workspace"), \
                mock.patch.object(dev, "cmake_build") as cmake_build:
            dev.execute(args)
        forwarded = list(cmake_build.call_args.args[2])
        self.assertEqual([dev.DEFAULT_TARGET], forwarded)


class TargetFamilyValidationTests(unittest.TestCase):
    def test_mixed_families_are_still_rejected(self):
        with self.assertRaises(ValueError):
            dev.target_family_for_targets(["corona_engine", "some_core_only_target"])

    def test_same_family_is_accepted(self):
        # 两个 `*_tests` 目标同属 tests 族（`corona_engine` 属 examples 族，混传会被拒绝——这是既有行为）。
        self.assertEqual(
            "tests",
            dev.target_family_for_targets(["corona_cef_app_tests", "corona_cef_popup_overlay_tests"]),
        )


if __name__ == "__main__":
    unittest.main()
