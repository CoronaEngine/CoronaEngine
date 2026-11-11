"""Editor settings have exactly one owner, and it is reachable two ways only.

The ownership rule this pins:

* C++ decides where `CoronaEditor.ini` lives (`editor_ini_path()`), because the editor's
  executable directory is a C++ fact.
* Python edits that file through `ProjectArchive.editor_settings`, taking the path as an
  argument - it never derives one itself.
* No other module may claim to own the recent-project list or the version.

The module that used to be a second owner (`config/project_state.py`) is gone; this guard
keeps a third from appearing.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
PY = ROOT / "embedded_python"
CPP = ROOT / "src/systems/ui/cef/editor_scene_service.cpp"


def test_python_never_derives_the_ini_path():
    offenders = []
    for path in PY.rglob("*.py"):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        if "CoronaEditor.ini" in text:
            offenders.append(path.relative_to(ROOT).as_posix())
    assert not offenders, (
        "Python must receive the ini path from C++ instead of rebuilding it; offenders: "
        + ", ".join(offenders))


def test_the_cpp_side_names_the_ini_in_one_place():
    text = CPP.read_text(encoding="utf-8-sig")
    assert text.count("CoronaEditor.ini") == 1, (
        "the ini path must be derived once (editor_ini_path) and passed to Python")


def test_python_edits_settings_through_the_archive_service():
    handler = PY / "runtime/archive/editor_settings.py"
    assert handler.exists(), "the ProjectArchive settings handler is the single editor"
    service = PY / "plugins/ProjectArchive/main.py"
    assert "editor_settings" in service.read_text(encoding="utf-8-sig"), (
        "the settings handler has to be reachable through the ProjectArchive service")


def test_the_second_owner_stays_deleted():
    assert not (PY / "config/project_state.py").exists(), (
        "config/project_state.py claimed to own the ini without being imported by anything")
