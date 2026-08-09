from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
PLUGINS_ROOT = EDITOR_ROOT / "plugins"

PLUGIN_DIRECTORIES = (
    "AITool",
    "FileManager",
    "MainView",
    "ProjectArchive",
    "ProjectLauncher",
    "ProjectSettings",
    "SceneTools",
)


def test_plugins_have_a_local_boundary_inventory():
    boundary = PLUGINS_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "业务插件",
        "native aggregate",
        "Quasar",
        "runtime",
        "删除条件",
    ):
        assert marker in source
    for directory in PLUGIN_DIRECTORIES:
        assert f"`{directory}/`" in source


def test_plugins_keep_registration_and_ownership_in_the_runtime_layer():
    boundary = (PLUGINS_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    registry = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    assert "runtime/registry.py" in boundary
    assert "PYTHON_SCRIPT_SERVICES" in registry
    assert '"SceneTools": ("plugins.SceneTools.main", "SceneTools")' in registry
    assert "plugins.AITool.main" in registry


def test_legacy_service_implementations_live_under_compat_owners():
    registry = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    expected = {
    }

    for service_name, module_path in expected.items():
        assert f'"{service_name}": ("{module_path}", "{service_name}")' in registry
        package = service_name
        facade = module_path.rsplit(".", 1)[-1] + ".py"
        assert (PLUGINS_ROOT / package / "compat" / facade).is_file()
        shim = (PLUGINS_ROOT / package / "main.py").read_text(encoding="utf-8")
        assert f"from {module_path} import {service_name}" in shim
        assert f"class {service_name}" not in shim

    project_launcher = (PLUGINS_ROOT / "ProjectLauncher" / "main.py").read_text(
        encoding="utf-8"
    )
    assert "class ProjectLauncher" in project_launcher
    assert not (
        PLUGINS_ROOT / "ProjectLauncher" / "compat" / "legacy_project_launcher.py"
    ).exists()

    project_settings = (PLUGINS_ROOT / "ProjectSettings" / "main.py").read_text(
        encoding="utf-8"
    )
    assert "class ProjectSettings" in project_settings
    assert not (
        PLUGINS_ROOT / "ProjectSettings" / "compat" / "legacy_project_settings.py"
    ).exists()

    file_manager = (PLUGINS_ROOT / "FileManager" / "main.py").read_text(
        encoding="utf-8"
    )
    assert "class FileManager" in file_manager
    assert not (
        PLUGINS_ROOT / "FileManager" / "compat" / "legacy_file_manager.py"
    ).exists()

    main_view = (PLUGINS_ROOT / "MainView" / "main.py").read_text(encoding="utf-8")
    assert "class MainView" in main_view
    assert not (PLUGINS_ROOT / "MainView" / "compat" / "legacy_main_view.py").exists()


def test_active_plugins_have_local_boundary_documents():
    for directory in ("AITool", "FileManager", "ProjectLauncher", "ProjectSettings"):
        boundary = PLUGINS_ROOT / directory / "BOUNDARY.md"
        assert boundary.is_file(), directory
        source = boundary.read_text(encoding="utf-8")
        assert "canonical owner" in source, directory
        assert "不负责" in source, directory
        assert "删除条件" in source, directory


def test_quasar_is_explicitly_outside_the_repository_migration_scope():
    boundary = (PLUGINS_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    quasar = PLUGINS_ROOT / "AITool" / "Quasar"

    assert quasar.is_dir()
    assert "不修改" in boundary
    assert "外部子模块" in boundary


def test_unreferenced_scene_tools_helper_is_removed_after_native_migration():
    boundary = (PLUGINS_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    helper = PLUGINS_ROOT / "SceneTools" / "compat" / "legacy_vision_import_helper.py"
    canonical_import = PLUGINS_ROOT / "SceneTools" / "vision_import.py"

    assert not helper.exists()
    assert canonical_import.is_file()
    assert "canonical" in boundary
    assert "removed compatibility code" in boundary
