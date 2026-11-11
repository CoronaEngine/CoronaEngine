"""Project directories are allocated by the archive service, not by C++.

The editor used to pick creative_world_1, _2, ... by looping over the filesystem before asking
Python to create the project - while Python creates its own directories anyway. This pins the
behaviour that replaced it: sequential names, occupied slots skipped, and the display name
following the directory index so the two cannot disagree.
"""
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "embedded_python"))

from runtime.archive.project import create_project_in  # noqa: E402


def _request(base: Path, stem: str = "creative_world") -> dict:
    return {"base_dir": str(base), "dir_stem": stem, "display_base": "创造世界",
            "world": {"type": "creative", "prompt": "test"}}


def test_directories_are_sequential_and_the_name_follows(tmp_path):
    first = create_project_in(_request(tmp_path))
    second = create_project_in(_request(tmp_path))

    assert Path(first["path"]).name == "creative_world_1"
    assert Path(second["path"]).name == "creative_world_2"
    assert first["name"] == "创造世界_1"
    assert second["name"] == "创造世界_2"


def test_the_project_is_actually_written(tmp_path):
    created = create_project_in(_request(tmp_path))
    assert (Path(created["path"]) / "scene.ini").is_file()
    assert Path(created["project_path"]) == Path(created["path"])
    assert Path(created["base_dir"]) == tmp_path


def test_an_occupied_slot_is_skipped(tmp_path):
    (tmp_path / "creative_world_1").mkdir()
    (tmp_path / "creative_world_2").mkdir()

    created = create_project_in(_request(tmp_path))
    assert Path(created["path"]).name == "creative_world_3"


def test_cpp_no_longer_allocates_the_directory():
    service = (ROOT / "src/systems/ui/cef/editor_scene_service.cpp").read_text(encoding="utf-8-sig")
    start = service.index("NativeResult create_launcher_project(")
    end = service.index("\n}\n", start)
    body = service[start:end]
    assert "create_directories" not in body, "C++ must not create the project directory"
    assert "std::filesystem::exists" not in body, "C++ must not probe for a free name"
    assert "create_project_in" in body, "creation goes through the archive service allocator"
