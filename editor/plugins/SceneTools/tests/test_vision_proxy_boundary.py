import importlib
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
MAIN_SOURCE = (PLUGIN_ROOT / "compat" / "legacy_scene_tools.py").read_text(encoding="utf-8")


def test_vision_proxy_implementation_has_a_dedicated_owner():
    source_path = PLUGIN_ROOT / "vision_proxy.py"
    assert source_path.is_file()
    source = source_path.read_text(encoding="utf-8")
    for symbol in ("write_vision_primitive_proxy", "vision_model_native_local_correction"):
        assert f"def {symbol}" in source
        assert f"def _{symbol}" not in MAIN_SOURCE


def test_vision_proxy_writes_obj_and_reports_model_correction(tmp_path):
    proxy = importlib.import_module("plugins.SceneTools.vision_proxy")
    shape = {"name": "Floor", "param": {"width": 2, "height": 4}}

    relative, absolute = proxy.write_vision_primitive_proxy(
        str(tmp_path), shape, "quad", "/scene/shapes/0", "shape-guid"
    )
    assert relative == "Resource/vision_proxies/Floor_b91fa49f80fa.obj"
    obj = Path(absolute)
    assert obj.is_file()
    assert "# Corona external_live proxy" in obj.read_text(encoding="utf-8")

    model = tmp_path / "mesh.obj"
    model.write_text("v 0 0 0\nv 2 4 6\n", encoding="utf-8")
    correction = proxy.vision_model_native_local_correction(str(model))
    assert correction["native_local_correction_offset"] == [1.0, 2.0, -3.0]
    assert correction["native_local_correction_scale"] == 6.0
