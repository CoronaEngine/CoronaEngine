"""The Web surface ``window.corona`` publishes, as the contract document renders it.

This used to be an enforced pin: ``test_typed_native_surface.py`` compared it name-for-name
with ``editor_operation_table.h``. That comparison is gone - it made every rename a two-file
edit and could only say "the sets differ". The table is the contract now; this list is what the
generator draws the document's method table from, and drift between the two is a documentation
bug, not a build failure.

The spelling is the row's own: snake_case, the same on the wire, in the table, and in the
JS facade. There is no derivation left to drift.
"""
EXPECTED_SURFACE = {
    "app": {"close_process", "is_vision_available"},
    # One verb per action; the identity names a resource or an actor of a scene.
    "audio": {"play", "stop"},
    "bridge": {"call"},
    "input": {"dispatch"},
    "network": {
        "clear_discovered_peers", "connect_to_peer", "get_discovered_peers", "get_session_info",
        "search_lan", "set_project_root", "start_session", "stop_session",
    },
    "project": {
        "create_multiplayer_project", "create_world_project",
        "get_project_load_status", "get_recent_projects", "open_document", "open_project",
        "open_project_file", "save_active_project_info",
    },
    "scene": {
        "create_actor", "create_camera", "delete_actor", "delete_camera", "focus_actor",
        "import_resource_file", "read_active", "read_actor_properties", "read_actor_rig",
        "read_camera_render_settings", "read_environment", "read_model_nodes", "read_scene",
        "read_transforms", "rebind_actor_resource", "save", "select_actor", "select_model_file",
        "set_actor_ik_chains", "set_actor_transform", "set_camera_render_settings",
        "set_environment", "set_model_node_state", "update_actor", "update_camera",
    },
    "viewport": {"capture_screenshot", "configure"},
    "window": {
        "close", "create_panel", "focus",
        "on_close_requested", "on_closed", "set_window_presentation",
    },
}
