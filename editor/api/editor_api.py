import json

from .lan_chat_adapters import _LanChatQueueAdapter, _LanChatTransportAdapter
from script_runtime.manifest_adapter import ScriptRuntimeEditorApi
from script_runtime.native_engine_adapter import get_script_runtime_adapter
from runtime.project_context import (
    _resolve_native_engine,
    get_active_project_path,
    is_native_engine_available,
    set_active_project_path,
)
from runtime.editor_host import (
    emit_editor_event,
    get_editor_selection,
    set_editor_camera_input_enabled,
)
_CPP_EDITOR_API_METHODS = None
_CPP_EDITOR_API_EVENTS = None
_SCRIPT_RUNTIME_API_METHODS = None
_CPP_EDITOR_API_CALLER_PYTHON_SCRIPT = 2
_CPP_EDITOR_API_CALLER_SCRIPT_RUNTIME = 4


def _validate_cpp_api_caller(name, spec, caller_mask, caller_name):
    allowed_callers = spec.get("allowed_callers", 0) if isinstance(spec, dict) else 0
    if (int(allowed_callers) & caller_mask) == 0:
        raise RuntimeError(
            f"Editor API caller is not allowed by C++ manifest: {caller_name} cannot call {name}"
        )


def _ensure_cpp_api_methods():
    global _CPP_EDITOR_API_METHODS
    if _CPP_EDITOR_API_METHODS is None:
        manifest = _invoke_cpp_api("EditorApi.list_methods", [], validate_method=False)
        methods = manifest.get("methods", []) if isinstance(manifest, dict) else []
        _CPP_EDITOR_API_METHODS = {
            method.get("api"): method
            for method in methods
            if isinstance(method, dict) and isinstance(method.get("api"), str)
        }
    return _CPP_EDITOR_API_METHODS


def _ensure_cpp_api_method(api_name):
    methods = _ensure_cpp_api_methods()
    if api_name not in _CPP_EDITOR_API_METHODS:
        raise RuntimeError(f"Editor API method is not defined by C++ manifest: {api_name}")
    return methods[api_name]


def _ensure_script_runtime_api_methods():
    global _SCRIPT_RUNTIME_API_METHODS
    if _SCRIPT_RUNTIME_API_METHODS is None:
        manifest = _invoke_script_cpp_api("EditorApi.list_methods", [], validate_method=False)
        methods = manifest.get("methods", []) if isinstance(manifest, dict) else []
        _SCRIPT_RUNTIME_API_METHODS = {
            method.get("api"): method
            for method in methods
            if isinstance(method, dict) and isinstance(method.get("api"), str)
        }
    return _SCRIPT_RUNTIME_API_METHODS


def _ensure_script_runtime_api_method(api_name):
    methods = _ensure_script_runtime_api_methods()
    if api_name not in methods:
        raise RuntimeError(f"Editor API method is not defined by C++ manifest: {api_name}")
    return methods[api_name]


def _ensure_cpp_api_events():
    global _CPP_EDITOR_API_EVENTS
    if _CPP_EDITOR_API_EVENTS is None:
        manifest = _invoke_cpp_api("EditorApi.list_events", [], validate_method=False)
        events = manifest.get("events", []) if isinstance(manifest, dict) else []
        _CPP_EDITOR_API_EVENTS = {
            event.get("event"): event
            for event in events
            if isinstance(event, dict) and isinstance(event.get("event"), str)
        }
    return _CPP_EDITOR_API_EVENTS


def _ensure_cpp_api_event(event_name):
    events = _ensure_cpp_api_events()
    if event_name not in events:
        raise RuntimeError(f"Editor API event is not defined by C++ manifest: {event_name}")
    event_spec = events[event_name]
    _validate_cpp_api_caller(event_name, event_spec, _CPP_EDITOR_API_CALLER_PYTHON_SCRIPT, "PythonScript")
    return event_spec


def _find_cpp_api_method_by_python_wrapper(wrapper_path):
    for spec in _ensure_cpp_api_methods().values():
        if spec.get("python_wrapper") == wrapper_path:
            return spec
    return None


def _find_cpp_api_event_by_python_wrapper(wrapper_path):
    for event_spec in _ensure_cpp_api_events().values():
        if event_spec.get("python_wrapper") == wrapper_path:
            return event_spec
    return None


def _cpp_value_matches_type(value, value_type):
    if value_type == "any":
        return True
    if value_type == "null":
        return value is None
    if value_type == "boolean":
        return isinstance(value, bool)
    if value_type == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if value_type == "number":
        return (isinstance(value, (int, float)) and not isinstance(value, bool))
    if value_type == "string":
        return isinstance(value, str)
    if value_type == "object":
        return isinstance(value, dict)
    if value_type == "array":
        return isinstance(value, list)
    return False


def _validate_cpp_api_args(
    api_name,
    args,
    caller_mask=_CPP_EDITOR_API_CALLER_PYTHON_SCRIPT,
    caller_name="PythonScript",
    method_getter=None,
):
    spec = (method_getter or _ensure_cpp_api_method)(api_name)
    if spec is None:
        return None
    _validate_cpp_api_caller(api_name, spec, caller_mask, caller_name)
    if not isinstance(args, list):
        raise RuntimeError(f"Editor API argument schema mismatch for {api_name}: args must be an array")
    params = spec.get("params", []) if isinstance(spec, dict) else []
    if len(args) > len(params):
        raise RuntimeError(f"Editor API argument schema mismatch for {api_name}: too many arguments")
    for index, param in enumerate(params):
        value_missing = index >= len(args)
        value = None if value_missing else args[index]
        if value_missing or value is None:
            if param.get("optional"):
                continue
            raise RuntimeError(
                f"Editor API argument schema mismatch for {api_name}: missing {param.get('name', index)}"
            )
        if not _cpp_value_matches_type(value, param.get("type")):
            raise RuntimeError(
                f"Editor API argument schema mismatch for {api_name}: "
                f"{param.get('name', index)} must be {param.get('type')}"
            )
    return spec


def _validate_cpp_api_return(api_name, data, spec):
    if not isinstance(spec, dict):
        return
    return_type = spec.get("return", "any")
    if not _cpp_value_matches_type(data, return_type):
        raise RuntimeError(
            f"Editor API return schema mismatch for {api_name}: data must be {return_type}"
        )


def _validate_cpp_api_event_payload(event_name, payload, event_spec):
    if not isinstance(event_spec, dict):
        return
    payload_type = event_spec.get("payload", "any")
    if not _cpp_value_matches_type(payload, payload_type):
        raise RuntimeError(
            f"Editor API event payload schema mismatch for {event_name}: payload must be {payload_type}"
        )


def _invoke_cpp_api(api_name, args=None, validate_method=True):
    """Invoke a C++ defined editor API method through the CoronaEngine binding."""
    normalized_args = args or []
    spec = None
    if validate_method:
        spec = _validate_cpp_api_args(api_name, normalized_args)
    import CoronaEngine

    payload = json.dumps(normalized_args, ensure_ascii=False)
    response_text = CoronaEngine._invoke_cpp_editor_api(api_name, payload)
    try:
        response = json.loads(response_text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid C++ Editor API response: {response_text}") from exc
    if not response.get("success", False):
        raise RuntimeError(response.get("error") or f"C++ Editor API failed: {api_name}")
    data = response.get("data")
    _validate_cpp_api_return(api_name, data, spec)
    return data


def _invoke_typed_cpp_api(api_name, wrapper_path, args=None):
    spec = _ensure_cpp_api_method(api_name)
    if spec.get("python_wrapper") != wrapper_path:
        raise RuntimeError(f"Python wrapper path is not defined by C++ manifest: {wrapper_path}")
    return _invoke_cpp_api(api_name, args)


def _invoke_manifest_cpp_api(wrapper_path, args=None):
    spec = _find_cpp_api_method_by_python_wrapper(wrapper_path)
    if spec is None:
        raise RuntimeError(f"Python wrapper path is not defined by C++ manifest: {wrapper_path}")
    return _invoke_typed_cpp_api(spec.get("api"), wrapper_path, args or [])


def _invoke_script_cpp_api(api_name, args=None, validate_method=True):
    """Invoke the restricted Script Runtime API through its C++ channel."""
    normalized_args = args or []
    spec = None
    if validate_method:
        spec = _validate_cpp_api_args(
            api_name,
            normalized_args,
            _CPP_EDITOR_API_CALLER_SCRIPT_RUNTIME,
            "ScriptRuntime",
            _ensure_script_runtime_api_method,
        )
    import CoronaEngine

    payload = json.dumps(normalized_args, ensure_ascii=False)
    invoker = getattr(CoronaEngine, "_invoke_cpp_script_api", None)
    if not callable(invoker):
        raise RuntimeError("Script Runtime API channel is unavailable")
    response_text = invoker(api_name, payload)
    try:
        response = json.loads(response_text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"Invalid C++ Script Runtime API response: {response_text}") from exc
    if not response.get("success", False):
        raise RuntimeError(response.get("error") or f"C++ Script Runtime API failed: {api_name}")
    data = response.get("data")
    _validate_cpp_api_return(api_name, data, spec)
    return data


def _invoke_script_manifest_cpp_api(wrapper_path, args=None):
    spec = next(
        (
            candidate
            for candidate in _ensure_script_runtime_api_methods().values()
            if candidate.get("python_wrapper") == wrapper_path
        ),
        None,
    )
    if spec is None:
        raise RuntimeError(f"Python wrapper path is not defined by C++ manifest: {wrapper_path}")
    return _invoke_script_cpp_api(spec.get("api"), args or [])


def get_script_runtime_editor_api():
    """Return the manifest adapter restricted to the ScriptRuntime caller."""
    return ScriptRuntimeEditorApi(_invoke_script_manifest_cpp_api)


def _register_editor_api_event_callback(event_name, wrapper_name, callback):
    event_spec = _ensure_cpp_api_event(event_name)
    if event_spec.get("python_wrapper") != wrapper_name:
        raise RuntimeError(f"Editor API event wrapper is not defined by C++ manifest: {wrapper_name}")
    import CoronaEngine

    def _dispatch(payload_json, event):
        payload = json.loads(payload_json) if isinstance(payload_json, str) else payload_json
        _validate_cpp_api_event_payload(event_name, payload, event_spec)
        return callback(payload, event)

    return CoronaEngine.register_python_script_callback(event_name, _dispatch)


def _register_manifest_editor_api_event_callback(wrapper_name, callback):
    event_spec = _find_cpp_api_event_by_python_wrapper(wrapper_name)
    if event_spec is None:
        raise RuntimeError(f"Editor API event wrapper is not defined by C++ manifest: {wrapper_name}")
    return _register_editor_api_event_callback(event_spec.get("event"), wrapper_name, callback)


def _emit_cpp_editor_api_event(event_name, payload):
    event_spec = _ensure_cpp_api_event(event_name)
    _validate_cpp_api_event_payload(event_name, payload, event_spec)
    import CoronaEngine

    return CoronaEngine.emit_editor_api_event(event_name, json.dumps(payload))


def _emit_manifest_cpp_editor_api_event(wrapper_name, payload):
    event_spec = _find_cpp_api_event_by_python_wrapper(wrapper_name)
    if event_spec is None:
        raise RuntimeError(f"Editor API event wrapper is not defined by C++ manifest: {wrapper_name}")
    return _emit_cpp_editor_api_event(event_spec.get("event"), payload)


class _DynamicApiNamespace:
    def __init__(self, wrapper_path):
        self._wrapper_path = wrapper_path

    def __getattr__(self, name):
        wrapper_path = f"{self._wrapper_path}.{name}"
        method_spec = _find_cpp_api_method_by_python_wrapper(wrapper_path)
        if method_spec is not None:
            def _method(*args):
                return _invoke_manifest_cpp_api(wrapper_path, list(args))

            setattr(self, name, _method)
            return _method

        event_spec = _find_cpp_api_event_by_python_wrapper(wrapper_path)
        if event_spec is not None:
            def _on_event(callback):
                return _register_manifest_editor_api_event_callback(wrapper_path, callback)

            setattr(self, name, _on_event)
            return _on_event

        namespace = _DynamicApiNamespace(wrapper_path)
        setattr(self, name, namespace)
        return namespace


class _CoronaEditorApiMeta(type):
    def __getattr__(cls, name):
        return _DynamicApiNamespace(name)


class _ProjectApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("project")

    @staticmethod
    def get_app_version():
        return _invoke_manifest_cpp_api("project.get_app_version", [])

    @staticmethod
    def get_default_project_path():
        return _invoke_manifest_cpp_api("project.get_default_project_path", [])

    @staticmethod
    def get_recent_projects():
        return _invoke_manifest_cpp_api("project.get_recent_projects", [])

    @staticmethod
    def get_project_load_status():
        return _invoke_manifest_cpp_api("project.get_project_load_status", [])

    @staticmethod
    def create_project(project_data):
        return _invoke_manifest_cpp_api("project.create_project", [project_data or {}])

    @staticmethod
    def create_world_project(world_data):
        return _invoke_manifest_cpp_api(
            "project.create_world_project", [world_data or {}]
        )

    @staticmethod
    def create_multiplayer_project(project_data):
        return _invoke_manifest_cpp_api(
            "project.create_multiplayer_project", [project_data or {}]
        )

    @staticmethod
    def copy_existing_to_data(payload):
        return _invoke_manifest_cpp_api(
            "project.copy_existing_to_data", [payload or {}]
        )

    @staticmethod
    def open_project(project_path, options=None):
        return _invoke_manifest_cpp_api(
            "project.open_project", [project_path, options or {}]
        )

    @staticmethod
    def open_project_file():
        return _invoke_manifest_cpp_api("project.open_project_file", [])

    @staticmethod
    def set_project_mode(mode_data):
        return _invoke_manifest_cpp_api("project.set_project_mode", [mode_data or {}])


class _FilesApi(_DynamicApiNamespace):
    """Manifest-backed file/project resource operations."""

    def __init__(self):
        super().__init__("files")


    @staticmethod
    def get_project_info():
        return _invoke_manifest_cpp_api("files.get_project_info", [])

    @staticmethod
    def get_files(relative_path=""):
        return _invoke_manifest_cpp_api("files.get_files", [relative_path or ""])

    @staticmethod
    def get_file_tree(relative_path=""):
        return _invoke_manifest_cpp_api("files.get_file_tree", [relative_path or ""])

    @staticmethod
    def create_folder(path, folder_name):
        return _invoke_manifest_cpp_api("files.create_folder", [path, folder_name])

    @staticmethod
    def create_file(path, file_name, file_type):
        return _invoke_manifest_cpp_api(
            "files.create_file", [path, file_name, file_type]
        )

    @staticmethod
    def delete_item(path):
        return _invoke_manifest_cpp_api("files.delete_item", [path])

    @staticmethod
    def rename_item(old_path, new_name):
        return _invoke_manifest_cpp_api("files.rename_item", [old_path, new_name])

    @staticmethod
    def open_file(path, file_type):
        return _invoke_manifest_cpp_api("files.open_file", [path, file_type])


class _ProjectSettingsApi(_DynamicApiNamespace):
    """Manifest-backed active-project metadata operations."""

    def __init__(self):
        super().__init__("project_settings")

    @staticmethod
    def get_active_project_info():
        return _invoke_manifest_cpp_api("project_settings.get_active_project_info", [])

    @staticmethod
    def save_active_project_info(settings):
        return _invoke_manifest_cpp_api(
            "project_settings.save_active_project_info", [settings or {}]
        )

    @staticmethod
    def browse_scene_file():
        return _invoke_manifest_cpp_api("project_settings.browse_scene_file", [])


class _EditorApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("editor")

    @staticmethod
    def list_methods():
        return _invoke_typed_cpp_api("EditorApi.list_methods", "editor.list_methods", [])

    @staticmethod
    def list_events():
        return _invoke_typed_cpp_api("EditorApi.list_events", "editor.list_events", [])

    @staticmethod
    def off(callback_token):
        return _invoke_typed_cpp_api("EditorApi.unregister_callback", "editor.unregister_callback", [callback_token])


class _EventsApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("events")

    @staticmethod
    def on_ai_chunk(callback):
        return _register_manifest_editor_api_event_callback("events.on_ai_chunk", callback)

    @staticmethod
    def on_log_batch(callback):
        return _register_manifest_editor_api_event_callback("events.on_log_batch", callback)

    @staticmethod
    def on_actor_changed(callback):
        return _register_manifest_editor_api_event_callback("events.on_actor_changed", callback)

    @staticmethod
    def on_actor_selection_changed(callback):
        return _register_manifest_editor_api_event_callback("events.on_actor_selection_changed", callback)

    @staticmethod
    def on_actor_transform_updated(callback):
        return _register_manifest_editor_api_event_callback("events.on_actor_transform_updated", callback)

    @staticmethod
    def on_actor_pick_result(callback):
        return _register_manifest_editor_api_event_callback("events.on_actor_pick_result", callback)

    @staticmethod
    def on_focus_pose_result(callback):
        return _register_manifest_editor_api_event_callback("events.on_focus_pose_result", callback)

    @staticmethod
    def on_scene_added(callback):
        return _register_manifest_editor_api_event_callback("events.on_scene_added", callback)

    @staticmethod
    def on_scene_renamed(callback):
        return _register_manifest_editor_api_event_callback("events.on_scene_renamed", callback)

    @staticmethod
    def on_project_opened(callback):
        return _register_manifest_editor_api_event_callback("events.on_project_opened", callback)

    @staticmethod
    def on_lan_chat_event(callback):
        return _register_manifest_editor_api_event_callback("events.on_lan_chat_event", callback)

    @staticmethod
    def on_network_actor_ownership_claimed(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_actor_ownership_claimed", callback)

    @staticmethod
    def on_network_actor_delete_sync_broadcast_requested(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_actor_delete_sync_broadcast_requested", callback)

    @staticmethod
    def on_network_actor_state_sync_broadcast_requested(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_actor_state_sync_broadcast_requested", callback)

    @staticmethod
    def on_network_actor_sync_broadcast_requested(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_actor_sync_broadcast_requested", callback)

    @staticmethod
    def on_network_actor_transform_sync_broadcast_requested(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_actor_transform_sync_broadcast_requested", callback)

    @staticmethod
    def on_network_asset_import_completed(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_asset_import_completed", callback)

    @staticmethod
    def on_network_file_sync_status_changed(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_file_sync_status_changed", callback)

    @staticmethod
    def on_network_sync_pause_requested(callback):
        return _register_manifest_editor_api_event_callback("events.on_network_sync_pause_requested", callback)


class _LanChatApi(_DynamicApiNamespace):
    """Manifest-backed LANChat aggregate for editor Python callers."""

    def __init__(self):
        super().__init__("lan_chat")

    @staticmethod
    def start_room(payload):
        return _invoke_manifest_cpp_api("lan_chat.start_room", [payload])

    @staticmethod
    def start_local_room(payload):
        return _invoke_manifest_cpp_api("lan_chat.start_local_room", [payload])

    @staticmethod
    def stop_room():
        return _invoke_manifest_cpp_api("lan_chat.stop_room", [])

    @staticmethod
    def stop_local_room():
        return _invoke_manifest_cpp_api("lan_chat.stop_local_room", [])

    @staticmethod
    def get_history():
        return _invoke_manifest_cpp_api("lan_chat.get_history", [])

    @staticmethod
    def list_history_rooms():
        return _invoke_manifest_cpp_api("lan_chat.list_history_rooms", [])

    @staticmethod
    def load_history_room(payload):
        return _invoke_manifest_cpp_api("lan_chat.load_history_room", [payload])

    @staticmethod
    def join_room(payload):
        return _invoke_manifest_cpp_api("lan_chat.join_room", [payload])

    @staticmethod
    def leave_room():
        return _invoke_manifest_cpp_api("lan_chat.leave_room", [])

    @staticmethod
    def send_message(payload):
        return _invoke_manifest_cpp_api("lan_chat.send_message", [payload])

    @staticmethod
    def send_agent_reply(payload):
        return _invoke_manifest_cpp_api("lan_chat.send_agent_reply", [payload])

    @staticmethod
    def send_system_message(payload):
        return _invoke_manifest_cpp_api("lan_chat.send_system_message", [payload])

    @staticmethod
    def send_system_message_to_host(payload):
        return _invoke_manifest_cpp_api(
            "lan_chat.send_system_message_to_host", [payload]
        )

    @staticmethod
    def send_system_message_to_user(payload):
        return _invoke_manifest_cpp_api(
            "lan_chat.send_system_message_to_user", [payload]
        )

    @staticmethod
    def poll_agent_trigger():
        return _invoke_manifest_cpp_api("lan_chat.poll_agent_trigger", [])

    @staticmethod
    def poll_coordinator_sync_message():
        return _invoke_manifest_cpp_api(
            "lan_chat.poll_coordinator_sync_message", []
        )

    @staticmethod
    def poll_room_event():
        return _invoke_manifest_cpp_api("lan_chat.poll_room_event", [])

    @staticmethod
    def poll_sync_event():
        return _invoke_manifest_cpp_api("lan_chat.poll_sync_event", [])

    @staticmethod
    def add_agent(payload):
        return _invoke_manifest_cpp_api("lan_chat.add_agent", [payload])

    @staticmethod
    def remove_agent(payload):
        return _invoke_manifest_cpp_api("lan_chat.remove_agent", [payload])

    @staticmethod
    def list_agents():
        return _invoke_manifest_cpp_api("lan_chat.list_agents", [])

    @staticmethod
    def get_local_ip():
        return _invoke_manifest_cpp_api("lan_chat.get_local_ip", [])


class _NetworkApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("network")

    @staticmethod
    def lock_object(object_id, user_id, operation="modify"):
        return _invoke_manifest_cpp_api(
            "network.lock_object", [object_id, user_id, operation]
        )

    @staticmethod
    def unlock_object(object_id, user_id):
        return _invoke_manifest_cpp_api(
            "network.unlock_object", [object_id, user_id]
        )

    @staticmethod
    def get_lock_owner(object_id):
        return _invoke_manifest_cpp_api("network.get_lock_owner", [object_id])

    @staticmethod
    def get_session_info():
        return _invoke_manifest_cpp_api("network.get_session_info", [])

    @staticmethod
    def broadcast_intent(user_id, tooltip, position, status="placing_object"):
        return _invoke_manifest_cpp_api(
            "network.broadcast_intent", [user_id, tooltip, position, status]
        )

    @staticmethod
    def check_preview_collision(user_id, position, delta=0.5):
        return _invoke_manifest_cpp_api(
            "network.check_preview_collision", [user_id, position, delta]
        )


def get_network_adapter(native_engine=None):
    """Return the manifest-backed public network contract."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        return CoronaEditorApi.network
    return None


def get_lan_chat_adapter(native_engine=None):
    """Return the public LANChat contract for an embedded native host."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        return CoronaEditorApi.lan_chat
    return None


def get_lan_chat_transport_adapter(native_engine=None):
    """Return the reliable LANChat transport adapter for an embedded host."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        return _LanChatTransportAdapter(CoronaEditorApi.lan_chat)
    return None


def get_lan_chat_queue_adapter(native_engine=None):
    """Return the LANChat queue adapter for an embedded host."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        return _LanChatQueueAdapter(CoronaEditorApi.lan_chat)
    return None


class _SceneToolsApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("scene_tools")

    @staticmethod
    def create_scene(scene_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.create_scene", [scene_name]
        )

    @staticmethod
    def reload_scene(scene_name, project_path=""):
        return _invoke_manifest_cpp_api(
            "scene_tools.reload_scene", [scene_name, project_path]
        )

    @staticmethod
    def create_actor(scene_name, source_path, actor_type, actor_data):
        return _invoke_manifest_cpp_api(
            "scene_tools.create_actor",
            [scene_name, source_path, actor_type, actor_data],
        )

    @staticmethod
    def remove_actor(scene_name, actor_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.remove_actor", [scene_name, actor_name]
        )

    @staticmethod
    def focus_actor(scene_name, actor_name, camera_name=""):
        return _invoke_manifest_cpp_api(
            "scene_tools.focus_actor", [scene_name, actor_name, camera_name]
        )

    @staticmethod
    def set_actor_state(scene_name, actor_name, state):
        """Update editor-owned actor state through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.set_actor_state", [scene_name, actor_name, state]
        )

    @staticmethod
    def save_actor(scene_name, actor_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.save_actor", [scene_name, actor_name]
        )

    @staticmethod
    def select_model_file(scene_name, actor_name, file_type="model"):
        return _invoke_manifest_cpp_api(
            "scene_tools.select_model_file", [scene_name, actor_name, file_type]
        )

    @staticmethod
    def set_actor_physics(scene_name, actor_name, physics):
        """Apply an actor physics value object through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.set_actor_physics", [scene_name, actor_name, physics]
        )

    @staticmethod
    def set_actor_camera_lock(scene_name, actor_name, camera_lock):
        """Set native camera-follow state through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.set_actor_camera_lock", [scene_name, actor_name, camera_lock]
        )

    @staticmethod
    def sun_direction(scene_name, enabled, direction):
        """Set the scene sun state through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.sun_direction", [scene_name, enabled, direction]
        )

    @staticmethod
    def floor_grid(scene_name, enabled):
        """Set the scene floor-grid state through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.floor_grid", [scene_name, enabled]
        )

    @staticmethod
    def set_physics_params(
        scene_name, gravity=None, floor_y=None, floor_restitution=None, fixed_dt=None
    ):
        """Set scene physics parameters through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.set_physics_params",
            [scene_name, gravity, floor_y, floor_restitution, fixed_dt],
        )

    @staticmethod
    def get_physics_params(scene_name):
        """Get scene physics parameters through the SceneTools contract."""
        return _invoke_manifest_cpp_api(
            "scene_tools.get_physics_params", [scene_name]
        )

    @staticmethod
    def set_render_backend(mode, scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.set_render_backend", [mode, scene_name, camera_name]
        )

    @staticmethod
    def get_render_backend(scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.get_render_backend", [scene_name, camera_name]
        )

    @staticmethod
    def set_vision_render_mode(scene_name, camera_name=None, mode=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.set_vision_render_mode", [scene_name, camera_name, mode]
        )

    @staticmethod
    def get_vision_render_mode(scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.get_vision_render_mode", [scene_name, camera_name]
        )

    @staticmethod
    def set_shadow_cascade_debug(scene_name, camera_name=None, enabled=False):
        return _invoke_manifest_cpp_api(
            "scene_tools.set_shadow_cascade_debug", [scene_name, camera_name, enabled]
        )

    @staticmethod
    def get_shadow_cascade_debug(scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.get_shadow_cascade_debug", [scene_name, camera_name]
        )

    @staticmethod
    def set_ssao_enabled(scene_name, camera_name=None, enabled=True):
        return _invoke_manifest_cpp_api(
            "scene_tools.set_ssao_enabled", [scene_name, camera_name, enabled]
        )

    @staticmethod
    def get_ssao_enabled(scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.get_ssao_enabled", [scene_name, camera_name]
        )

    @staticmethod
    def set_output_mode(scene_name, camera_name=None, mode="final_color"):
        return _invoke_manifest_cpp_api(
            "scene_tools.set_output_mode", [scene_name, camera_name, mode]
        )

    @staticmethod
    def get_output_mode(scene_name=None, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.get_output_mode", [scene_name, camera_name]
        )

    @staticmethod
    def create_camera_view(scene_name, name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.create_camera_view", [scene_name, name]
        )

    @staticmethod
    def open_camera_view(scene_name, camera_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.open_camera_view", [scene_name, camera_name]
        )

    @staticmethod
    def close_camera_view(scene_name, camera_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.close_camera_view", [scene_name, camera_name]
        )

    @staticmethod
    def rename_camera_view(scene_name, camera_name, name):
        return _invoke_manifest_cpp_api(
            "scene_tools.rename_camera_view", [scene_name, camera_name, name]
        )

    @staticmethod
    def list_camera_views(scene_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.list_camera_views", [scene_name]
        )

    @staticmethod
    def update_camera_view(scene_name, camera_name, state):
        return _invoke_manifest_cpp_api(
            "scene_tools.update_camera_view", [scene_name, camera_name, state]
        )

    @staticmethod
    def delete_camera(scene_name, camera_name):
        return _invoke_manifest_cpp_api(
            "scene_tools.delete_camera", [scene_name, camera_name]
        )

    @staticmethod
    def is_vision_available():
        return _invoke_manifest_cpp_api("scene_tools.is_vision_available", [])

    @staticmethod
    def load_vision_scene(path):
        return _invoke_manifest_cpp_api("scene_tools.load_vision_scene", [path])

    @staticmethod
    def save_screenshot(scene_name, path, camera_name=None):
        return _invoke_manifest_cpp_api(
            "scene_tools.save_screenshot", [scene_name, path, camera_name]
        )


def get_scene_tools_adapter(native_engine=None):
    """Return the manifest-backed SceneTools contract."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        try:
            manifest_spec = _find_cpp_api_method_by_python_wrapper(
                "scene_tools.create_actor"
            )
        except Exception:
            manifest_spec = None
        if manifest_spec is not None:
            return CoronaEditorApi.scene_tools
    return None


def get_scene_adapter(native_engine=None):
    """Return the manifest-backed scene value-object contract."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        try:
            manifest_spec = _find_cpp_api_method_by_python_wrapper("scene.get_snapshot")
        except Exception:
            manifest_spec = None
        if manifest_spec is not None:
            return CoronaEditorApi.scene
    return None


def get_viewport_adapter(native_engine=None):
    """Return the manifest-backed viewport contract."""
    native_engine = _resolve_native_engine(native_engine)
    if native_engine is None:
        return None
    if callable(getattr(native_engine, "_invoke_cpp_editor_api", None)):
        try:
            manifest_spec = _find_cpp_api_method_by_python_wrapper("viewport.capture")
        except Exception:
            manifest_spec = None
        if manifest_spec is not None:
            return CoronaEditorApi.viewport
    return None


class _SceneApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("scene")

    @staticmethod
    def list_actor_tree(scene_name):
        return _invoke_manifest_cpp_api("scene.list_actor_tree", [scene_name])

    @staticmethod
    def select_actor(scene_name, actor_type, actor_name):
        return _invoke_manifest_cpp_api("scene.select_actor", [scene_name, actor_type, actor_name])

    @staticmethod
    def get_snapshot(scene_name=""):
        """Return the authoritative scene value object from the C++ editor."""
        return _invoke_manifest_cpp_api("scene.get_snapshot", [scene_name])

    @staticmethod
    def set_actor_transform(scene_name, actor_name, transform):
        """Apply an actor transform without exposing a native Actor object."""
        return _invoke_manifest_cpp_api(
            "scene.set_actor_transform", [scene_name, actor_name, transform]
        )


class _ViewportApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("viewport")

    @staticmethod
    def capture(scene_name, camera_name, camera, output_path):
        """Capture a viewport value object through the editor aggregate API."""
        return _invoke_manifest_cpp_api(
            "viewport.capture", [scene_name, camera_name, camera, output_path]
        )

    @staticmethod
    def set_camera_pose(scene_name, camera_name, camera):
        """Update an editor camera using a value object, not a native Camera handle."""
        return _invoke_manifest_cpp_api(
            "viewport.set_camera_pose", [scene_name, camera_name, camera]
        )


class _MainApi(_DynamicApiNamespace):
    def __init__(self):
        super().__init__("main")

    @staticmethod
    def on_init(project_path=""):
        return _invoke_manifest_cpp_api("main.on_init", [project_path])

    @staticmethod
    def create_scene(scene_name):
        return _invoke_manifest_cpp_api("main.create_scene", [scene_name])

    @staticmethod
    def scene_save(scene_name, snapshot=None):
        args = [scene_name]
        if snapshot is not None:
            args.append(snapshot)
        return _invoke_manifest_cpp_api("main.scene_save", args)

    @staticmethod
    def remove_scene(scene_name):
        return _invoke_manifest_cpp_api("main.remove_scene", [scene_name])


class CoronaEditorApi(metaclass=_CoronaEditorApiMeta):
    editor = _EditorApi()
    events = _EventsApi()
    lan_chat = _LanChatApi()
    project = _ProjectApi()
    files = _FilesApi()
    project_settings = _ProjectSettingsApi()
    network = _NetworkApi()
    scene = _SceneApi()
    scene_tools = _SceneToolsApi()
    viewport = _ViewportApi()
    main = _MainApi()
