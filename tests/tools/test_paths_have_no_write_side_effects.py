"""Resolving a path never touches the disk, and the field set stays minimal.

`paths_config` used to `mkdir` the active project's ``media/`` and ``models/``
while resolving them, and `runtime.editor_host` calls `get_default_paths()` at
import time. Opening the editor therefore created two empty directories beside
the executable - or under the current working directory when no project was open
yet - even when nothing was ever written into them. In a read-only install
directory the same ``mkdir`` turned startup into a ``PermissionError``.

The replacement rule these guards pin: path helpers return paths and nothing
else, and the model carries only the paths the host actually reads. The
project-scoped helpers and the ``frontend_dist`` copy had no reader anywhere in
the tree: CEF resolves ``Frontend/dist/index.html`` from
``Utils::editor_directory()`` itself, so Python keeps no second copy of it.
"""
from pathlib import Path
import dataclasses
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from config import paths_config  # noqa: E402


def test_resolving_the_default_paths_creates_nothing(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)

    config = paths_config.get_default_paths()

    assert config.repo_root == (ROOT / "embedded_python").resolve()
    assert list(tmp_path.iterdir()) == []


def test_the_model_only_holds_the_path_the_host_reads():
    """`bootstrap` reads repo_root to extend sys.path; that is the whole surface."""
    fields = {field.name for field in dataclasses.fields(paths_config.PathsConfig)}

    assert fields == {"repo_root"}


def test_the_unused_project_path_helpers_stay_deleted():
    """Re-adding one restores a second owner for "where does this project live"."""
    retired = (
        "get_project_media_dir",
        "get_project_models_dir",
        "get_project_screenshots_dir",
        "get_project_recognition_db",
        "_get_active_project_path",
    )

    assert [name for name in retired if hasattr(paths_config, name)] == []


def test_the_host_does_not_publish_a_frontend_url():
    """The frontend is CEF's to locate; a Python copy of that path had no reader."""
    from runtime.editor_host import CoronaEditor

    assert not hasattr(CoronaEditor, "url")


def test_the_editor_startup_import_creates_nothing(tmp_path):
    """`import runtime.editor_host` is where the old startup write happened."""
    environment = dict(
        os.environ,
        PYTHONPATH=str(ROOT / "embedded_python"),
        PYTHONDONTWRITEBYTECODE="1",
    )
    environment.pop("CORONAENGINE_PROJECT", None)

    # bootstrap resolves the same paths through get_app_config(), so both
    # startup callers have to stay read-only.
    script = (
        "import runtime.editor_host\n"
        "from config.app_config import get_app_config\n"
        "get_app_config()\n"
    )
    completed = subprocess.run(
        [sys.executable, "-c", script],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert list(tmp_path.iterdir()) == []


def test_paths_config_does_not_create_directories():
    """The rule is local to the module: no `mkdir` may come back here."""
    source = (ROOT / "embedded_python/config/paths_config.py").read_text(encoding="utf-8-sig")

    assert ".mkdir(" not in source
    assert "makedirs" not in source
