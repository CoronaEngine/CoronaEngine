from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
RUNTIME_ROOT = EDITOR_ROOT / "runtime"


def _runtime_sources():
    return (
        path
        for path in RUNTIME_ROOT.rglob("*.py")
        if "tests" not in path.parts
    )


def test_runtime_has_a_local_boundary_inventory():
    boundary = RUNTIME_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "编辑器 Python host",
        "插件注册",
        "公共 host support",
        "legacy adapter",
        "script_runtime",
        "删除条件",
    ):
        assert marker in source
    for path in ("registry.py", "editor_host.py", "archive/", "legacy/"):
        assert path in source


def test_runtime_registry_remains_the_plugin_lifecycle_owner():
    source = (RUNTIME_ROOT / "registry.py").read_text(encoding="utf-8")

    assert "PYTHON_SCRIPT_SERVICES" in source
    assert "_BLOCKLY_PACKAGE = \"script_runtime\"" in source
    assert "plugins.AITool.main" in source
    assert "register" in source


def test_runtime_legacy_objects_are_not_new_editor_api_owners():
    boundary = (RUNTIME_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    legacy_root = RUNTIME_ROOT / "legacy"

    assert legacy_root.is_dir()
    assert "compatibility" in boundary
    assert "不得新增" in boundary
    assert (legacy_root / "entities" / "actor.py").is_file()
    assert (legacy_root / "entities" / "scene.py").is_file()


def test_runtime_does_not_import_legacy_backend_as_a_new_owner():
    offenders = []
    for path in _runtime_sources():
        source = path.read_text(encoding="utf-8")
        if "from backend" in source or "import backend" in source:
            offenders.append(path)
    assert offenders == []
