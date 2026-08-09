from types import SimpleNamespace
import pytest


def test_project_scene_routes_prefer_script_runtime_manifest(monkeypatch):
    import api.editor_api
    from script_runtime.engine import corona_engine

    class SceneApi:
        def list_routes(self):
            return {
                "status": "success",
                "scenes": [
                    {"path": "Scene/first.scene", "name": "First"},
                    {"path": "Scene/second.scene", "name": "Second"},
                ],
                "active_scene": "Scene/first.scene",
            }

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: SimpleNamespace(scene=SceneApi()),
    )

    assert corona_engine._project_scene_routes() == [
        "Scene/first.scene",
        "Scene/second.scene",
    ]


def test_native_snapshot_builds_restricted_scene_script_target():
    from script_runtime.manifest_adapter import scene_target_from_snapshot

    target = scene_target_from_snapshot(
        {
            "scene": "Scene/level.scene",
            "scene_name": "Level",
            "script": "Scripts/level.py",
            "actors": [
                {"name": "Hero", "script": "Scripts/hero.py"},
                {"name": "Light"},
            ],
        }
    )

    assert target.route == "Scene/level.scene"
    assert target.name == "Level"
    assert target.script_path == "Scripts/level.py"
    assert [actor.name for actor in target._actors] == ["Hero", "Light"]
    assert target._actors[0].script_path == "Scripts/hero.py"
    assert target._actors[1].script_path == ""


def test_set_scene_switches_native_route_before_legacy_scene_fallback(monkeypatch):
    import api.editor_api
    from script_runtime.engine import corona_engine

    calls = []

    class SceneApi:
        def switch(self, route):
            calls.append(route)
            return {"status": "success", "scene": route, "active_scene": route}

    native_scene = SimpleNamespace(route="Scene/second.scene")
    context = SimpleNamespace(
        scene_name="",
        target_scene_name="",
        scene=None,
        target_scene=None,
        actor=None,
        target_actor=None,
        target_type="project",
        initialized=False,
    )

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: SimpleNamespace(scene=SceneApi()),
    )
    monkeypatch.setattr(corona_engine, "_resolve_scene_route", lambda name: name)
    monkeypatch.setattr(corona_engine, "_runtime_scene", lambda: None)
    monkeypatch.setattr(
        corona_engine,
        "_current_context",
        lambda: context,
    )
    monkeypatch.setattr(
        corona_engine,
        "resolve_runtime_target",
        lambda target_type, scene_name, actor_name="", **kwargs: {
            "status": "ok",
            "scene_name": scene_name,
            "scene": native_scene,
        },
    )

    assert corona_engine.setScene("Scene/second.scene") is True
    assert calls == ["Scene/second.scene"]
    assert context.scene is native_scene
    assert context.scene_name == "Scene/second.scene"


def test_set_scene_does_not_implicitly_create_a_legacy_scene(monkeypatch):
    import api.editor_api
    from script_runtime.engine import corona_engine

    context = SimpleNamespace(
        scene_name="",
        target_scene_name="",
        scene=None,
        target_scene=None,
        actor=None,
        target_actor=None,
        target_type="project",
        initialized=False,
        isolated=False,
        stop_requested=False,
    )
    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: SimpleNamespace(
            scene=SimpleNamespace(
                switch=lambda route: {"status": "success", "scene": route}
            )
        ),
    )
    monkeypatch.setattr(corona_engine, "_current_context", lambda: context)
    monkeypatch.setattr(
        corona_engine,
        "_native_scene_snapshot",
        lambda scene_name: (None, ["native scene unavailable"]),
    )
    assert corona_engine.setScene("Scene/legacy.scene") is False


def test_project_scene_routes_do_not_fallback_to_legacy_scene_adapter():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[1] / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _project_scene_routes")
    end = source.index("def _resolve_scene_route", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_runtime_scene_does_not_reconstruct_a_legacy_python_scene():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[1] / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _runtime_scene")
    end = source.index("def _runtime_scene_key", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_known_actor_queries_do_not_fallback_to_legacy_scene_adapter():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[1] / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _iter_known_actors")
    end = source.index("def _deleted_names", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_actor_resolution_does_not_fallback_to_legacy_scene_adapter():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[1] / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _resolve_actor")
    end = source.index("def _current_actor", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_actor_runtime_target_prefers_native_snapshot_over_legacy_scene(monkeypatch):
    from types import SimpleNamespace

    from script_runtime.engine import corona_engine

    native_payload = {
        "scene": "Scene/level.scene",
        "scene_name": "Level",
        "actors": [{"name": "Hero", "handle": 7}],
    }

    monkeypatch.setattr(
        corona_engine,
        "_native_scene_snapshot",
        lambda scene_name: (native_payload, []),
    )

    result = corona_engine.resolve_runtime_target(
        "actor", scene_name="Scene/level.scene", actor_name="Hero"
    )

    assert result["status"] == "ok"
    assert result["binding_mode"] == "native_editor"
    assert result["actor"].name == "Hero"
    assert result["actor"].handle == 7


def test_runtime_target_does_not_implicitly_fallback_to_legacy_scene(monkeypatch):
    from script_runtime.engine import corona_engine

    monkeypatch.setattr(
        corona_engine,
        "_native_scene_snapshot",
        lambda scene_name: (None, ["native scene unavailable"]),
    )
    result = corona_engine.resolve_runtime_target(
        "project", scene_name="Scene/legacy.scene"
    )

    assert result["status"] == "error"
    assert result["binding_mode"] == ""
    assert "scene" not in result


def test_scene_environment_uses_the_script_runtime_scene_contract():
    from script_runtime.manifest_adapter import ScriptRuntimeSceneAdapter

    calls = []

    def invoke(method, args):
        calls.append((method, args))
        return {"status": "success", "scene": "Scene/level.scene"}

    scene = ScriptRuntimeSceneAdapter(invoke)
    state = {
        "sun": {"enabled": True, "direction": [1.0, 2.0, 3.0]},
        "grid": {"enabled": False},
        "physics": {"gravity": [0.0, -9.8, 0.0]},
    }

    scene.get_environment("Scene/level.scene")
    scene.set_environment("Scene/level.scene", state)

    assert calls == [
        ("scene.get_environment", ["Scene/level.scene"]),
        ("scene.set_environment", ["Scene/level.scene", state]),
    ]


def test_script_runtime_scene_tools_exposes_actor_state_and_physics_aggregates():
    from script_runtime.manifest_adapter import ScriptRuntimeEditorApi

    calls = []

    def invoke(method, args):
        calls.append((method, args))
        return {"status": "success"}

    api = ScriptRuntimeEditorApi(invoke)
    api.scene_tools.set_actor_state("Scene/level.scene", "Hero", {"visible": False})
    api.scene_tools.set_actor_physics(
        "Scene/level.scene", "Hero", {"physics_enabled": True}
    )

    assert calls == [
        ("scene_tools.set_actor_state", ["Scene/level.scene", "Hero", {"visible": False}]),
        ("scene_tools.set_actor_physics", ["Scene/level.scene", "Hero", {"physics_enabled": True}]),
    ]


def test_native_actor_proxy_uses_script_runtime_scene_tools_for_actor_operations(monkeypatch):
    import api.editor_api
    from script_runtime.engine import corona_engine

    calls = []

    class SceneToolsApi:
        def set_actor_state(self, *args):
            calls.append(("state", args))
            return {"status": "success"}

        def set_actor_physics(self, *args):
            calls.append(("physics", args))
            return {"status": "success"}

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: type("Api", (), {"scene_tools": SceneToolsApi()})(),
    )
    actor = corona_engine._NativeEditorActorProxy(
        type("Scene", (), {"route": "Scene/level.scene"})(),
        {"name": "Hero", "geometry": {}},
    )

    actor.set_visible(False)
    actor.set_physics_enabled(True)

    assert calls == [
        ("state", ("Scene/level.scene", "Hero", {"visible": False})),
        ("physics", ("Scene/level.scene", "Hero", {"physics_enabled": True})),
    ]


def test_blockly_preview_snapshot_prefers_native_scene_values(monkeypatch):
    import api.editor_api
    from script_runtime.blockly import main as blockly_main

    route = "Scene/level.scene"
    calls = []

    class SceneApi:
        def get_snapshot(self, scene_name):
            calls.append(("snapshot", scene_name))
            return {
                "status": "success",
                "scene": route,
                "name": "Level",
                "cameras": [{
                    "name": "MainCamera",
                    "position": [1.0, 2.0, 3.0],
                    "forward": [0.0, 0.0, 1.0],
                    "world_up": [0.0, 1.0, 0.0],
                    "fov": 45.0,
                }],
                "actors": [{
                    "name": "Hero",
                    "geometry": {
                        "position": [4.0, 5.0, 6.0],
                        "rotation": [0.0, 1.0, 0.0],
                        "scale": [1.0, 1.0, 1.0],
                    },
                }],
            }

        def get_environment(self, scene_name):
            calls.append(("environment", scene_name))
            return {
                "status": "success",
                "scene": route,
                "sun": {"enabled": True, "direction": [1.0, 2.0, 3.0]},
                "grid": {"enabled": False},
                "physics": {"gravity": [0.0, -9.8, 0.0]},
            }

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: type("Api", (), {"scene": SceneApi()})(),
    )
    monkeypatch.setattr(
        blockly_main.ScratchTool,
        "_project_scene_routes",
        classmethod(lambda cls, project_path=None: [route]),
    )

    snapshot = blockly_main.ScratchTool._create_preview_state_snapshot()

    assert calls == [("snapshot", route), ("environment", route)]
    assert snapshot["scenes"][route]["binding_mode"] == "native_editor"
    assert snapshot["scenes"][route]["environment"]["physics"]["gravity"] == [0.0, -9.8, 0.0]
    assert snapshot["scenes"][route]["actors"]["Hero"]["position"] == [4.0, 5.0, 6.0]


def test_blockly_project_snapshot_fails_closed_when_native_snapshot_is_unavailable(
    monkeypatch,
):
    from script_runtime.blockly import main as blockly_main

    route = "Scene/level.scene"
    monkeypatch.setattr(
        blockly_main.ScratchTool,
        "_project_scene_routes",
        classmethod(lambda cls, project_path=None: [route]),
    )
    monkeypatch.setattr(
        blockly_main.ScratchTool,
        "_create_native_preview_scene_state",
        classmethod(lambda cls, scene_route: None),
    )
    with pytest.raises(RuntimeError, match="native"):
        blockly_main.ScratchTool._create_preview_state_snapshot()


def test_blockly_preview_restore_uses_native_environment_camera_and_actor_contract(monkeypatch):
    import api.editor_api
    from script_runtime.blockly import main as blockly_main

    calls = []

    class SceneApi:
        def set_environment(self, scene_name, state):
            calls.append(("environment", scene_name, state))
            return {"status": "success"}

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: type("Api", (), {"scene": SceneApi(), "viewport": type("Viewport", (), {
            "set_camera_pose": lambda self, *args: calls.append(("camera", *args)) or {"status": "success"}
        })()})(),
    )
    monkeypatch.setattr(blockly_main, "cancel_pending_auto_saves", lambda: None)
    monkeypatch.setattr(blockly_main.ScratchTool, "_notify_preview_state_restored", staticmethod(lambda routes: None))
    blockly_main.ScratchTool._preview_state_snapshot = {
        "scenes": {
            "Scene/level.scene": {
                "binding_mode": "native_editor",
                "environment": {"grid": {"enabled": False}},
                "cameras": {"MainCamera": {"position": [1, 2, 3]}},
                "actors": {"Hero": {"position": [4, 5, 6], "rotation": [0, 1, 0], "scale": [1, 1, 1]}},
            }
        }
    }

    try:
        restored, error = blockly_main.ScratchTool._restore_preview_state_snapshot()
    finally:
        blockly_main.ScratchTool._preview_state_snapshot = None

    assert restored is True
    assert error is None
    assert calls[0] == ("environment", "Scene/level.scene", {"grid": {"enabled": False}})
