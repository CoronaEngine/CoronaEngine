import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")


def test_vision_geometry_has_no_scene_tools_implementation_owner():
    source_path = PLUGIN_ROOT / "vision_geometry.py"
    assert source_path.is_file()
    source = source_path.read_text(encoding="utf-8")
    for symbol in (
        "flatten_matrix4x4",
        "matrix4x4_to_corona_trs",
        "vision_primitive_vertices",
        "vision_primitive_world_vertices",
        "extract_vision_shape_transform",
    ):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE


def test_vision_geometry_preserves_handedness_and_primitive_shapes():
    geometry = importlib.import_module("plugins.SceneTools.vision_geometry")

    matrix = geometry.flatten_matrix4x4(
        [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1]
    )
    assert geometry.apply_vision_matrix_to_corona(matrix, [1, 2, 3]) == [3.0, 5.0, -7.0]
    assert geometry.extract_vision_shape_transform({"param": {"transform": {
        "type": "trs", "param": {"t": [1, 2, 3], "s": [2, 3, 4]}
    }}}) == {
        "position": [1.0, 2.0, -3.0],
        "rotation": [0.0, 0.0, 0.0],
        "scale": [2.0, 3.0, 4.0],
    }
    vertices, faces = geometry.vision_primitive_vertices(
        {"param": {"width": 2, "height": 4}}, "quad"
    )
    assert len(vertices) == 4
    assert faces == [[1, 2, 3], [3, 2, 4]]
