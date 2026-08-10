from pathlib import Path


SOURCE = Path(__file__).parents[1].joinpath(
    "cai_extensions",
    "flows",
    "scene_composition_workflow_v2",
    "nodes_tier_place.py",
).read_text(encoding="utf-8")


def test_mechanics_diagnostic_prefers_native_snapshot_values():
    start = SOURCE.index("def _verify_mechanics_available(scene_name: str)")
    end = SOURCE.index("def _import_actors(", start)
    diagnostic = SOURCE[start:end]

    assert "native_scene_state" in diagnostic
    assert "native_actor_views" in diagnostic
    assert "native_actor_views_with_legacy_fallback" not in diagnostic
    assert "get_legacy_scene" not in diagnostic
    assert 'getattr(sample, "_mechanics"' not in diagnostic


def test_mechanics_settlement_uses_native_actor_contract():
    start = SOURCE.index("def _apply_physics_settlement(")
    end = SOURCE.index("def _cleanup_tier_actors(", start)
    settlement = SOURCE[start:end]

    assert "set_actor_physics_value" in settlement
    assert "find_native_actor" in settlement
    assert "find_actor_with_legacy_fallback" not in settlement
    assert "_mechanics" not in settlement


def test_wall_object_fix_prefers_native_transform_contract():
    start = SOURCE.index("def _fix_wall_objects(")
    end = SOURCE.index("def _apply_physics_settlement(", start)
    wall_fix = SOURCE[start:end]

    assert "find_native_actor" in wall_fix
    assert "find_actor_with_legacy_fallback" not in wall_fix
    assert "get_legacy_scene" not in wall_fix
