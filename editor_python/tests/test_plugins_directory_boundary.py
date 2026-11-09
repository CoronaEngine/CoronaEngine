from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
PLUGINS_ROOT = EDITOR_ROOT / "plugins"

def test_only_archive_plugin_and_native_project_template_are_required():
    assert (PLUGINS_ROOT / "ProjectArchive/main.py").is_file()
    assert (PLUGINS_ROOT / "ProjectLauncher/templates/actor/demo.actor").is_file()
    assert not any((PLUGINS_ROOT / name / "main.py").exists() for name in
                   ("ProjectLauncher", "ProjectSettings", "SceneTools", "FileManager", "MainView"))


def test_quasar_and_its_bridge_are_retired():
    assert not (PLUGINS_ROOT / "AITool" / "Quasar").exists()
    assert not any((PLUGINS_ROOT / "AITool" / "cai_extensions").rglob("*.py"))


def test_unreferenced_scene_tools_helper_is_removed_after_native_migration():
    helper = PLUGINS_ROOT / "SceneTools" / "compat" / "legacy_vision_import_helper.py"
    canonical_import = PLUGINS_ROOT / "SceneTools" / "vision_import.py"

    assert not helper.exists()
    assert not canonical_import.exists()
