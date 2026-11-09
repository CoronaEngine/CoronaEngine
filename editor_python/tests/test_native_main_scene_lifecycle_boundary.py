from pathlib import Path
import re

EDITOR = Path(__file__).resolve().parents[1]
ENGINE = EDITOR.parent


def test_manifest_and_handlers_expose_no_multiscene_or_migration_operations():
    manifest = (ENGINE / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")
    handlers = (ENGINE / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
    for method in ("create_scene", "remove_scene", "list_routes", "switch", "migrate_legacy_scene",
                   "copy_existing_to_data", "choose_portable_scene_target", "browse_scene_file"):
        assert not re.search(r"EDITOR_API_METHOD[^\n]*, " + method + r",", manifest)
        assert f'{{"{method}",' not in handlers
    assert "importVisionScene" not in manifest
    assert "loadVisionScene" not in manifest
    assert '"scene.getSnapshot", "scene.get_snapshot"' in manifest
    assert "migrate_project_sidecar_scene_to_embedded" not in handlers


def test_native_init_is_singular_and_save_does_not_reload():
    source = (ENGINE / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
    init = source.split("nlohmann::json make_native_main_init_payload", 1)[1].split("\n}", 1)[0]
    assert '"scene"' in init
    assert '"scenes"' not in init
    save = source.split('{"scene_save",', 1)[1].split("void register_project_settings", 1)[0]
    assert "resolve_native_editor_scene_request" in save
    assert "reload_native_editor_scene" not in save
    assert "save_editor_scene(*scene, false, metadata)" in save
    assert "persist_native_scene_" not in source


def test_adapters_and_ui_have_no_old_scene_lifecycle():
    paths = ["api/editor_api.py",
             "Frontend/src/api/editorApi.js", "Frontend/src/services/sceneService.js",
             "Frontend/src/services/projectLauncherService.js",
             "Frontend/src/services/projectSettingsService.js"]
    source = "\n".join((EDITOR / path).read_text(encoding="utf-8") for path in paths)
    for token in ("def create_scene(", "def create_new_scene(", "def remove_scene(", "def switch_scene(",
                  "createScene:", "removeScene:", "migrateLegacyScene:", "choosePortableSceneTarget:",
                  "browseSceneFile:", "setProjectMode:", "copyExistingToData:", "listRoutes:"):
        assert token not in source


def test_only_actor_templates_remain():
    templates = EDITOR / "plugins/ProjectLauncher/templates"
    assert (templates / "actor/demo.actor").is_file()
    assert not list(templates.rglob("*.scene"))
    assert not list(templates.rglob("project.ini"))


def test_main_page_has_one_current_scene_and_no_scene_tabs_or_creation_dialog():
    page = (EDITOR / "Frontend/src/views/layout/MainPage.vue").read_text(encoding="utf-8")
    lifecycle = (EDITOR / "Frontend/src/utils/projectSceneLifecycle.js").read_text(encoding="utf-8")
    assert "const currentSceneId = ref(DEFAULT_SCENE_NAME)" in page
    for token in ("tabs.value", "activeTab", "confirmAddTab", "cancelAddTab", "switchTab", "closeTab",
                  "newTabName", "resolveSceneTabs", "添加场景"):
        assert token not in page
    assert "payload?.scene?.path" in lifecycle
    assert "resolveSceneTabs" not in lifecycle


def test_shipped_examples_have_only_scene_ini_archives():
    examples = ENGINE / "examples/scenes"
    assert not list(examples.rglob("*.scene"))
    assert not list(examples.rglob("project.ini"))
    # Examples are optional; validate any shipped archive instead of requiring
    # the removed learnopengl_ssao fixture.
    from runtime.archive.parser import parse_archive
    for scene in examples.rglob("scene.ini"):
        assert parse_archive(str(scene))["archive_type"] == "portable_scene"


def test_python_adapters_do_not_default_to_old_archive_paths():
    import ast

    for root in ("runtime", "config", "api", "plugins", "script_runtime"):
        for path in (EDITOR / root).rglob("*.py"):
            if "tests" in path.parts:
                continue
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8-sig"))):
                if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
                    continue
                value = node.value.replace("\\", "/")
                assert not (value.endswith(".scene") and "/" in value), (path, value)
                assert value != "project.ini", path
