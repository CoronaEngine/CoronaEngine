from pathlib import Path


ROOT = Path(__file__).parents[4]
MANIFEST = (ROOT / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp").read_text(
    encoding="utf-8"
)
HANDLER = (ROOT / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp").read_text(
    encoding="utf-8"
)


def test_actor_physics_has_a_scene_tools_manifest_contract():
    assert "kSceneActorPhysicsParams" in MANIFEST
    assert '"sceneTools.setActorPhysics"' in MANIFEST
    assert '"scene_tools.set_actor_physics"' in MANIFEST


def test_actor_physics_is_handled_by_scene_tools_not_scene_datas():
    assert '"set_actor_physics"' in HANDLER
    scene_tools_start = HANDLER.index("void register_scene_tools_api_handlers")
    scene_tools = HANDLER[scene_tools_start:]
    assert '"set_actor_physics"' in scene_tools


def test_actor_physics_updates_revision_and_uses_existing_actor_changed_event():
    start = HANDLER.index('{"set_actor_physics"')
    end = HANDLER.index('{"capture_viewport"', start)
    handler = HANDLER[start:end]

    assert "actor_version" in handler
    assert "emit_actor_change(context" in handler
