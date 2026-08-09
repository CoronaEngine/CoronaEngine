from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
RUNTIME_ROOT = EDITOR_ROOT / "runtime"


def test_runtime_has_a_local_boundary_inventory():
    boundary = (RUNTIME_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    assert "编辑器 Python host" in boundary
    assert "插件注册" in boundary
    assert (RUNTIME_ROOT / "registry.py").is_file()
    assert (RUNTIME_ROOT / "editor_host.py").is_file()


def test_runtime_registry_remains_the_plugin_lifecycle_owner():
    source = (RUNTIME_ROOT / "registry.py").read_text(encoding="utf-8")
    assert "PYTHON_SCRIPT_SERVICES" in source
    assert '"plugins.AITool.main"' in source
    assert "register" in source


def test_runtime_does_not_import_removed_packages():
    for path in RUNTIME_ROOT.rglob("*.py"):
        if "tests" in path.parts:
            continue
        source = path.read_text(encoding="utf-8-sig")
        assert "from backend" not in source
        assert "CoronaCore" not in source
