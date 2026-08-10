from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from horizon_workspace import load_lock
from dev import conan_options, target_family_for_target, target_family_for_targets
from workflow import (
    DEFAULT_TARGET_FAMILY,
    TARGET_FAMILIES,
    _cmake_bracket,
    _environment_value,
    _set_environment_value,
    build_dir,
    configuration_slug,
    preset_name,
    safe_remove,
    target_family_slug,
)


class WorkflowTests(unittest.TestCase):
    def test_configuration_paths_are_per_configuration_and_target_family(self) -> None:
        root = Path("C:/repo")
        self.assertEqual(configuration_slug("RelWithDebInfo"), "relwithdebinfo")
        self.assertEqual(DEFAULT_TARGET_FAMILY, "examples")
        self.assertEqual(build_dir(root, "Debug", "core"), root / "build" / "conan" / "core" / "debug")
        self.assertEqual(build_dir(root, "Debug"), root / "build" / "conan" / "examples" / "debug")
        self.assertEqual(preset_name("vision-tests", "RelWithDebInfo"), "vision-tests-relwithdebinfo")

    def test_unknown_configuration_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            configuration_slug("Profile")
        with self.assertRaises(ValueError):
            target_family_slug("everything")

    def test_target_family_mapping_and_options(self) -> None:
        self.assertEqual(TARGET_FAMILIES, ("core", "examples", "tests", "vision", "vision-tests", "vision-oidn"))
        self.assertEqual(target_family_for_target("CoronaEngine"), "core")
        self.assertEqual(target_family_for_target("corona_engine"), "examples")
        self.assertEqual(target_family_for_target("corona_resource_tests"), "tests")
        self.assertEqual(target_family_for_target("vision-gui"), "vision")
        self.assertEqual(target_family_for_target("test-render_graph"), "vision-tests")
        self.assertEqual(target_family_for_target("CP_OIDN_CUDA"), "vision-oidn")
        self.assertEqual(target_family_for_targets(["corona_engine"]), "examples")
        with self.assertRaises(ValueError):
            target_family_for_targets(["corona_engine", "CoronaEngine"])
        self.assertIn("&:with_vision_tests=True", conan_options("vision-tests"))
        self.assertIn("&:with_oidn=True", conan_options("vision-oidn"))

    def test_cmake_bracket_handles_embedded_delimiter(self) -> None:
        self.assertEqual(_cmake_bracket("a]]b"), "[=[a]]b]=]")

    def test_environment_keys_are_merged_case_insensitively(self) -> None:
        environment = {"PATH": "old", "Path": "duplicate", "OTHER": "value"}
        _set_environment_value(environment, "Path", "with-msvc")
        self.assertEqual(_environment_value(environment, "PATH"), "with-msvc")
        self.assertEqual(sum(key.casefold() == "path" for key in environment), 1)

    def test_safe_remove_refuses_outside_repository(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "repo"
            root.mkdir()
            with self.assertRaises(RuntimeError):
                safe_remove(root, root.parent)

    def test_horizon_lock_requires_full_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            lock_file = Path(directory) / "horizon.lock.json"
            lock_file.write_text(json.dumps({
                "schema_version": 1,
                "url": "https://github.com/CoronaEngine/Horizon.git",
                "ref": "conan-migration",
                "commit": "a" * 40,
            }), encoding="utf-8")
            self.assertEqual(load_lock(lock_file).commit, "a" * 40)
            lock_file.write_text("{}", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                load_lock(lock_file)


if __name__ == "__main__":
    unittest.main()
