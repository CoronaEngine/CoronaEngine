import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")


def test_vision_storage_has_a_dedicated_owner():
    storage_source = PLUGIN_ROOT / "vision_storage.py"
    assert storage_source.is_file()
    source = storage_source.read_text(encoding="utf-8")
    for symbol in (
        "derived_vision_scene_path",
        "runtime_vision_scene_path",
        "atomic_write_json",
    ):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE


def test_vision_storage_generates_stable_paths_and_atomic_json(tmp_path):
    storage = importlib.import_module("plugins.SceneTools.vision_storage")

    class Scene:
        name = "Preview"
        route = str(tmp_path / "Project" / "Scene" / "main.scene")

    source = tmp_path / "vision.json"
    source.write_text("{}", encoding="utf-8")
    derived = storage.derived_vision_scene_path(str(source), Scene())
    runtime = storage.runtime_vision_scene_path(Scene(), str(tmp_path))
    assert derived == storage.derived_vision_scene_path(str(source), Scene())
    assert derived.endswith(".json")
    assert runtime.startswith(str(tmp_path))

    output = tmp_path / ".corona" / "vision_live" / "scene.json"
    storage.atomic_write_json(str(output), {"ok": True})
    assert output.read_text(encoding="utf-8").strip().startswith("{")


def test_runtime_vision_scene_path_uses_canonical_project_context(monkeypatch, tmp_path):
    storage = importlib.import_module("plugins.SceneTools.vision_storage")
    project_root = tmp_path / "ActiveProject"
    project_root.mkdir()

    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    class Scene:
        name = "Preview"
        route = "Scene/main.scene"

    runtime = Path(storage.runtime_vision_scene_path(Scene()))
    assert runtime.parent == project_root / ".corona" / "vision_live"
