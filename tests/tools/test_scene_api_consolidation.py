"""Keep the Web scene surface consolidated without widening hot-path reads.

The scene names now live in `editor_operation_table.h`; this guard pins which of them
are still published and which retired spellings must not come back.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import operation_table

ROOT = Path(__file__).resolve().parents[2]


def _web_scene_names() -> set[str]:
    return {row["name"]
            for row in operation_table.web_rows() if row["group"] == "scene"}


def test_scene_v8_has_only_the_consolidated_names():
    names = _web_scene_names()
    assert len(names) == 25, f"expected 25 published scene operations, found {len(names)}"
    for name in ('read_scene', 'update_actor', 'update_camera', 'read_actor_rig',
                 'read_active', 'read_transforms', 'set_actor_transform', 'set_actor_ik_chains'):
        assert name in names
    for name in ('read_tree', 'read_actor_index', 'read_actor_tree', 'read_cameras',
                 'set_actor_properties', 'rename_actor', 'set_camera_properties', 'rename_camera',
                 'read_actor_skeleton', 'read_actor_ik_chains'):
        assert name not in names


def test_scene_query_uses_explicit_owned_projection():
    source = (ROOT / "src/systems/ui/cef/cef_scene_operations.h").read_text(encoding="utf-8-sig")
    assert 'enum class ActorProjection' in source
    assert 'ActorProjection actors' in source
    assert 'bool cameras = false' in source
    binding = (ROOT / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8-sig")
    assert 'bool read_scene_query(' in binding
    assert 'scene_query_value(' in binding
