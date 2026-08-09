import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")


def test_vision_binding_matching_has_a_dedicated_owner():
    source_path = PLUGIN_ROOT / "vision_bindings.py"
    assert source_path.is_file()
    source = source_path.read_text(encoding="utf-8")
    for symbol in (
        "vision_shape_identity_key",
        "binding_is_compatible",
        "find_previous_binding",
        "vision_import_summary",
    ):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE


def test_vision_binding_matching_preserves_guid_identity_priority():
    bindings = importlib.import_module("plugins.SceneTools.vision_bindings")
    shape = {"guid": "actor-guid", "type": "model", "param": {"fn": "a.obj"}}
    existing = {"shape_guid": "actor-guid", "json_path": "/old", "actor_guid": "actor"}
    used = set()

    result = bindings.find_previous_binding(
        [existing], used, shape, "model", "/scene/shapes/0", "C:/scene.json", "C:/a.obj"
    )
    assert result is existing
    assert used == {0}

    summary = bindings.vision_import_summary(
        "embedded", [existing], [{"type": "quad", "reason": "unsupported_shape_type"}]
    )
    assert summary["embedded"] is True
    assert summary["unsupported_by_reason"] == {"unsupported_shape_type": 1}
