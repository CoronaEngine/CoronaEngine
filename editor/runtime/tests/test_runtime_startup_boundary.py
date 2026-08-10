from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
RUNTIME_ROOT = EDITOR_ROOT / "runtime"


def test_editor_main_is_only_the_host_facing_startup_shim():
    main = (EDITOR_ROOT / "main.py").read_text(encoding="utf-8")

    assert "from runtime.bootstrap import editor, run" in main
    assert "register_core_python_script_services" not in main
    assert "register_script_dispatcher" not in main
    assert "reimport()" not in main


def test_runtime_bootstrap_is_the_single_registration_owner():
    bootstrap = (RUNTIME_ROOT / "bootstrap.py").read_text(encoding="utf-8")

    assert "register_core_python_script_services()" in bootstrap
    assert "editor.register_script_dispatcher()" in bootstrap
    assert "from runtime.plugin_loader import reimport" in bootstrap
    assert "reimport()" in bootstrap


def test_startup_guards_prevent_duplicate_registration_and_runtime_init():
    registry = (RUNTIME_ROOT / "registry.py").read_text(encoding="utf-8")
    host = (RUNTIME_ROOT / "editor_host.py").read_text(encoding="utf-8")

    assert "_registered_service_names" in registry
    assert "if service_name in _registered_service_names" in registry
    assert "if cls._runtime_initialized" in host
    assert "if not cls._runtime_initialized:" in host


def test_startup_boundary_is_documented():
    boundary = (RUNTIME_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")

    for marker in (
        "editor/main.py",
        "runtime/bootstrap.py",
        "一次性注册",
        "幂等",
        "重复初始化",
        "关闭",
    ):
        assert marker in boundary
