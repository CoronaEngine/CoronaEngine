import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
import check_pip_modules as checker


class PythonRequirementsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.requirements = Path(temporary.name) / "requirements.txt"
        self.requirements.write_text("torch==2.14.1+cpu\n", encoding="utf-8")

    def run_check(self, *options):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            code = checker.main(["-r", str(self.requirements), "--no-unicode", *options])
        return code, output.getvalue()

    def test_missing_dependency_fails_by_default(self):
        with patch.object(checker, "get_installed_version", return_value=None):
            self.assertEqual(self.run_check()[0], 1)

    def test_cuda_wheel_does_not_satisfy_cpu_requirement(self):
        with patch.object(checker, "get_installed_version", return_value="2.14.1+cu128"):
            self.assertEqual(self.run_check()[0], 1)

    def test_cpu_pin_is_enforced_without_packaging(self):
        with patch.object(checker, "SpecifierSet", None), patch.object(checker, "Version", None):
            self.assertTrue(checker.version_satisfies("2.14.1+cpu", "==2.14.1+cpu"))
            self.assertFalse(checker.version_satisfies("2.14.1+cu128", "==2.14.1+cpu"))

    def test_existing_cpu_wheel_needs_no_installation(self):
        with patch.object(checker, "get_installed_version", return_value="2.14.1+cpu"), \
                patch.object(checker, "auto_install") as install:
            self.assertEqual(self.run_check("--auto-install")[0], 0)
        install.assert_not_called()

    def test_failed_pip_still_fails_if_package_metadata_appears(self):
        with patch.object(checker, "get_installed_version", side_effect=[None, "2.14.1+cpu"]), \
                patch.object(checker, "auto_install", return_value=1):
            self.assertEqual(self.run_check("--auto-install")[0], 1)

    def test_successful_pip_with_missing_dependency_still_fails(self):
        with patch.object(checker, "get_installed_version", return_value=None), \
                patch.object(checker, "auto_install", return_value=0):
            self.assertEqual(self.run_check("--auto-install")[0], 1)

    def test_install_rechecks_all_packages_and_reports_final_json(self):
        self.requirements.write_text("torch==2.14.1+cpu\nnumpy\n", encoding="utf-8")
        versions = {}

        def install_file(path):
            self.assertEqual(path, str(self.requirements))
            versions.update(torch="2.14.1+cpu", numpy="2.5.4")
            return 0

        with patch.object(checker, "get_installed_version", side_effect=versions.get), \
                patch.object(checker, "auto_install", side_effect=install_file) as install:
            code, output = self.run_check("--auto-install", "--json")
        self.assertEqual(code, 0)
        install.assert_called_once()
        report = json.loads(output)
        self.assertEqual(report["ok"], 2)
        self.assertEqual(report["missing"], [])
        self.assertTrue(all(row["status"] == "ok" for row in report["details"]))

    def test_pip_receives_original_file_with_cpu_index(self):
        content = ("--extra-index-url https://download.pytorch.org/whl/cpu\n"
                   "torch==2.14.1+cpu\n")
        self.requirements.write_text(content, encoding="utf-8")
        self.assertEqual(len(checker.parse_requirements(str(self.requirements))), 1)
        with patch.object(checker.subprocess, "call", return_value=0) as pip, \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(checker.auto_install(str(self.requirements)), 0)
        self.assertEqual(pip.call_args.args[0],
                         [sys.executable, "-m", "pip", "install", "-r", str(self.requirements)])
        self.assertEqual(self.requirements.read_text(encoding="utf-8"), content)

    def test_unrecognized_requirement_cannot_silently_pass(self):
        self.requirements.write_text("-r omitted-requirements.txt\n", encoding="utf-8")
        self.assertEqual(self.run_check()[0], 2)


if __name__ == "__main__":
    unittest.main()
