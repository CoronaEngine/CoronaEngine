"""Keep retired duplicate native bindings out of both sides of the bridge."""
from pathlib import Path


def test_camera_reads_retire_full_scene_and_camera_list_web_routes():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['get_scene_snapshot', 'list_camera_views']:
        declaration = next(line for line in manifest.splitlines() if f'SceneTools, {name},' in line)
        assert 'WRAPPED_CALLERS' in declaration and '"", "scene' in declaration
        assert '~caller_mask(EditorApiCaller::Cef)' in declaration or 'caller_mask(EditorApiCaller::PythonScript)' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'getSnapshot:' not in facade and 'listCameraViews:' not in facade
    assert 'window.corona.scene.readCameras(sceneName, scope)' in facade


def test_tree_read_retires_manifest_and_shares_owned_rows():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, list_scene_tree,' in line)
    assert 'WRAPPED_CALLERS' in declaration and '"", "scene_tools.list_scene_tree"' in declaration
    assert 'caller_mask(EditorApiCaller::Cef)' not in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'listSceneTree:' not in facade
    assert 'window.corona.scene.readTree(sceneName, scope)' in facade
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert 'scene_tree_actor_to_json(scene_tree_actor_row(actor))' in native
    assert 'reply.tree.actors.push_back(scene_tree_actor_row(actor))' in native


def test_archive_read_is_not_exposed_to_web_without_a_production_consumer():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, read_archive_scene,' in line)
    assert '"", "scene.read_archive_scene"' in declaration
    assert 'animation_callers() & ~caller_mask(EditorApiCaller::Cef)' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'readArchive:' not in facade
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert '{"read_archive_scene", [](const NativeRequest& request, const NativeContext&) {' in native


def test_actor_property_read_retires_duplicate_web_routes():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, read_actor,' in line)
    assert '"", "scene.read_actor"' in declaration
    assert '~caller_mask(EditorApiCaller::Cef)' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'getActor:' not in facade and 'readActor:' not in facade
    assert 'window.corona.scene.readActorProperties(sceneName, actor, scope)' in facade


def test_camera_window_toggles_are_not_cef_manifest_routes():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['open_camera_view', 'close_camera_view', 'create_camera_view']:
        declaration = next(line for line in manifest.splitlines() if f'SceneTools, {name},' in line)
        assert 'WRAPPED_CALLERS' in declaration
        assert 'caller_mask(EditorApiCaller::PythonScript)' in declaration
        assert '"", "scene_tools.' in declaration


def test_camera_v8_properties_cannot_write_window_lifecycle():
    root = Path(__file__).resolve().parents[2]
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    parser = binding.split('bool read_camera_properties(')[1].split('return vector(')[0]
    assert '"view_open"' not in parser


def test_script_input_retires_all_legacy_web_routes_and_duplicate_native_type():
    root = Path(__file__).resolve().parents[2]
    for name in ['cef_renderer_bridge.cpp', 'cef_viewport_gizmo.cpp']:
        source = (root / 'src/systems/ui/cef' / name).read_text(encoding='utf-8-sig')
        for retired in ['"InputInject"', '"injectInput"', 'handle_input_inject']:
            assert retired not in source, (name, retired)
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['key_event', 'key_release', 'mouse_event']:
        assert f'WRAPPED_CALLERS(Scripts, {name},' not in manifest
    binding = (root / 'src/systems/script/python/engine_bindings.cpp').read_text(encoding='utf-8-sig')
    assert 'struct InputEvent {' not in binding


def test_web_pick_has_no_fast_route_json_broker_or_result_event():
    root = Path(__file__).resolve().parents[2]
    assert not (root / 'src/systems/ui/cef/request_response_broker.cpp').exists()
    assert not (root / 'src/systems/ui/cef/request_response_broker.h').exists()
    for name in ['src/systems/ui/cef/cef_renderer_bridge.cpp', 'src/systems/ui/cef/cef_viewport_gizmo.cpp',
                 'src/systems/ui/editor_api/cef_editor_api.cpp', 'CoronaEditor/editor/Frontend/src/api/editorApi.js',
                 'CoronaEditor/editor/Frontend/src/api/sceneReader.js', 'CoronaEditor/editor/api/editor_api.py']:
        source = (root / name).read_text(encoding='utf-8-sig')
        for retired in ['"ViewportPick"', '"pickActor"', 'handle_viewport_pick', 'cef_request_response_broker',
                        'actorPickResult', 'onActorPickResult', 'on_actor_pick_result']:
            assert retired not in source, (name, retired)

def test_orphan_mesh_move_web_transport_and_event_are_removed():
    root = Path(__file__).resolve().parents[2]
    for name in ['src/systems/ui/cef/cef_renderer_bridge.cpp',
                 'src/systems/ui/cef/cef_viewport_gizmo.cpp',
                 'src/systems/ui/editor_api/cef_editor_api.cpp',
                 'CoronaEditor/editor/Frontend/src/api/editorApi.js',
                 'CoronaEditor/editor/Frontend/src/api/sceneReader.js']:
        source = (root / name).read_text(encoding='utf-8-sig')
        for retired in ['moveActorMesh', 'ActorMeshMove', 'handle_mesh_move',
                        'MeshMoveCallbackTask', 'meshMoveResult',
                        'onMeshMoveResult', 'on_mesh_move_result']:
            assert retired not in source, (name, retired)


def test_unused_duplicate_property_fast_binding_and_receiver_are_removed():
    native = Path(__file__).resolve().parents[2] / 'src/systems/ui/cef'
    for filename in ['cef_renderer_bridge.cpp', 'cef_viewport_gizmo.cpp']:
        source = (native / filename).read_text(encoding='utf-8-sig')
        assert 'PropertyFast' not in source
        assert '"setProperty"' not in source
        assert 'handle_property_fast' not in source
    assert 'resolve_profile_handles' not in (native / 'cef_viewport_gizmo.cpp').read_text(encoding='utf-8-sig')


def test_orphan_focus_fast_result_queue_and_resource_search_forwarder_are_deleted():
    root = Path(__file__).resolve().parents[2]
    for path in [root / 'src/systems/ui/cef/cef_renderer_bridge.cpp',
                 root / 'src/systems/ui/cef/cef_viewport_gizmo.cpp',
                 root / 'src/systems/ui/editor_api/cef_editor_api.cpp',
                 root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js',
                 root / 'CoronaEditor/editor/Frontend/src/api/sceneReader.js']:
        source = path.read_text(encoding='utf-8-sig')
        for name in ['ComputeActorFocusPoseFast', 'computeActorFocusPose', 'focusPoseResult',
                     'onFocusPoseResult', 'ResourceSearch.focus_actor', 'resourceSearch.focusActor']:
            assert name not in source, (path, name)
    handlers = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert '{"focus_actor", script_method}' not in handlers


def test_live_and_non_web_offscreen_capture_share_the_native_save_operation():
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert source.count('->save_screenshot_sync(') == 1
    assert source.count('save_native_camera_screenshot(*camera,') == 3


def test_retired_camera_move_transport_and_exclusive_queue_are_removed():
    root = Path(__file__).resolve().parents[2]
    for name in ['src/systems/ui/cef/cef_renderer_bridge.cpp', 'src/systems/ui/cef/cef_viewport_gizmo.cpp',
                 'src/shared_data_hub.cpp', 'include/corona/shared_data_hub.h', 'src/systems/optics/optics_system.cpp']:
        source = (root / name).read_text(encoding='utf-8-sig')
        for retired in ['CameraMoveFast', '"cameraMove"', 'handle_camera_move_fast', 'parse_vec3_list',
                        'CameraMoveCommand', 'enqueue_camera_move', 'drain_camera_moves', 'pending_camera_moves_',
                        'camera_move_mutex_', 'camera_move_sequence_', 'apply_pending_camera_moves']:
            assert retired not in source, (name, retired)


def test_viewport_configuration_retires_the_three_fast_routes():
    root = Path(__file__).resolve().parents[2]
    for name in ['cef_renderer_bridge.cpp', 'cef_viewport_gizmo.cpp']:
        source = (root / 'src/systems/ui/cef' / name).read_text(encoding='utf-8-sig')
        for retired in ['"CameraViewportFast"', '"ViewportUiMode"', '"ViewportUiCalibration"',
                        '"setCameraViewport"', '"setViewportUiMode"', '"setViewportUiCalibration"',
                        'handle_camera_viewport_fast', 'handle_viewport_ui_mode', 'handle_viewport_ui_calibration']:
            assert retired not in source, (name, retired)


def test_pointer_configuration_retires_duplicate_cursor_transport_and_queue():
    root = Path(__file__).resolve().parents[2]
    for name in ['src/systems/ui/cef/cef_renderer_bridge.cpp', 'src/systems/ui/cef/cef_viewport_gizmo.cpp',
                 'include/corona/shared_data_hub.h', 'src/shared_data_hub.cpp',
                 'include/corona/systems/optics/optics_system.h', 'src/systems/optics/optics_system.cpp']:
        source = (root / name).read_text(encoding='utf-8-sig')
        for retired in ['"ViewportUiPointer"', '"ViewportSystemCursor"', '"viewportUiPointer"',
                        '"setViewportSystemCursorHidden"', 'ViewportUiPointerCommand',
                        'enqueue_viewport_ui_pointer', 'drain_viewport_ui_pointer_commands',
                        'pending_viewport_ui_pointer_commands_', 'viewport_ui_pointer_sequence_',
                        'viewport_cursor_states_', 'set_browser_tab_system_cursor_state']:
            assert retired not in source, (name, retired)


def test_gizmo_has_no_realtime_transport_shim():
    root = Path(__file__).resolve().parents[2]
    assert not (root / 'src/systems/ui/cef/cef_realtime_bridge.cpp').exists()
    source = (root / 'src/systems/ui/cef/cef_viewport_gizmo.cpp').read_text(encoding='utf-8-sig')
    assert 'handle_realtime_process_message' not in source
    assert 'forward_process_message_to_router' not in source
    client = (root / 'src/systems/ui/cef/cef_client.cpp').read_text(encoding='utf-8-sig')
    renderer = (root / 'src/systems/ui/cef/cef_renderer_bridge.cpp').read_text(encoding='utf-8-sig')
    assert 'OnProcessMessageReceived' not in client
    assert 'OnProcessMessageReceived' not in renderer
    assert 'RendererMessage' not in client
