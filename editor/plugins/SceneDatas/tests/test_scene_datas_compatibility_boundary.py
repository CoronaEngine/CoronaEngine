import ast
from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = PLUGIN_ROOT / "main.py"
COMPAT_SOURCE_PATH = PLUGIN_ROOT / "compat" / "legacy_scene_datas_plugin.py"


def test_scene_datas_has_a_local_compatibility_inventory():
    inventory = PLUGIN_ROOT / "COMPATIBILITY.md"

    assert inventory.is_file()
    source = inventory.read_text(encoding="utf-8")
    assert "canonical owner" in source
    assert "scene.get_snapshot" in source
    assert "sceneTools" in source
    assert "删除条件" in source


def test_scene_datas_is_only_a_historical_registration_shell():
    source = SOURCE_PATH.read_text(encoding="utf-8")
    compat_source = COMPAT_SOURCE_PATH.read_text(encoding="utf-8")
    tree = ast.parse(compat_source, filename=str(COMPAT_SOURCE_PATH))
    classes = [node for node in tree.body if isinstance(node, ast.ClassDef)]

    assert "from plugins.SceneDatas.compat.legacy_scene_datas_plugin import" in source
    assert "register_web(\"SceneDatas\")" in compat_source
    assert len(classes) == 1
    class_body = classes[0].body
    assert all(
        isinstance(node, ast.Expr)
        and isinstance(getattr(node, "value", None), ast.Constant)
        and isinstance(node.value.value, str)
        for node in class_body
    )
    assert "scene.get_snapshot" not in compat_source
    assert "sceneTools" not in compat_source
