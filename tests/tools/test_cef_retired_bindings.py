"""Keep retired duplicate native bindings out of both sides of the bridge."""
from pathlib import Path


def test_unused_resource_search_is_not_exposed_to_web_but_keeps_python_contracts():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for method in ['fuzzy_search', 'get_stats', 'image_search', 'list_types',
                   'mark_index_dirty', 'prepare_index', 'rebuild_index']:
        line = next(line for line in manifest.splitlines() if f'ResourceSearch, {method},' in line)
        assert 'WRAPPED_CALLERS(ResourceSearch,' in line
        assert 'all_callers() & ~caller_mask(EditorApiCaller::Cef)' in line
        assert ', "", "resource_search.' in line
    owner = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert 'register_script_module("ResourceSearch", resource_search_methods)' in owner


def test_file_browser_retirement_preserves_non_web_permissions():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for method in ['get_project_info', 'get_files', 'get_file_tree', 'create_file',
                   'create_folder', 'rename_item', 'delete_item', 'open_file']:
        line = next(line for line in manifest.splitlines() if f'FileManager, {method},' in line)
        assert 'WRAPPED_CALLERS(FileManager,' in line
        assert 'all_callers() & ~caller_mask(EditorApiCaller::Cef)' in line
        assert ', "", "files.' in line
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert "call_manifest_editor_api('files." not in facade
    assert 'callProjectFileMutation' not in facade
    assert 'getProjectInfo:' not in facade and 'getFiles:' not in facade


def test_project_files_have_one_named_web_contract_and_no_generic_manifest_owner():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    assert 'WRAPPED_CALLERS(ProjectFiles,' not in manifest
    owner = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert 'project_file_methods' not in owner
    assert 'register_script_module("ProjectFiles"' not in owner
    operation = owner.split('ProjectFilesReply execute_project_files_operation(', 1)[1].split(
        'ScriptsReply execute_scripts_operation(', 1)[0]
    assert 'invoke_python_script_service(' in operation
    assert '"identity", request.identity' in operation
    assert '.dump(' not in operation and 'json::parse' not in operation
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert "call_manifest_editor_api('projectFiles." not in facade
    target = (root / 'CoronaEditor/editor/Frontend/src/blockly/targetArchive.js').read_text(encoding='utf-8-sig')
    assert 'JSON.' not in target and 'response?.data' not in target


def test_scripts_retire_cef_only_and_use_owned_native_contracts():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['run_path', 'run_file', 'get_run', 'stop', 'get_status']:
        line = next(line for line in manifest.splitlines() if f'Scripts, {name},' in line)
        assert ', "", "", ' in line
        assert line.endswith('caller_mask(EditorApiCaller::Native)),')
    header = (root / 'src/systems/ui/cef/cef_scripts_operations.h').read_text(encoding='utf-8-sig')
    for forbidden in ['nlohmann', 'CefV8', 'payload_json']:
        assert forbidden not in header
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert "call_manifest_editor_api('scripts." not in facade
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert native.count('Script path must be a project-relative .py file') == 1
    assert native.count('start_native_script_file(') == 3


def test_native_python_service_transport_uses_owned_values_without_json_text():
    root = Path(__file__).resolve().parents[2]
    owner = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    invoke = owner.split('NativeResult invoke_python_script_service(', 1)[1]
    assert 'service_args' in invoke
    assert 'service_result' in invoke
    assert 'payload_json' not in invoke
    assert 'json::parse' not in invoke
    assert 'python_script_request_json' not in owner
    runtime = (root / 'src/systems/script/python/python_api.cpp').read_text(encoding='utf-8-sig')
    service = runtime.split('if (request.kind == PythonRuntimeRequestKind::ServiceCall)', 1)[1].split(
        'if (request.kind == PythonRuntimeRequestKind::Callback', 1)[0]
    assert 'dispatch_script_values_from_cpp' in service
    assert 'payload_json' not in service


def test_actor_creation_retires_only_cef_and_shares_native_business_operation():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, create_actor,' in line)
    assert ', "", ' in declaration
    assert 'non_cef_animation_callers()' in declaration
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    typed = native.split('if (request.method == SceneMethod::CreateActor)', 1)[1].split('if (request.method == SceneMethod::CreateCamera)', 1)[0]
    assert 'create_native_editor_actor(' in typed
    assert 'ReadActorProperties' in typed
    legacy = native.split('{"create_actor",', 1)[1].split('{"rename_actor",', 1)[0]
    assert 'create_native_editor_actor(' in legacy
    assert 'context.script_runtime' in legacy


def test_audio_and_resource_import_have_one_native_business_implementation():
    root = Path(__file__).resolve().parents[2]
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert native.count('event_bus->publish<::Corona::Events::PlayAudioEvent>') == 1
    assert native.count('event_bus->publish<::Corona::Events::StopAudioEvent>') == 1
    assert native.count('actor.acoustics->play(') == 1
    assert native.count('actor.acoustics->stop(') == 1
    legacy_import = native.split('{"import_resource_file",', 1)[1].split('{"on_init",', 1)[0]
    assert 'import_native_resource(' in legacy_import
    assert 'select_resource_file_native(' not in legacy_import
    assert 'Corona::API::import_media(' not in legacy_import


def test_typed_resource_import_preserves_audio_metadata():
    root = Path(__file__).resolve().parents[2]
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    projection = binding.split('resource_import_value(', 1)[1].split('bool read_actor_context(', 1)[0]
    assert '"sample_rate"' in projection
    assert '"channels"' in projection


def test_audio_and_import_keep_original_python_permissions_after_cef_retirement():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for module, method in [('MainView', 'import_resource_file'), ('SceneTools', 'play_audio'),
                           ('SceneTools', 'stop_audio'), ('SceneTools', 'actor_play_audio'),
                           ('SceneTools', 'actor_stop_audio')]:
        declaration = next(line for line in manifest.splitlines() if f'{module}, {method},' in line)
        assert 'WRAPPED_CALLERS' in declaration
        assert ', "", ' in declaration
        assert declaration.endswith('caller_mask(EditorApiCaller::PythonScript)),')


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
    assert 'caller_mask(EditorApiCaller::PythonScript)' in declaration
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


def test_unused_animation_and_reload_routes_are_not_exposed_to_web():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['set_actor_animation', 'pause_actor_animation', 'resume_actor_animation', 'reload_scene']:
        declaration = next(line for line in manifest.splitlines() if f'SceneTools, {name},' in line)
        assert 'WRAPPED_CALLERS' in declaration
        assert 'animation_callers() & ~caller_mask(EditorApiCaller::Cef)' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    for name in ['setActorAnimation:', 'pauseActorAnimation:', 'resumeActorAnimation:', 'reloadScene:']:
        assert name not in facade


def test_unused_render_read_routes_are_not_exposed_to_web():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['get_output_mode', 'get_vision_render_mode', 'get_vision_denoise',
                 'get_vision_accumulation', 'get_vision_stable_planes']:
        declaration = next(line for line in manifest.splitlines() if f'SceneTools, {name},' in line)
        assert 'WRAPPED_CALLERS' in declaration
        assert 'animation_callers() & ~caller_mask(EditorApiCaller::Cef)' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    for name in ['getOutputMode:', 'getVisionRenderMode:', 'getVisionDenoise:',
                 'getVisionAccumulation:', 'getVisionStablePlanes:']:
        assert name not in facade


def test_vision_availability_uses_typed_scene_binding():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines()
                       if 'SceneTools, is_vision_available,' in line)
    assert 'WRAPPED_CALLERS' in declaration
    assert 'caller_mask(EditorApiCaller::PythonScript)' in declaration
    assert '"sceneTools.isVisionAvailable"' not in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'sceneTools:' not in facade
    assert 'isVisionAvailable: () => window.corona.scene.isVisionAvailable()' in facade
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'SceneMethod::ReadVisionAvailability' in binding


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


def test_camera_rename_and_delete_use_scoped_scene_v8_only_for_web():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['delete_camera', 'rename_camera_view']:
        declaration = next(line for line in manifest.splitlines() if f'SceneTools, {name},' in line)
        assert 'WRAPPED_CALLERS' in declaration
        assert 'caller_mask(EditorApiCaller::PythonScript)' in declaration
        assert 'caller_mask(EditorApiCaller::Cef)' not in declaration
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    for name in ['renameCamera', 'deleteCamera']:
        assert f'CreateFunction("{name}"' in binding
    assert 'SceneMethod::RenameCamera' in binding
    assert 'SceneMethod::DeleteCamera' in binding


def test_camera_v8_properties_cannot_write_window_lifecycle():
    root = Path(__file__).resolve().parents[2]
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    parser = binding.split('bool read_camera_properties(')[1].split('return vector(')[0]
    assert '"view_open"' not in parser


def test_actor_rename_uses_scoped_scene_v8_only_for_web():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, rename_actor,' in line)
    assert 'WRAPPED_CALLERS' in declaration
    assert 'animation_callers() & ~caller_mask(EditorApiCaller::Cef)' in declaration
    assert '"", "scene_tools.rename_actor"' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'call_manifest_editor_api(\'sceneTools.renameActor\'' not in facade
    assert 'renameActor: (sceneName, actor, name, scope) =>' in facade
    assert 'window.corona.scene.renameActor(sceneName, actor, name, scope)' in facade
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'new NativeHandler(endpoint, SceneMethod::RenameActor)' in binding
    assert 'actor_rename_value(reply.actor_rename)' in binding


def test_actor_delete_uses_scoped_scene_v8_and_retires_manifest_for_web():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines() if 'SceneTools, remove_actor,' in line)
    assert 'WRAPPED_CALLERS' in declaration
    assert 'caller_mask(EditorApiCaller::Cef)' in declaration
    assert '"", "scene_tools.remove_actor"' in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'removeActor:' not in facade
    assert 'window.corona.scene.deleteActor(sceneName, actor, scope)' in facade
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'new NativeHandler(endpoint, SceneMethod::DeleteActor)' in binding
    assert 'actor_delete_value(reply.actor_delete)' in binding
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    deletion = native.split('if (request.method == SceneMethod::DeleteActor) {', 1)[1].split(
        'if (request.method == SceneMethod::ReadModelNodes', 1)[0]
    assert 'scene->read_only' in deletion
    assert 'value.actor_guid == patch.actor' in deletion
    assert 'erase_native_editor_actor(*scene, actor)' in deletion
    assert 'selection_cleared' in native


def test_actor_resource_rebinding_uses_scoped_scene_v8_and_retires_web_manifest():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in manifest.splitlines()
                       if 'SceneTools, rebind_actor_resource,' in line)
    assert 'WRAPPED_CALLERS' in declaration
    assert '"", "scene_tools.rebind_actor_resource"' in declaration
    assert '~caller_mask(EditorApiCaller::Cef)' in declaration

    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'sceneTools:' not in facade
    assert 'window.corona.scene.rebindActorResource(sceneName, actorGuid, path, scope)' in facade

    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'SceneMethod::RebindActorResource' in binding
    assert 'actor_resource_rebind_value(reply.actor_resource_rebind)' in binding

    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    operation = native.split('if (request.method == SceneMethod::RebindActorResource) {', 1)[1].split(
        'if (request.method == SceneMethod::FocusActor)', 1)[0]
    assert 'scene->read_only' in operation
    assert 'patch.actor_guid' in operation and 'patch.path' in operation
    assert 'replacement_data["actor_guid"] = patch.actor_guid' in operation
    assert 'replacement_data["name"] = actor->name' in operation
    assert 'replacement_data["transform"] = {' in operation
    assert 'loaded.geometry->set_position(preserved_position)' in operation
    assert 'loaded.mechanics->set_body_type(preserved_body_type)' in operation
    assert 'SceneMethod::ReadActorProperties' in operation


def test_scene_save_has_one_scoped_v8_entry_and_retires_duplicate_web_facades():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    for name in ['save_actor', 'scene_save']:
        declaration = next(line for line in manifest.splitlines() if f', {name},' in line)
        assert 'WRAPPED' in declaration
        assert 'caller_mask(EditorApiCaller::Cef)' not in declaration
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'save: (sceneName, scope) => window.corona.scene.save(sceneName, scope)' in facade
    assert 'saveActor:' not in facade
    assert 'sceneSave:' not in facade
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'SceneMethod::Save' in binding
    assert 'CreateFunction("save"' in binding


def test_open_actor_duplicate_web_facade_is_retired():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    assert 'sceneTools.openActor' not in manifest
    native = (root / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp').read_text(encoding='utf-8-sig')
    assert '"open_actor"' not in native
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'openActor:' not in facade
    scene_bar = (root / 'CoronaEditor/editor/Frontend/src/views/sidebar/SceneBar.vue').read_text(encoding='utf-8-sig')
    assert 'editorApi.sceneTools.openActor' not in scene_bar
    assert 'ControlObject' not in scene_bar


def test_model_file_selection_uses_scoped_v8_and_retires_manifest_route():
    root = Path(__file__).resolve().parents[2]
    manifest = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    assert 'sceneTools.selectModelFile' not in manifest
    binding = (root / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'SceneMethod::SelectModelFile' in binding
    assert 'CreateFunction("selectModelFile"' in binding
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert "window.corona.scene.selectModelFile(sceneName" in facade
    assert 'sceneTools:' not in facade
    assert "'SceneTools.select_model_file'" not in facade


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


def test_main_init_is_python_only_and_has_no_web_facade():
    root = Path(__file__).resolve().parents[2]
    source = (root / 'src/systems/ui/editor_api/cef_editor_api.cpp').read_text(encoding='utf-8-sig')
    declaration = next(line for line in source.splitlines() if 'MainView, on_init,' in line)
    assert 'WRAPPED_CALLERS' in declaration and ', "", ' in declaration
    assert declaration.endswith('caller_mask(EditorApiCaller::PythonScript)),')
    facade = (root / 'CoronaEditor/editor/Frontend/src/api/editorApi.js').read_text(encoding='utf-8-sig')
    assert 'onInit:' not in facade
