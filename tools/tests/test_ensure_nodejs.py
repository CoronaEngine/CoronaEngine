import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "build" / "ensure_nodejs.py"
spec = importlib.util.spec_from_file_location("ensure_nodejs", SCRIPT)
nodejs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nodejs)


class EnsureNodejsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="corona node test ")
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name).resolve()
        (self.prefix / "conda-meta").mkdir()
        prefix_patch = patch.object(nodejs.sys, "prefix", str(self.prefix))
        prefix_patch.start()
        self.addCleanup(prefix_patch.stop)

    def make_node(self):
        directory = self.prefix if nodejs.os.name == "nt" else self.prefix / "bin"
        directory.mkdir(exist_ok=True)
        node = directory / ("node.exe" if nodejs.os.name == "nt" else "node")
        npm = directory / ("npm.cmd" if nodejs.os.name == "nt" else "npm")
        cli = (
            directory / "node_modules/npm/bin/npm-cli.js"
            if nodejs.os.name == "nt"
            else self.prefix / "lib/node_modules/npm/bin/npm-cli.js"
        )
        cli.parent.mkdir(parents=True, exist_ok=True)
        for file in (node, npm, cli):
            file.touch()
        return directory, node, npm, cli

    @staticmethod
    def working_process(command, **kwargs):
        output = f"v{nodejs.NODE_VERSION}\n" if len(command) == 2 else "10.9.3\n"
        return subprocess.CompletedProcess(command, 0, output)

    def test_reuses_pinned_local_node_even_when_terminal_points_at_base(self):
        directory, node, _, cli = self.make_node()
        with patch.dict(nodejs.os.environ, {"CONDA_PREFIX": "unrelated-base"}), \
                patch.object(nodejs.subprocess, "run", side_effect=self.working_process) as run:
            self.assertEqual(nodejs.ensure_nodejs("unused-conda", self.prefix), directory)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[0], [str(node), "--version"])
        self.assertEqual(run.call_args_list[1].args[0], [str(node), str(cli), "--version"])
        self.assertTrue(run.call_args.kwargs["env"]["PATH"].startswith(str(directory)))

    def test_missing_node_does_not_probe_system_path(self):
        with patch.object(nodejs.subprocess, "run") as run:
            self.assertIsNone(nodejs.probe_node(self.prefix))
        run.assert_not_called()

    def test_installs_missing_or_wrong_version_in_explicit_prefix(self):
        for initial_version in (None, "v24.0.0"):
            with self.subTest(initial_version=initial_version):
                installed = False
                if initial_version:
                    self.make_node()

                def run_command(command, **kwargs):
                    nonlocal installed
                    if command[0] == "project-conda":
                        self.assertIn("--freeze-installed", command)
                        self.assertIn(f"nodejs={nodejs.NODE_VERSION}", command)
                        self.assertEqual(command[command.index("--prefix") + 1], str(self.prefix))
                        self.make_node()
                        installed = True
                        return subprocess.CompletedProcess(command, 0)
                    if not installed:
                        return subprocess.CompletedProcess(command, 0, initial_version)
                    return self.working_process(command, **kwargs)

                with patch.object(nodejs.subprocess, "run", side_effect=run_command):
                    self.assertIsNotNone(nodejs.ensure_nodejs("project-conda", self.prefix))
                self.assertTrue(installed)

    def test_repairs_missing_npm_when_conda_already_records_version(self):
        _, _, npm, _ = self.make_node()
        npm.unlink()
        (self.prefix / "conda-meta" / f"nodejs-{nodejs.NODE_VERSION}-test.json").write_text("{}")

        def run_command(command, **kwargs):
            if command[0] == "project-conda":
                self.assertIn("--force-reinstall", command)
                npm.touch()
                return subprocess.CompletedProcess(command, 0)
            return self.working_process(command, **kwargs)

        with patch.object(nodejs.subprocess, "run", side_effect=run_command):
            self.assertIsNotNone(nodejs.ensure_nodejs("project-conda", self.prefix))

    def test_broken_npm_is_not_considered_a_working_toolchain(self):
        self.make_node()
        with patch.object(nodejs.subprocess, "run", side_effect=[
            subprocess.CompletedProcess([], 0, f"v{nodejs.NODE_VERSION}\n"),
            subprocess.CalledProcessError(1, ["npm"]),
        ]):
            self.assertIsNone(nodejs.probe_node(self.prefix))

    def test_rejects_wrong_environment_before_installing(self):
        with patch.object(nodejs.sys, "prefix", str(self.prefix / "different-env")), \
                patch.object(nodejs.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "Python belongs to"):
                nodejs.ensure_nodejs("project-conda", self.prefix)
        run.assert_not_called()

    def test_rejects_non_conda_prefix(self):
        (self.prefix / "conda-meta").rmdir()
        with patch.object(nodejs.subprocess, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "Not a Conda environment"):
                nodejs.ensure_nodejs("project-conda", self.prefix)
        run.assert_not_called()

    def test_rechecks_after_install_and_rejects_incomplete_installation(self):
        with patch.object(nodejs.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(RuntimeError, "not usable"):
                nodejs.ensure_nodejs("project-conda", self.prefix)

    def test_install_failure_returns_error_without_success_json(self):
        output = io.StringIO()
        with patch.object(nodejs.subprocess, "run", side_effect=subprocess.CalledProcessError(9, ["conda"])), \
                contextlib.redirect_stdout(output):
            result = nodejs.main(["--conda", "project-conda", "--prefix", str(self.prefix)])
        self.assertEqual(result, 1)
        self.assertEqual(output.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
