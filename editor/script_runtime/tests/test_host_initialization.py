from types import SimpleNamespace


def test_script_runtime_host_initializes_scripts_through_active_owner(monkeypatch, tmp_path):
    import script_runtime.engine.host as host_module
    import script_runtime.engine.scripts_manager as manager_module

    class FakeScriptsManager:
        def __init__(self):
            self.initialized = None

        def initialize_project(self, project_script, scene):
            self.initialized = (project_script, scene)

    scene = SimpleNamespace(
        name="Main",
        route="Scene/main.scene",
        script_path="",
        _actors=(),
    )

    class SceneApi:
        def list_routes(self):
            return {
                "scenes": [{"path": scene.route, "name": scene.name}],
                "active_scene": scene.route,
            }

        def switch(self, route):
            raise RuntimeError("script startup must not reload the active scene")

        def get_snapshot(self, route):
            assert route == scene.route
            return {
                "scene": scene.route,
                "scene_name": scene.name,
                "script": scene.script_path,
                "actors": [],
            }

    monkeypatch.setattr(
        host_module,
        "get_script_runtime_editor_api",
        lambda: SimpleNamespace(scene=SceneApi()),
    )
    monkeypatch.setattr(host_module, "scene_target_from_snapshot", lambda data: scene)
    monkeypatch.setattr(manager_module, "ScriptsManager", FakeScriptsManager)

    runtime_host = SimpleNamespace(scripts_mgr=None, _scripts_initialized=False)
    host_module.initialize_scripts(runtime_host, str(tmp_path))

    assert isinstance(runtime_host.scripts_mgr, FakeScriptsManager)
    assert runtime_host.scripts_mgr.initialized == (
        str(tmp_path / "Scripts" / "project_script.py"),
        scene,
    )
    assert runtime_host._scripts_initialized is True


def test_script_runtime_host_selects_first_scene_when_no_route_is_active(monkeypatch):
    import script_runtime.engine.host as host_module

    calls = []
    snapshot = {"scene": "Scene/main.scene", "actors": []}

    class SceneApi:
        def list_routes(self):
            return {"scenes": [{"path": snapshot["scene"]}], "active_scene": ""}

        def switch(self, route):
            calls.append(("switch", route))
            return {"status": "success", "scene": route}

        def get_snapshot(self, route):
            calls.append(("snapshot", route))
            return snapshot

    monkeypatch.setattr(
        host_module,
        "get_script_runtime_editor_api",
        lambda: SimpleNamespace(scene=SceneApi()),
    )

    target = host_module._native_scene_target()

    assert target.route == snapshot["scene"]
    assert calls == [("switch", snapshot["scene"]), ("snapshot", snapshot["scene"])]


def test_script_runtime_host_skips_initialization_when_native_scene_is_unavailable(
    monkeypatch, tmp_path
):
    import script_runtime.engine.host as host_module

    monkeypatch.setattr(
        host_module,
        "get_script_runtime_editor_api",
        lambda: (_ for _ in ()).throw(RuntimeError("native scene unavailable")),
    )

    runtime_host = SimpleNamespace(scripts_mgr=None, _scripts_initialized=False)
    host_module.initialize_scripts(runtime_host, str(tmp_path))

    assert runtime_host.scripts_mgr is None
    assert runtime_host._scripts_initialized is False
