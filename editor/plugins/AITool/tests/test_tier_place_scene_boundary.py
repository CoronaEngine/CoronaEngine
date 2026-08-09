from pathlib import Path


SOURCE = Path(__file__).parents[1].joinpath(
    "cai_extensions",
    "flows",
    "scene_composition_workflow_v2",
    "nodes_tier_place.py",
).read_text(encoding="utf-8")


def test_tier2_scene_resolution_prefers_native_snapshot_and_keeps_legacy_fallback():
    """Tier2 placement must read scene facts through the aggregate adapter first."""
    assert "def _resolve_tier2_scene(scene_name: str)" in SOURCE
    native_start = SOURCE.index("def _resolve_tier2_scene(scene_name: str)")
    native_end = SOURCE.index("def _calculate_semantic_position(", native_start)
    resolver = SOURCE[native_start:native_end]

    assert "native_scene_state" in resolver
    assert "native_actor_views_with_legacy_fallback" in resolver
    assert "get_legacy_scene" not in resolver


def test_tier2_placement_does_not_resolve_scene_manager_inline():
    """The node itself must use the centralized resolver, not a second boundary."""
    node_start = SOURCE.index("def tier2_place_node(state: Dict[str, Any])")
    node_end = SOURCE.index("def tier3_place_node(", node_start)
    node = SOURCE[node_start:node_end]

    assert "_resolve_tier2_scene(scene_name)" in node
    assert "scene_manager" not in node


def test_tier2_native_placement_tool_loader_does_not_require_legacy_scene_manager():
    """Native placement tools must not import the old manager just to register."""
    source = Path(__file__).parents[1].joinpath(
        "cai_extensions", "mcp", "tools", "place_object_near.py"
    ).read_text(encoding="utf-8")
    loader_start = source.index("def load_place_object_near_tools")
    loader = source[loader_start:source.index("\n\n__all__", loader_start)]

    assert "from CoronaCore.core.managers import scene_manager" not in loader
    assert "scene_manager=None" in loader


def test_native_scene_tool_loader_does_not_resolve_legacy_manager_at_registration():
    source = Path(__file__).parents[1].joinpath(
        "cai_extensions", "mcp", "tools", "scene_tools.py"
    ).read_text(encoding="utf-8")
    loader_start = source.index("def load_scene_tools")
    loader = source[loader_start:source.index("\n\n__all__", loader_start)]

    assert "from CoronaCore.core.managers import scene_manager" not in loader
    assert "scene_manager=None" in loader
