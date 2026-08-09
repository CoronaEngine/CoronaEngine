import sys
from pathlib import Path
from unittest.mock import patch


EDITOR_ROOT = Path(__file__).resolve().parents[5]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from editor.plugins.AITool.cai_extensions.scene_placement import paths


def test_scene_placement_scene_is_owned_by_generated_project_root():
    with patch(
        "runtime.project_context.get_project_root",
        return_value=Path("D:/Projects/Example"),
    ):
        output = paths.resolve_scene_placement_output_path(
            "Scene/room/room.scene", "Room A"
        )

    assert output == Path(
        "D:/Projects/Example/Resource/generated/scene_placement/Room A.json"
    )


def test_relative_asset_root_is_resolved_from_generated_owner():
    with patch(
        "runtime.project_context.get_project_root",
        return_value=Path("D:/Projects/Example"),
    ):
        output = paths.resolve_scene_placement_asset_root("assets")

    assert output == Path(
        "D:/Projects/Example/Resource/generated/scene_placement/assets"
    )


def test_absolute_asset_root_remains_explicit():
    explicit = Path("D:/Cache/scene-assets")
    with patch(
        "runtime.project_context.get_project_root",
        return_value=Path("D:/Projects/Example"),
    ):
        output = paths.resolve_scene_placement_asset_root(str(explicit))

    assert output == explicit
