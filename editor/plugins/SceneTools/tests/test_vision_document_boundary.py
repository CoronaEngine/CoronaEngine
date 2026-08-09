import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")


def test_vision_document_parsing_has_a_dedicated_owner():
    document_source = PLUGIN_ROOT / "vision_document.py"
    assert document_source.is_file()
    source = document_source.read_text(encoding="utf-8")
    for symbol in (
        "vision_document_for_embedded_storage",
        "extract_vision_camera_pose",
        "infer_vision_render_mode",
        "iter_vision_shapes",
        "vision_shape_type",
        "resolve_vision_model_path",
        "shape_collection",
        "remove_shape_at_json_path",
        "shape_at_json_path",
        "compact_removed_shapes",
    ):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE


def test_vision_document_parsing_preserves_coordinate_and_resource_rules(tmp_path):
    module = importlib.import_module("plugins.SceneTools.vision_document")
    source_path = tmp_path / "scene.json"
    source_path.write_text("{}", encoding="utf-8")
    document = {
        "scene": {
            "shapes": [{"type": "model", "param": {"fn": "assets/a.obj"}}],
            "camera": {
                "param": {
                    "position": [1, 2, 3],
                    "forward": [0, 0, -1],
                    "fov": 1.0,
                }
            },
        }
    }

    embedded = module.vision_document_for_embedded_storage(document, str(source_path))
    assert embedded["scene"]["shapes"][0]["param"]["fn"].endswith("assets\\a.obj")
    camera = module.extract_vision_camera_pose(document)
    assert camera["position"] == [1.0, 2.0, -3.0]
    assert camera["fov"] == 1.0 * 180.0 / 3.141592653589793
    assert list(module.iter_vision_shapes(document))[0][1] == "/scene/shapes/0"
    assert module.resolve_vision_model_path(str(source_path), document["scene"]["shapes"][0]).endswith(
        "assets\\a.obj"
    )


def test_vision_document_shape_mutation_handles_list_paths():
    module = importlib.import_module("plugins.SceneTools.vision_document")
    document = {"scene": {"shapes": [{"name": "a"}, {"name": "b"}]}}

    assert module.shape_collection(document) == document["scene"]["shapes"]
    assert module.shape_at_json_path(document, "/scene/shapes/1")["name"] == "b"
    assert module.remove_shape_at_json_path(document, "/scene/shapes/0")
    module.compact_removed_shapes(document)
    assert document["scene"]["shapes"] == [{"name": "b"}]


def test_embedded_document_without_source_path_keeps_relative_resources():
    module = importlib.import_module("plugins.SceneTools.vision_document")
    document = {
        "scene": {
            "shapes": [{"type": "model", "param": {"fn": "assets/a.obj"}}]
        }
    }

    embedded = module.vision_document_for_embedded_storage(document, "")

    assert embedded["scene"]["shapes"][0]["param"]["fn"] == "assets/a.obj"
