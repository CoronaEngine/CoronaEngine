from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_python_service_loader_has_a_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "plugin_loader.py"
    compatibility = EDITOR_ROOT / "CoronaPlugin" / "compat" / "legacy_load_utils.py"
    main_source = (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from runtime.plugin_loader import reimport" in main_source


def test_editor_entrypoint_implementation_lives_under_runtime():
    bootstrap = EDITOR_ROOT / "runtime" / "bootstrap.py"
    main_source = (EDITOR_ROOT / "main.py").read_text(encoding="utf-8")

    assert bootstrap.is_file()
    bootstrap_source = bootstrap.read_text(encoding="utf-8")
    assert "register_core_python_script_services" in bootstrap_source
    assert "register_script_dispatcher" in bootstrap_source
    assert "from runtime.bootstrap import" in main_source
    assert "register_core_python_script_services" not in main_source
    assert "register_script_dispatcher" not in main_source


def test_python_service_loader_does_not_probe_legacy_registry_packages():
    source = (EDITOR_ROOT / "runtime" / "plugin_loader.py").read_text(encoding="utf-8")

    assert "backend.registry" not in source
    assert "Backend.registry" not in source


def test_python_service_loader_does_not_start_legacy_scene_datas_shell():
    source = (EDITOR_ROOT / "runtime" / "plugin_loader.py").read_text(encoding="utf-8")

    assert "register_active_python_script_services" in source
    assert "register_remaining_python_script_services" not in source


def test_plugin_base_has_a_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "plugin_base.py"
    compatibility = EDITOR_ROOT / "CoronaPlugin" / "compat" / "legacy_plugin_base.py"

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    for source_path in (EDITOR_ROOT / "plugins").rglob("main.py"):
        source = source_path.read_text(encoding="utf-8")
        if "PluginBase" not in source:
            continue
        assert "from CoronaPlugin.core.corona_plugin_base import" not in source
        assert "from runtime.plugin_base import PluginBase" in source
