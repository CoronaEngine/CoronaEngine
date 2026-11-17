"""Resolving a path never touches the disk.

`paths_config` used to `mkdir` the active project's ``media/`` and ``models/``
while resolving them, and `runtime.editor_host` calls `get_default_paths()` at
import time. Opening the editor therefore created two empty directories beside
the executable - or under the current working directory when no project was open
yet - even when nothing was ever written into them. In a read-only install
directory the same ``mkdir`` turned startup into a ``PermissionError``.

The replacement rule these guards pin: path helpers return paths and nothing
else; the writer that needs a directory creates it. The nested directories that
are genuinely produced at runtime (`documents`, `script_files`, `assets`,
`project`) already create their own parent before writing.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from config import paths_config  # noqa: E402


def test_resolving_the_default_paths_creates_nothing(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)

    config = paths_config.get_default_paths()

    assert config.autosave_dir.name == "media"
    assert config.assets_model_dir.name == "models"
    assert config.object_recognition_db == config.assets_model_dir / "database.db"
    assert not (tmp_path / "media").exists()
    assert not (tmp_path / "models").exists()


def test_every_project_path_helper_is_read_only(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)

    resolved = [
        paths_config.get_project_media_dir(),
        paths_config.get_project_models_dir(),
        paths_config.get_project_screenshots_dir(),
        paths_config.get_project_recognition_db(),
    ]

    assert [path.parent for path in resolved] == [
        tmp_path,
        tmp_path,
        tmp_path,
        tmp_path / "models",
    ]
    assert list(tmp_path.iterdir()) == []


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
