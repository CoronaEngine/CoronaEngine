import ast
from pathlib import Path

import pytest


BACKEND_ROOT = Path(__file__).resolve().parents[1] / "backend"


def test_project_settings_does_not_import_unused_editor_runtime_container():
    source = (BACKEND_ROOT.parent / "plugins" / "ProjectSettings" / "main.py").read_text(encoding="utf-8")
    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source


def test_file_system_routes_compat_events_through_editor_api_adapter():
    source = (BACKEND_ROOT.parent / "plugins" / "FileManager" / "main.py").read_text(encoding="utf-8")
    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source
    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditorApi.files" in source


def test_file_manager_delegates_legacy_scene_bindings_to_runtime_adapter():
    source = (BACKEND_ROOT.parent / "plugins" / "FileManager" / "main.py").read_text(encoding="utf-8")

    assert "legacy_file_scene_adapter" not in source
    assert "from runtime.legacy_scene_store import" not in source
    assert "legacy_scene_store." not in source


def test_legacy_file_scene_adapter_owns_legacy_scene_bindings():
    adapter = BACKEND_ROOT.parent / "plugins" / "FileManager" / "compat" / "legacy_file_scene_adapter.py"
    shim = BACKEND_ROOT.parent / "runtime" / "legacy_file_scene_adapter.py"
    source = adapter.read_text(encoding="utf-8")

    assert "from runtime.legacy_scene_store import legacy_scene_store" in source
    assert "emit_compat_editor_event" in source
    assert "def rename_file_binding" in source
    assert "def open_file_binding" in source
    assert "from plugins.FileManager.compat.legacy_file_scene_adapter import *" in shim.read_text(
        encoding="utf-8"
    )


def test_blockly_uses_editor_api_compatibility_adapter():
    source = (
        BACKEND_ROOT.parent / "script_runtime" / "blockly" / "main.py"
    ).read_text(encoding="utf-8")
    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source
    assert "from CoronaCore.core.legacy.managers import scene_manager" not in source
    assert "from script_runtime.compat.legacy_scene_adapter import" in source
    assert "legacy_scene_store" not in source
    assert "set_compat_editor_camera_input_enabled" in source
    assert "get_compat_editor_selection" in source


def test_script_runtime_does_not_import_legacy_entity_modules_at_runtime():
    repo_root = BACKEND_ROOT.parents[1]
    for relative_path in (
        "editor/script_runtime/engine/entities/actor_script.py",
        "editor/script_runtime/engine/entities/scene_script.py",
        "editor/script_runtime/engine/scripts_manager.py",
    ):
        source = (repo_root / relative_path).read_text(encoding="utf-8")
        tree = ast.parse(source)
        assert all(
            not isinstance(node, ast.ImportFrom)
            or node.module != "CoronaCore.core.legacy.entities"
            for node in tree.body
        )


def test_script_runtime_type_annotations_do_not_depend_on_legacy_entity_paths():
    repo_root = BACKEND_ROOT.parents[1]
    sources = [
        repo_root / "editor/script_runtime/engine/entities/actor_script.py",
        repo_root / "editor/script_runtime/engine/entities/scene_script.py",
        repo_root / "editor/script_runtime/engine/scripts_manager.py",
    ]

    for path in sources:
        source = path.read_text(encoding="utf-8")
        assert "runtime.legacy.entities" not in source, path
    assert (repo_root / "editor/script_runtime/engine/contracts.py").is_file()


def test_network_contract_is_not_available_to_script_runtime():
    manifest_source = (
        BACKEND_ROOT.parents[1] / "src/systems/ui/editor_api/cef_editor_api.cpp"
    ).read_text(encoding="utf-8-sig")
    assert "return caller_mask(EditorApiCaller::Cef) | caller_mask(EditorApiCaller::PythonScript);" in manifest_source
    assert "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(Network," not in manifest_source

    script_runtime_section = (
        BACKEND_ROOT.parents[1] / "editor/script_runtime/native_engine_adapter.py"
    ).read_text(encoding="utf-8-sig")
    assert "network_" not in script_runtime_section


def test_scene_tools_uses_public_project_context_for_active_path():
    scene_tools_source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    vision_adapter_source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/compat/legacy_vision_import_adapter.py"
    ).read_text(encoding="utf-8")
    runtime_source = (
        BACKEND_ROOT.parents[1] / "editor/runtime/legacy_engine_adapter.py"
    ).read_text(encoding="utf-8")

    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in scene_tools_source
    assert "get_active_project_path" not in scene_tools_source
    assert "get_active_project_path" in vision_adapter_source
    assert "get_editor_engine_adapter" not in scene_tools_source
    assert "class EditorEngineAdapter" in runtime_source


def test_scene_tools_routes_vision_legacy_scene_lookup_through_runtime_adapter():
    scene_tools_source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    adapter = BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/compat/legacy_vision_scene_adapter.py"
    import_adapter = BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/compat/legacy_vision_import_adapter.py"

    assert "from plugins.SceneTools.compat.legacy_vision_import_adapter import" in scene_tools_source
    assert "from plugins.SceneTools.compat.legacy_vision_scene_adapter import" in import_adapter.read_text(encoding="utf-8")
    assert "from runtime.legacy_scene_store import" not in scene_tools_source
    assert "scene_manager.get" not in scene_tools_source
    assert adapter.is_file()
    assert "from runtime.legacy_scene_store import legacy_scene_store" in adapter.read_text(
        encoding="utf-8"
    )
    assert "from plugins.SceneTools.compat.legacy_vision_import_adapter import *" in (
        BACKEND_ROOT.parents[1] / "editor/runtime/legacy_vision_import_adapter.py"
    ).read_text(encoding="utf-8")


def test_editor_host_camera_lock_uses_scene_tools_contract():
    source = (BACKEND_ROOT.parents[1] / "editor/runtime/editor_host.py").read_text(
        encoding="utf-8"
    )
    start = source.index("    def camera_lock_set")
    end = source.index("    @classmethod\n    def object_key_down", start)
    camera_lock_source = source[start:end]

    assert "CoronaEditorApi.scene_tools.set_actor_camera_lock" in camera_lock_source
    assert "legacy_scene_store" not in camera_lock_source
    assert "camera_follow_clear" in camera_lock_source


def test_editor_host_legacy_camera_follow_isolated_in_runtime_adapter():
    source = (BACKEND_ROOT.parents[1] / "editor/runtime/editor_host.py").read_text(
        encoding="utf-8"
    )
    adapter = BACKEND_ROOT.parents[1] / "editor/runtime/legacy_camera_follow.py"
    start = source.index("    def _update_camera_follow")
    end = source.index("    scripts_mgr = None", start)
    method_source = source[start:end]

    assert "from runtime.legacy_camera_follow import update_camera_follow" in method_source
    assert "legacy_scene_store" not in method_source
    assert adapter.is_file()
    assert "from runtime.legacy_scene_store import legacy_scene_store" in adapter.read_text(
        encoding="utf-8"
    )


def test_main_view_legacy_scene_store_isolated_in_runtime_adapter():
    main_view = BACKEND_ROOT.parents[1] / "editor/plugins/MainView/main.py"
    adapter = BACKEND_ROOT.parents[1] / "editor/plugins/MainView/compat/legacy_main_view_scene_adapter.py"
    adapter_shim = BACKEND_ROOT.parents[1] / "editor/runtime/legacy_main_view_scene_adapter.py"
    source = main_view.read_text(encoding="utf-8")

    assert "from runtime.legacy_scene_store import legacy_scene_store" not in source
    assert "legacy_main_view_scene_adapter" not in source
    assert adapter.is_file()
    adapter_source = adapter.read_text(encoding="utf-8")
    assert "from runtime.legacy_scene_store import legacy_scene_store" in adapter_source
    assert "def get_or_create_scene" in adapter_source
    assert "def remove_scene" in adapter_source
    assert "from plugins.MainView.compat.legacy_main_view_scene_adapter import *" in adapter_shim.read_text(
        encoding="utf-8"
    )


def test_main_view_legacy_scene_adapter_forwards_scene_lifecycle(monkeypatch):
    from plugins.MainView.compat import legacy_main_view_scene_adapter as scene_adapter

    calls = []

    class Scene:
        def set_enabled(self, enabled):
            calls.append(("set_enabled", enabled))

    class Store:
        def get(self, route):
            calls.append(("get", route))
            return Scene()

        def get_or_create(self, route):
            calls.append(("get_or_create", route))
            return Scene()

        def remove(self, route):
            calls.append(("remove", route))

    monkeypatch.setattr(scene_adapter, "legacy_scene_store", Store())

    scene_adapter.get_or_create_scene("Scene/a.scene")
    scene_adapter.get_scene("Scene/a.scene")
    scene_adapter.remove_scene("Scene/a.scene")

    assert calls == [
        ("get_or_create", "Scene/a.scene"),
        ("get", "Scene/a.scene"),
        ("get", "Scene/a.scene"),
        ("set_enabled", False),
        ("remove", "Scene/a.scene"),
    ]


def test_editor_host_script_initialization_uses_script_runtime_engine_owner():
    source = (BACKEND_ROOT.parents[1] / "editor/runtime/editor_host.py").read_text(
        encoding="utf-8"
    )
    host_owner = BACKEND_ROOT.parents[1] / "editor/script_runtime/engine/host.py"
    adapter = BACKEND_ROOT.parents[1] / "editor/script_runtime/compat/legacy_script_runtime_adapter.py"
    adapter_shim = BACKEND_ROOT.parents[1] / "editor/runtime/legacy_script_runtime_adapter.py"
    start = source.index("        if not cls._scripts_initialized")
    end = source.index("        cls.checkpoint()", start)
    initialization_source = source[start:end]

    assert "from runtime.legacy_scene_store import" not in initialization_source
    assert "from script_runtime.engine.host import initialize_scripts" in initialization_source
    assert host_owner.is_file()
    assert not adapter.exists()
    owner_source = host_owner.read_text(encoding="utf-8")
    assert "def initialize_scripts" in owner_source
    assert not adapter_shim.exists()


def test_scene_tools_routes_environment_and_physics_through_editor_api():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditorApi.scene_tools.sun_direction" in source
    assert "CoronaEditorApi.scene_tools.floor_grid" in source
    assert "CoronaEditorApi.scene_tools.set_physics_params" in source
    assert "CoronaEditorApi.scene_tools.get_physics_params" in source
    assert "scene.set_sun_direction" not in source
    assert "scene.set_floor_grid" not in source
    assert "scene.set_gravity" not in source


def test_scene_tools_routes_camera_debug_through_editor_api():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    camera_debug_source = source[
        source.index("    def set_output_mode") : source.index(
            "    def prepare_external_live_vision_scene"
        )
    ]

    for method in (
        "set_render_backend",
        "get_render_backend",
        "set_vision_render_mode",
        "get_vision_render_mode",
        "set_shadow_cascade_debug",
        "get_shadow_cascade_debug",
        "set_ssao_enabled",
        "get_ssao_enabled",
        "set_output_mode",
        "get_output_mode",
    ):
        assert f"CoronaEditorApi.scene_tools.{method}" in camera_debug_source

    assert "camera.set_output_mode" not in camera_debug_source
    assert "camera.get_output_mode" not in camera_debug_source
    assert "camera.set_render_backend" not in camera_debug_source
    assert "camera.get_render_backend" not in camera_debug_source
    assert "_ENGINE.set_camera_shadow_cascade_debug" not in camera_debug_source
    assert "_ENGINE.get_camera_shadow_cascade_debug" not in camera_debug_source
    assert "_ENGINE.set_camera_ssao_enabled" not in camera_debug_source
    assert "_ENGINE.get_camera_ssao_enabled" not in camera_debug_source


def test_scene_tools_routes_camera_view_lifecycle_through_editor_api():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    lifecycle_source = source[
        source.index("    def open_camera_view") : source.index(
            "    def load_vision_scene"
        )
    ]

    for method in (
        "open_camera_view",
        "close_camera_view",
        "rename_camera_view",
        "list_camera_views",
        "update_camera_view",
        "delete_camera",
    ):
        assert f"CoronaEditorApi.scene_tools.{method}" in lifecycle_source

    assert "scene_manager.get" not in lifecycle_source
    assert "scene.save_data" not in lifecycle_source
    assert "camera.set_view_state" not in lifecycle_source


def test_scene_tools_routes_vision_bridge_through_editor_api():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    vision_source = source[
        source.index("    def is_vision_available") : source.index(
            "    def import_vision_scene_into_current_scene"
        )
    ]

    assert "CoronaEditorApi.scene_tools.is_vision_available" in vision_source
    assert "CoronaEditorApi.scene_tools.load_vision_scene" in vision_source
    assert "_ENGINE.is_vision_available" not in vision_source
    assert "_ENGINE.load_vision_scene" not in vision_source


def test_scene_tools_routes_screenshot_through_editor_api():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")
    screenshot_source = source[
        source.index("    def save_screenshot") : source.index(
            "    def set_output_mode"
        )
    ]

    assert "CoronaEditorApi.scene_tools.save_screenshot" in screenshot_source
    assert "scene_manager.get" not in screenshot_source
    assert "camera.save_screenshot" not in screenshot_source


def test_scene_tools_vision_import_uses_aggregate_native_operations():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/compat/legacy_vision_import_adapter.py"
    ).read_text(encoding="utf-8")
    import_source = source[
            source.index("def import_vision_scene_into_current_scene") : source.index(
            "def import_embedded_vision_scene_into_current_scene"
        )
    ]

    for method in (
        "is_vision_available",
        "create_actor",
        "load_vision_scene",
        "set_render_backend",
    ):
        assert f"CoronaEditorApi.scene_tools.{method}" in import_source

    assert "_ENGINE.is_vision_available" not in import_source
    assert "_ENGINE.create_editor_actor" not in import_source
    assert "_ENGINE.load_vision_scene" not in import_source
    assert "active_camera.set_render_backend" not in import_source


def test_editor_engine_adapter_preserves_native_handler_calls():
    from CoronaCore.core.engine_runtime import EditorEngineAdapter

    class FakeEngine:
        active_project_path = "D:/project"

        def is_vision_available(self):
            return 1

        def set_render_backend(self, mode):
            self.mode = mode

        def get_render_backend(self):
            return self.mode

        def load_vision_scene(self, path):
            self.loaded = path

        def create_editor_actor(self, *args):
            self.actor_args = args
            return "{\"status\": \"success\"}"

    engine = FakeEngine()
    adapter = EditorEngineAdapter(engine)

    assert adapter.active_project_path == "D:/project"
    assert adapter.is_vision_available() is True
    adapter.set_render_backend("vision")
    assert adapter.get_render_backend() == "vision"
    adapter.load_vision_scene("scene.json")
    assert engine.loaded == "scene.json"
    assert adapter.create_editor_actor("scene", "asset", "model", "{}") == "{\"status\": \"success\"}"
    assert engine.actor_args == ("scene", "asset", "model", "{}")


def test_legacy_runtime_entities_use_the_runtime_engine_adapter_owner():
    runtime_root = BACKEND_ROOT.parents[1] / "editor/runtime/legacy"
    sources = [
        runtime_root / "entities" / name
        for name in ("actor.py", "camera.py", "environment.py")
    ] + [
        runtime_root / "components" / name
        for name in ("acoustics.py", "geometry.py", "kinematics.py", "mechanics.py", "optics.py")
    ]

    assert sources
    for path in sources:
        source = path.read_text(encoding="utf-8")
        assert "from CoronaCore.core.engine_runtime import" not in source
        assert "from runtime.legacy_engine_adapter import" in source


def test_scene_tools_uses_project_context_adapter_for_active_path():
    source = (BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/compat/legacy_vision_import_adapter.py").read_text(
        encoding="utf-8"
    )

    assert "from api.editor_api import CoronaEditorApi, get_active_project_path" in source
    assert "get_editor_engine_adapter" not in source
    assert "_ENGINE =" not in source
    assert "return get_active_project_path()" in source


def test_editor_api_scene_datas_isolated_in_legacy_adapter():
    editor_api = BACKEND_ROOT.parents[1] / "editor/api/editor_api.py"
    adapter = BACKEND_ROOT.parents[1] / "editor/script_runtime/compat/legacy_scene_datas_adapter.py"
    historical_adapter = BACKEND_ROOT.parents[1] / "editor/script_runtime/legacy_scene_datas_adapter.py"
    compatibility = BACKEND_ROOT.parents[1] / "editor/CoronaCore/core/legacy_scene_datas_adapter.py"
    historical_compatibility = BACKEND_ROOT.parents[1] / "editor/CoronaCore/core/legacy_editor_api.py"
    source = editor_api.read_text(encoding="utf-8")

    assert "class _SceneDatasApi" not in source
    assert "LegacySceneDatasApi" in source
    assert "from script_runtime.compat.legacy_scene_datas_adapter import LegacySceneDatasApi" in source
    assert adapter.is_file()
    adapter_source = adapter.read_text(encoding="utf-8")
    assert "class LegacySceneDatasApi" in adapter_source
    assert "scene_datas.get_scene" in adapter_source
    assert historical_adapter.is_file()
    historical_source = historical_adapter.read_text(encoding="utf-8")
    assert "from script_runtime.compat.legacy_scene_datas_adapter import *" in historical_source
    assert compatibility.is_file()
    compatibility_source = compatibility.read_text(encoding="utf-8")
    assert "from script_runtime.compat.legacy_scene_datas_adapter import LegacySceneDatasApi" in compatibility_source
    assert "class LegacySceneDatasApi" not in compatibility_source
    assert historical_compatibility.is_file()
    historical_source = historical_compatibility.read_text(encoding="utf-8")
    assert "from script_runtime.compat.legacy_scene_datas_adapter import LegacySceneDatasApi" in historical_source
    assert "from CoronaCore.core.legacy_scene_datas_adapter import LegacySceneDatasApi" not in historical_source


def test_editor_api_legacy_network_fallbacks_have_runtime_adapter_owner():
    repo_root = BACKEND_ROOT.parents[1]
    editor_api = repo_root / "editor/api/editor_api.py"
    adapter = repo_root / "editor/runtime/legacy_network_adapters.py"
    source = editor_api.read_text(encoding="utf-8")

    assert adapter.is_file()
    assert "class _LegacyNetworkAdapter" not in source
    assert "class _LegacyLanChatAdapter" not in source
    assert "class _LegacyLanChatQueueAdapter" not in source
    assert "class _LegacyLanChatTransportAdapter" not in source
    assert "from runtime.legacy_network_adapters import" in source
    adapter_source = adapter.read_text(encoding="utf-8")
    assert "class _LegacyNetworkAdapter" in adapter_source
    assert "class _LegacyLanChatAdapter" in adapter_source
    assert "class _LegacyLanChatQueueAdapter" in adapter_source
    assert "class _LegacyLanChatTransportAdapter" in adapter_source


def test_editor_api_legacy_scene_fallbacks_have_runtime_adapter_owner():
    repo_root = BACKEND_ROOT.parents[1]
    editor_api = repo_root / "editor/api/editor_api.py"
    adapter = repo_root / "editor/runtime/legacy_scene_adapters.py"
    source = editor_api.read_text(encoding="utf-8")

    assert adapter.is_file()
    for class_name in (
        "_LegacySceneToolsAdapter",
        "_LegacySceneAdapter",
        "_LegacyViewportAdapter",
    ):
        assert f"class {class_name}" not in source
    assert "from runtime.legacy_scene_adapters import" in source
    adapter_source = adapter.read_text(encoding="utf-8")
    for class_name in (
        "_LegacySceneToolsAdapter",
        "_LegacySceneAdapter",
        "_LegacyViewportAdapter",
    ):
        assert f"class {class_name}" in adapter_source


def test_editor_engine_adapter_rejects_missing_native_operation():
    from CoronaCore.core.engine_runtime import EditorEngineAdapter

    with pytest.raises(RuntimeError, match="is_vision_available.*unavailable"):
        EditorEngineAdapter(object()).is_vision_available()


def test_paths_config_uses_the_project_context_adapter():
    source = (
        BACKEND_ROOT.parents[1] / "editor/config/paths_config.py"
    ).read_text(encoding="utf-8")

    assert "from CoronaCore.core.corona_editor import CoronaEditor" not in source
    assert "get_active_project_path" in source
    assert "from runtime.project_context import get_active_project_path" in source
    assert "from api.editor_api import get_active_project_path" not in source


def test_scene_tools_does_not_leak_legacy_camera_engine_objects():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")

    assert "engine_obj" not in source


def test_editor_engine_adapter_preserves_camera_debug_fallbacks():
    from CoronaCore.core.engine_runtime import EditorEngineAdapter

    class CameraWrapper:
        def __init__(self):
            self.shadow = False
            self.ssao = True

        def set_shadow_cascade_debug(self, enabled):
            self.shadow = enabled

        def get_shadow_cascade_debug(self):
            return self.shadow

        def set_ssao_enabled(self, enabled):
            self.ssao = enabled

        def get_ssao_enabled(self):
            return self.ssao

    class NativeCamera:
        def __init__(self):
            self.shadow = False
            self.ssao = True

        def set_shadow_cascade_debug(self, enabled):
            self.shadow = enabled

        def get_shadow_cascade_debug(self):
            return self.shadow

        def set_ssao_enabled(self, enabled):
            self.ssao = enabled

        def get_ssao_enabled(self):
            return self.ssao

    adapter = EditorEngineAdapter(object())
    wrapper = CameraWrapper()
    native = NativeCamera()
    fallback = type("Camera", (), {"engine_obj": native})()

    adapter.set_camera_shadow_cascade_debug(wrapper, True)
    adapter.set_camera_ssao_enabled(wrapper, False)
    assert adapter.get_camera_shadow_cascade_debug(wrapper) is True
    assert adapter.get_camera_ssao_enabled(wrapper) is False

    adapter.set_camera_shadow_cascade_debug(fallback, True)
    adapter.set_camera_ssao_enabled(fallback, False)
    assert adapter.get_camera_shadow_cascade_debug(fallback) is True
    assert adapter.get_camera_ssao_enabled(fallback) is False


def test_script_runtime_media_isolated_from_editor_scene_tools_contract():
    manifest = (
        BACKEND_ROOT.parents[1] / "src/systems/ui/editor_api/cef_editor_api.cpp"
    ).read_text(encoding="utf-8-sig")
    editor_api = (
        BACKEND_ROOT.parents[1] / "editor/api/editor_api.py"
    ).read_text(encoding="utf-8")
    script_runtime_adapter = (
        BACKEND_ROOT.parents[1] / "editor/script_runtime/native_engine_adapter.py"
    ).read_text(encoding="utf-8")
    scratch = (
        BACKEND_ROOT.parents[1] / "editor/script_runtime/engine/corona_engine.py"
    ).read_text(encoding="utf-8")

    for method in ("play_audio", "stop_audio", "actor_play_audio", "actor_stop_audio"):
        method_lines = [line for line in manifest.splitlines() if f"SceneTools, {method}" in line]
        assert any("EDITOR_API_METHOD_SCHEMA_WRAPPED(" in line for line in method_lines)
        assert not any("EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(" in line for line in method_lines)

    assert "class ScriptRuntimeNativeEngineAdapter" in script_runtime_adapter
    assert "def import_media" in script_runtime_adapter
    assert "def play_audio" in script_runtime_adapter
    assert "def stop_audio" in script_runtime_adapter
    assert "from script_runtime.native_engine_adapter import get_script_runtime_adapter" in scratch
    assert "get_script_runtime_adapter" in editor_api
    assert "from runtime.legacy_editor_adapters import" in editor_api


def test_scene_tools_handler_does_not_import_engine_components_directly():
    source = (
        BACKEND_ROOT.parents[1] / "editor/plugins/SceneTools/main.py"
    ).read_text(encoding="utf-8")

    assert "from CoronaCore.core.components" not in source
    assert "from CoronaCore.core.entities" not in source
    assert "from CoronaCore.core.legacy.components" not in source
    assert "from CoronaCore.core.legacy.entities" not in source


def test_editor_business_modules_do_not_bypass_runtime_and_legacy_adapters():
    repo_root = BACKEND_ROOT.parents[1]
    production_roots = (
        repo_root / "editor/plugins",
        repo_root / "editor/backend",
    )
    allowed_native_host = {
        repo_root / "editor/backend/registry.py",
    }
    forbidden_imports = (
        "from CoronaCore.core.corona_editor import CoronaEditor",
        "from CoronaCore.core.engine_runtime import CoronaEngine",
        "from CoronaCore.core.entities import",
        "from CoronaCore.core.components import",
        "from CoronaCore.core.managers import scene_manager",
        "from CoronaCore.core.legacy.entities import",
        "from CoronaCore.core.legacy.components import",
        "from CoronaCore.core.legacy.managers import scene_manager",
    )

    violations = []
    for root in production_roots:
        for path in root.rglob("*.py"):
            if "tests" in path.parts or path in allowed_native_host:
                continue
            source = path.read_text(encoding="utf-8")
            for forbidden in forbidden_imports:
                if forbidden in source:
                    violations.append(f"{path}: {forbidden}")

    assert violations == []


def test_editor_business_modules_do_not_import_raw_corona_engine_module():
    repo_root = BACKEND_ROOT.parents[1]
    production_root = repo_root / "editor/plugins"
    violations = []

    for path in production_root.rglob("*.py"):
        if "tests" in path.parts or "Quasar" in path.parts:
            continue
        source = path.read_text(encoding="utf-8-sig")
        tree = ast.parse(source, filename=str(path))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                modules = [node.module or ""]
            else:
                continue
            if any(module == "CoronaEngine" or module.startswith("CoronaEngine.") for module in modules):
                violations.append(str(path))

    assert violations == []


def test_editor_business_modules_do_not_call_legacy_editor_bindings_directly():
    repo_root = BACKEND_ROOT.parents[1]
    production_roots = (repo_root / "editor/plugins", repo_root / "editor/backend")
    legacy_names = (
        "create_editor_actor",
        "remove_editor_actor",
        "get_editor_scene_snapshot",
        "get_editor_actor_bounds",
        "set_editor_actor_transform",
        "set_editor_camera_transform",
        "capture_editor_camera_view",
    )

    violations = []
    for root in production_roots:
        for path in root.rglob("*.py"):
            if "tests" in path.parts or "Quasar" in path.parts:
                continue
            source = path.read_text(encoding="utf-8")
            for name in legacy_names:
                if f"CoronaEngine.{name}" in source or f"CoronaEditor.CoronaEngine.{name}" in source:
                    violations.append(f"{path}: {name}")

    assert violations == []


def test_editor_ai_extensions_use_runtime_project_context_not_private_paths_helper():
    repo_root = BACKEND_ROOT.parents[1]
    sources = {
        path: (repo_root / path).read_text(encoding="utf-8")
        for path in (
            "editor/plugins/AITool/cai_extensions/paths_provider.py",
            "editor/plugins/AITool/cai_extensions/agent/model_provider.py",
            "editor/plugins/AITool/cai_extensions/agent/scene_composer.py",
            "editor/plugins/AITool/cai_extensions/flows/model_retrieval_workflow/helpers.py",
            "editor/plugins/AITool/cai_extensions/flows/model_retrieval_workflow/local_model_library.py",
        )
    }

    for path, source in sources.items():
        assert (
            "runtime.project_context" in source
            or "from runtime import project_context" in source
        ), path
        assert "_get_active_project_path" not in source, path


def test_editor_ai_extensions_use_editor_screenshot_path_owner():
    repo_root = BACKEND_ROOT.parents[1]
    sources = {
        path: (repo_root / path).read_text(encoding="utf-8")
        for path in (
            "editor/plugins/AITool/cai_extensions/agent/vlm_capture.py",
            "editor/plugins/AITool/cai_extensions/mcp/tools/multi_view_capture.py",
            "editor/plugins/AITool/cai_extensions/mcp/tools/camera_tools.py",
        )
    }

    for path, source in sources.items():
        assert "config.paths_config" in source, path
        assert "Quasar.ai_config.paths_config" not in source, path
