"""The Python runtime deploy is incremental: an unchanged tree costs stats, not a recopy."""

import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "build_tools" / "scripts" / "deploy_python_runtime.py"


def run(source: Path, destination: Path, excludes=()) -> subprocess.CompletedProcess:
    command = [sys.executable, str(SCRIPT),
               "--source-lib", str(source / "Lib"),
               "--source-dlls", str(source / "DLLs"),
               "--destination", str(destination)]
    for name in excludes:
        command += ["--exclude", name]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
    assert result.returncode == 0, result.stderr
    return result


def make_source(root: Path) -> Path:
    (root / "Lib" / "site-packages" / "pkg").mkdir(parents=True)
    (root / "Lib" / "json").mkdir(parents=True)
    (root / "DLLs").mkdir(parents=True)
    (root / "Lib" / "json" / "__init__.py").write_text("VALUE = 1\n", encoding="utf-8")
    (root / "Lib" / "site-packages" / "pkg" / "mod.py").write_text("X = 1\n", encoding="utf-8")
    (root / "DLLs" / "python313.dll").write_bytes(b"MZ")
    return root


def test_first_deploy_copies_everything(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    result = run(source, destination)
    assert "deployed 3 changed file(s)" in result.stdout, result.stdout
    assert (destination / "Lib" / "json" / "__init__.py").read_text(encoding="utf-8") == "VALUE = 1\n"
    assert (destination / "DLLs" / "python313.dll").is_file()
    manifest = json.loads((destination / ".deploy-manifest.json").read_text(encoding="utf-8"))
    assert set(manifest) == {"Lib/json/__init__.py", "Lib/site-packages/pkg/mod.py",
                             "DLLs/python313.dll"}


def test_second_deploy_is_a_stat_only_no_op(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    run(source, destination)
    result = run(source, destination)
    assert "up to date (3 files)" in result.stdout, result.stdout
    assert "deployed" not in result.stdout


def test_only_the_changed_file_is_copied(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    run(source, destination)
    untouched = destination / "Lib" / "site-packages" / "pkg" / "mod.py"
    before = untouched.stat().st_mtime_ns
    (source / "Lib" / "json" / "__init__.py").write_text("VALUE = 2\n", encoding="utf-8")
    result = run(source, destination)
    assert "deployed 1 changed file(s)" in result.stdout, result.stdout
    assert (destination / "Lib" / "json" / "__init__.py").read_text(encoding="utf-8") == "VALUE = 2\n"
    assert untouched.stat().st_mtime_ns == before


def test_a_file_removed_from_the_source_leaves_the_deployment(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    run(source, destination)
    (source / "Lib" / "site-packages" / "pkg" / "mod.py").unlink()
    result = run(source, destination)
    assert "removed 1 stale file(s)" in result.stdout, result.stdout
    assert not (destination / "Lib" / "site-packages" / "pkg" / "mod.py").exists()


def test_excluded_distributions_are_never_copied(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    (source / "Lib" / "site-packages" / "torch").mkdir()
    (source / "Lib" / "site-packages" / "torch" / "init.py").write_text("", encoding="utf-8")
    (source / "Lib" / "site-packages" / "torch-2.6.0.dist-info").mkdir()
    (source / "Lib" / "site-packages" / "torch-2.6.0.dist-info" / "METADATA").write_text("", encoding="utf-8")
    result = run(source, destination, excludes=("torch", "PySide6"))
    assert "deployed 3 changed file(s)" in result.stdout, result.stdout
    assert not (destination / "Lib" / "site-packages" / "torch").exists()
    assert not (destination / "Lib" / "site-packages" / "torch-2.6.0.dist-info").exists()
    manifest = json.loads((destination / ".deploy-manifest.json").read_text(encoding="utf-8"))
    assert not any("torch" in key for key in manifest)


def test_a_distribution_excluded_later_disappears_from_the_deployment(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    (source / "Lib" / "site-packages" / "pandas").mkdir()
    (source / "Lib" / "site-packages" / "pandas" / "init.py").write_text("", encoding="utf-8")
    run(source, destination)
    assert (destination / "Lib" / "site-packages" / "pandas" / "init.py").is_file()
    result = run(source, destination, excludes=("pandas",))
    assert "removed 1 stale file(s)" in result.stdout, result.stdout
    assert not (destination / "Lib" / "site-packages" / "pandas" / "init.py").exists()


def test_a_removed_package_leaves_no_empty_directories(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    run(source, destination)
    import shutil
    shutil.rmtree(source / "Lib" / "site-packages" / "pkg")
    run(source, destination)
    assert not (destination / "Lib" / "site-packages" / "pkg").exists()


def test_a_deleted_deployment_file_is_restored(tmp_path):
    source, destination = make_source(tmp_path / "env"), tmp_path / "runtime" / "python-runtime"
    run(source, destination)
    (destination / "DLLs" / "python313.dll").unlink()
    result = run(source, destination)
    assert "deployed 1 changed file(s)" in result.stdout, result.stdout
    assert (destination / "DLLs" / "python313.dll").is_file()
