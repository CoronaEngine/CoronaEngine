"""Guard the migrated boundaries without banning JSON storage or HTTP bodies."""
import re
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[2] / "CoronaEditor" / "editor" / "Frontend" / "src"


def sources():
    return sorted(path for path in SOURCE_ROOT.rglob("*") if path.suffix in {".js", ".vue"})


def test_screenshot_has_one_direct_binding_and_no_old_web_picker_save_or_offscreen_facades():
    api = (SOURCE_ROOT / 'api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'window.corona.scene.captureScreenshot(sceneName, cameraId, options)' in api
    for path in sources():
        text = path.read_text(encoding='utf-8-sig')
        for retired in ['selectScreenshotPath', 'saveScreenshot', 'viewport.capture', 'SceneTools.select_screenshot_path']:
            assert retired not in text, (path, retired)


def test_actor_transform_has_one_v8_facade_without_the_old_manifest_route():
    api = (SOURCE_ROOT / 'api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'window.corona.scene.setActorTransform(sceneName, actorName, transform)' in api
    assert "call_manifest_editor_api('scene.setActorTransform'" not in api
    assert "persist: false" not in (SOURCE_ROOT / 'views/sidebar/Object.vue').read_text(encoding='utf-8-sig')


def test_actor_properties_have_one_facade_and_no_retired_state_physics_or_camera_lock_wrappers():
    api = (SOURCE_ROOT / 'api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'window.corona.scene.updateActor(sceneName, actorName, patch)' in api
    retired = re.compile(r'\bsetActor(?:State|Physics|CameraLock)\b')
    assert [path.relative_to(SOURCE_ROOT).as_posix() for path in sources()
            if retired.search(path.read_text(encoding='utf-8-sig'))] == []
    for name in ['Object.vue', 'SceneBar.vue']:
        source = (SOURCE_ROOT / 'views/sidebar' / name).read_text(encoding='utf-8-sig')
        assert 'editorApi.scene.updateActor(' in source
        assert 'archive_session: archiveSession.value' in source


def test_broadcast_channel_has_one_production_owner():
    constructor = re.compile(r"\bnew\s+(?:(?:window|globalThis)\.)?BroadcastChannel\s*\(")
    owners = [path.relative_to(SOURCE_ROOT).as_posix()
              for path in sources()
              for _ in constructor.finditer(path.read_text(encoding="utf-8-sig"))]
    assert owners == ["api/webMessages.js"]


def test_environment_has_one_scoped_v8_read_and_patch_without_granular_web_aliases():
    api = (SOURCE_ROOT / 'api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'window.corona.scene.getEnvironment(sceneName, scope)' in api
    assert 'window.corona.scene.setEnvironment(sceneName, patch)' in api
    retired = re.compile(r'\b(?:sunDirection|floorGrid|getPhysicsParams|setPhysicsParams)\b|sceneTools\.(?:getEnvironment|setEnvironment)')
    assert [path.relative_to(SOURCE_ROOT).as_posix() for path in sources()
            if retired.search(path.read_text(encoding='utf-8-sig'))] == []
    page = (SOURCE_ROOT / 'views/layout/MainPage.vue').read_text(encoding='utf-8-sig')
    assert 'archive = environmentArchiveSession.value' in page
    assert 'archive_session: context.archive' in page
    assert 'environmentArchiveSession.value = active.archive_session' in page


def test_migrated_web_broadcast_and_drag_regions_do_not_use_dock_commands():
    retired = re.compile(r"\bcmd\s*:\s*['\"](?:broadcast|setDragRegions)['\"]")
    offenders = [path.relative_to(SOURCE_ROOT).as_posix() for path in sources()
                 if retired.search(path.read_text(encoding="utf-8-sig"))]
    assert offenders == []


def test_orphan_node_runtime_protocol_is_removed_instead_of_kept_as_compatibility():
    assert not (SOURCE_ROOT / "services/nodeGraphRuntimeService.js").exists()
    retired = re.compile(r"nodeGraphRuntimeService|node-graph-save-(?:request|accepted|result)|node-graph-runtime-(?:toggle|state)|__coronaNodeGraphFlush|pendingWorldSaves")
    assert [path.relative_to(SOURCE_ROOT).as_posix() for path in sources()
            if retired.search(path.read_text(encoding="utf-8-sig"))] == []
    workspace = (SOURCE_ROOT / "blockly/components/NodeGraphWorkspace.vue").read_text(encoding="utf-8-sig")
    assert "async function handleToggleRun" in workspace
    assert "getNodeGraphSaves().register(" in workspace
    assert "__coronaNodeGraphFlush" not in workspace


def test_retired_script_window_events_and_private_ui_protocols_are_removed():
    retired = re.compile(r"__coronaEmit|_fromCross|creative-controls-(?:request|action|error)|creative-guidance-(?:request|ready|accepted)")
    assert [path.relative_to(SOURCE_ROOT).as_posix() for path in sources()
            if retired.search(path.read_text(encoding="utf-8-sig"))] == []
    assert not (SOURCE_ROOT / "services/nativeGuidanceRelay.js").exists()


def test_migrated_scene_reads_and_selection_have_no_manifest_or_numeric_handle_adapter():
    api = (SOURCE_ROOT / "api/editorApi.js").read_text(encoding="utf-8-sig")
    assert "readActive: () => window.corona.scene.readActive()" in api
    assert "window.corona.scene.selectActor(" in api
    assert not re.search(r"call_manifest_editor_api\(\s*['\"](?:scene.readActive|sceneTools.selectActor)['\"]", api)
    for component in ["views/layout/MainPage.vue", "views/tools/CameraView.vue"]:
        text = (SOURCE_ROOT / component).read_text(encoding="utf-8-sig")
        assert not re.search(r"sourceCameraHandle:\s*Number\(", text)
    camera = (SOURCE_ROOT / "views/tools/CameraView.vue").read_text(encoding="utf-8-sig")
    assert re.search(r"isViewportGizmoSelectionOwner\(\{\s*viewportScope: 'cameraView',\s*cameraId,\s*selection: payload,", camera)
    gizmo = (SOURCE_ROOT / "utils/viewportGizmo.js").read_text(encoding="utf-8-sig")
    assert "selection.source_camera_id" in gizmo
    assert "sourceCameraId === String(cameraId || '').trim()" in gizmo
    assert "cameraHandle" not in gizmo
