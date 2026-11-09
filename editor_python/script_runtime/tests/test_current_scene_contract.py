from types import SimpleNamespace
import pytest




def test_runtime_scene_does_not_reconstruct_a_legacy_python_scene():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[3] / "CoronaEditor" / "editor" / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _runtime_scene")
    end = source.index("def _runtime_scene_key", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_known_actor_queries_do_not_fallback_to_legacy_scene_adapter():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[3] / "CoronaEditor" / "editor" / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _iter_known_actors")
    end = source.index("def _deleted_names", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_actor_resolution_does_not_fallback_to_legacy_scene_adapter():
    from pathlib import Path

    source = (
        Path(__file__).resolve().parents[3] / "CoronaEditor" / "editor" / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
    ).read_text(encoding="utf-8")
    start = source.index("def _resolve_actor")
    end = source.index("def _current_actor", start)

    assert "script_runtime.compat.legacy_scene_adapter" not in source[start:end]


def test_actor_runtime_target_prefers_native_snapshot_over_legacy_scene(monkeypatch):
    from types import SimpleNamespace

    from frontend_assets import block_program as corona_engine

    native_payload = {
        "scene": "scene.ini",
        "scene_name": "Level",
        "actors": [{"name": "Hero", "handle": 7}],
    }

    monkeypatch.setattr(
        corona_engine,
        "_native_scene_snapshot",
        lambda scene_name: (native_payload, []),
    )

    result = corona_engine.resolve_runtime_target(
        "actor", scene_name="scene.ini", actor_name="Hero"
    )

    assert result["status"] == "ok"
    assert result["binding_mode"] == "native_editor"
    assert result["actor"].name == "Hero"
    assert result["actor"].handle == 7


def test_runtime_target_does_not_implicitly_fallback_to_legacy_scene(monkeypatch):
    from frontend_assets import block_program as corona_engine

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
        return {"status": "success", "scene": "scene.ini"}

    scene = ScriptRuntimeSceneAdapter(invoke)
    state = {
        "sun": {"enabled": True, "direction": [1.0, 2.0, 3.0]},
        "grid": {"enabled": False},
        "physics": {"gravity": [0.0, -9.8, 0.0]},
    }

    scene.get_environment("scene.ini")
    scene.set_environment("scene.ini", state)

    assert calls == [
        ("scene.get_environment", ["scene.ini"]),
        ("scene.set_environment", ["scene.ini", state]),
    ]


def test_script_runtime_scene_tools_exposes_actor_state_and_physics_aggregates():
    from script_runtime.manifest_adapter import ScriptRuntimeEditorApi

    calls = []

    def invoke(method, args):
        calls.append((method, args))
        return {"status": "success"}

    api = ScriptRuntimeEditorApi(invoke)
    api.scene_tools.set_actor_state("scene.ini", "Hero", {"visible": False})
    api.scene_tools.set_actor_physics(
        "scene.ini", "Hero", {"physics_enabled": True}
    )

    assert calls == [
        ("scene_tools.set_actor_state", ["scene.ini", "Hero", {"visible": False}]),
        ("scene_tools.set_actor_physics", ["scene.ini", "Hero", {"physics_enabled": True}]),
    ]


def test_native_actor_proxy_uses_script_runtime_scene_tools_for_actor_operations(monkeypatch):
    import api.editor_api
    from frontend_assets import block_program as corona_engine

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
        type("Scene", (), {"route": "scene.ini"})(),
        {"name": "Hero", "geometry": {}},
    )

    actor.set_visible(False)
    actor.set_physics_enabled(True)

    assert calls == [
        ("state", ("scene.ini", "Hero", {"visible": False})),
        ("physics", ("scene.ini", "Hero", {"physics_enabled": True})),
    ]


@pytest.mark.parametrize('body_type', ['dynamic', 'kinematic', 'static', 'phantom'])
def test_native_restore_uses_one_mechanics_patch_and_preserves_body_type(monkeypatch, body_type):
    import api.editor_api
    from script_runtime import scene_state

    calls = []

    class SceneToolsApi:
        def set_actor_state(self, *args):
            calls.append(("state", args))
            return {"status": "success"}

        def set_actor_physics(self, *args):
            assert 'collision_type' not in args[2] and 'physics_enabled' not in args[2]
            calls.append(("physics", args))
            return {"status": "success"}

    class SceneApi:
        def set_actor_transform(self, *args):
            calls.append(('transform', args))
            return {'status': 'success'}

    monkeypatch.setattr(
        api.editor_api,
        "get_script_runtime_editor_api",
        lambda: type("Api", (), {"scene_tools": SceneToolsApi(), 'scene': SceneApi()})(),
    )
    mechanics = {'body_type': body_type, 'mass': 3.0, 'restitution': 0.2, 'damping': 0.5,
                 'linear_lock': [True, False, True], 'angular_lock': [False, True, False]}
    scene_state._restore_native_actor_state('scene.ini', {
        'name': 'Hero', 'visible': False, 'geometry': {'position': [1, 2, 3]}, 'mechanics': mechanics,
    }, 'archive')
    assert calls == [
        ('physics', ('scene.ini', 'Hero', {'body_type': 'static'})),
        ('transform', ('scene.ini', 'Hero', {'position': [1, 2, 3], 'persist': False, 'archive_session': 'archive'})),
        ("state", ("scene.ini", "Hero", {"visible": False})),
        ('physics', ('scene.ini', 'Hero', mechanics)),
    ]


def test_script_runtime_has_no_scene_switching_contract():
    from frontend_assets import block_program as corona_engine
    from script_runtime.manifest_adapter import ScriptRuntimeSceneAdapter
    assert not hasattr(corona_engine, "setScene")
    assert not hasattr(corona_engine, "nextScene")
    adapter = ScriptRuntimeSceneAdapter(lambda *args: None)
    assert not hasattr(adapter, "switch")
    assert not hasattr(adapter, "list_routes")
