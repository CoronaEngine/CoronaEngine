from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_python_service_registry_has_a_runtime_canonical_owner():
    runtime_registry = EDITOR_ROOT / "runtime" / "registry.py"
    backend_registry = EDITOR_ROOT / "backend" / "registry.py"

    assert runtime_registry.is_file()
    assert "from runtime.registry import" in (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(
        encoding="utf-8"
    )
    assert "runtime.registry" in (EDITOR_ROOT / "runtime" / "plugin_loader.py").read_text(
        encoding="utf-8"
    )
    assert "Compatibility" in backend_registry.read_text(encoding="utf-8")


def test_backend_package_is_not_a_python_service_registry_owner():
    source = (EDITOR_ROOT / "backend" / "__init__.py").read_text(encoding="utf-8")

    assert "compatibility" in source.lower()
    assert "generated editor script paths" in source.lower()
    assert "services registered" not in source.lower()


def test_runtime_registry_keeps_generated_blockly_service_in_compatibility_domain():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    assert '_BLOCKLY_PACKAGE = "script_runtime"' in source
    assert '"ScratchTool": (f"{_BLOCKLY_PACKAGE}.blockly.main", "ScratchTool")' in source
    assert '"runtime.blockly.main"' not in source


def test_script_runtime_owns_generated_blockly_output_path():
    paths_source = (EDITOR_ROOT / "config" / "paths_config.py").read_text(encoding="utf-8")
    registry_source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    assert "generated_script_dir" in paths_source
    assert "backend/script" in registry_source
    assert "generated output owner" in registry_source.lower()


def test_legacy_python_services_are_explicit_registration_only():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    legacy_block = source[
        source.index("LEGACY_PYTHON_SCRIPT_SERVICES = {") : source.index(
            "LAZY_PYTHON_SCRIPT_SERVICES", source.index("LEGACY_PYTHON_SCRIPT_SERVICES = {")
        )
    ]
    assert 'LEGACY_PYTHON_SCRIPT_SERVICES = {' in legacy_block
    for service_name in (
        "SceneDatas",
        "ProjectLauncher",
        "FileManager",
        "ProjectSettings",
        "MainView",
        "SceneTools",
    ):
        assert f'"{service_name}"' in legacy_block
    assert "def register_active_python_script_services" in source
    assert "def register_legacy_python_script_services" in source


def test_project_archive_service_is_lazy_but_not_legacy_only():
    source = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    assert 'CORE_PYTHON_SCRIPT_SERVICES = set()' in source
    lazy_block = source[
        source.index("LAZY_PYTHON_SCRIPT_SERVICES") : source.index("_managed_services")
    ]
    assert '"ProjectArchive"' in lazy_block
