import sys
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[4]
EDITOR_ROOT = PROJECT_ROOT / "editor"
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
for path in (PROJECT_ROOT, EDITOR_ROOT, AI_TOOL_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

from cai_extensions.agent import agent_adapter
from cai_extensions.agent.scene_composer import _update_actor_physics


def test_scene_composer_uses_the_centralized_actor_view_boundary():
    source = (
        Path(__file__).resolve().parents[1]
        / "cai_extensions"
        / "agent"
        / "scene_composer.py"
    ).read_text(encoding="utf-8")

    assert "native_actor_views_with_legacy_fallback" in source
    assert "get_legacy_scene" not in source


def test_scene_composer_updates_native_actor_physics_as_a_value_object():
    actor = mock.Mock()

    assert _update_actor_physics(
        actor,
        physics_enabled=False,
        damping=0.98,
    )

    actor.set_mechanics.assert_called_once_with(
        {"physics_enabled": False, "damping": 0.98}
    )


def test_scene_composer_keeps_legacy_mechanics_inside_fallback_actor():
    mechanics = mock.Mock()
    actor = mock.Mock(set_mechanics=None, _mechanics=mechanics)

    assert _update_actor_physics(actor, physics_enabled=True)

    mechanics.set_physics_enabled.assert_called_once_with(True)


def test_agent_scene_reads_authoritative_native_actor_views_first():
    source = Path(agent_adapter.__file__).read_text(encoding="utf-8")
    assert "native_actor_views_with_legacy_fallback" in source
    assert "get_legacy_scene" not in source
    native_actor = mock.Mock(name="native_actor")
    with mock.patch(
        "plugins.AITool.cai_extensions.mcp.tools.native_scene_state.native_actor_views_with_legacy_fallback",
        return_value=[native_actor],
    ) as native_views, mock.patch(
        "runtime.legacy_scene_store._scene_manager"
    ) as legacy_manager:
        result = agent_adapter._get_runtime_scene_actors()

    assert result == [native_actor]
    native_views.assert_called_once_with("")
    legacy_manager.get.assert_not_called()


def test_agent_scene_keeps_legacy_manager_as_fallback():
    legacy_actor = mock.Mock(name="legacy_actor")
    legacy_scene = mock.Mock()
    legacy_scene.get_actors.return_value = [legacy_actor]
    with mock.patch(
        "plugins.AITool.cai_extensions.mcp.tools.native_scene_state.native_actor_views_with_legacy_fallback",
        return_value=[legacy_actor],
    ) as actor_views, mock.patch(
        "runtime.legacy_scene_store._scene_manager"
    ) as legacy_manager:
        result = agent_adapter._get_runtime_scene_actors()

    assert result == [legacy_actor]
    actor_views.assert_called_once_with("")
    legacy_manager.get.assert_not_called()
