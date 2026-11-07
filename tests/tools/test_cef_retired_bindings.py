"""Keep retired duplicate native bindings out of both sides of the bridge."""
from pathlib import Path


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
