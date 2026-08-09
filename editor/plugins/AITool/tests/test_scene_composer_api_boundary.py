from pathlib import Path


SCENE_COMPOSER_SOURCE = (
    Path(__file__).resolve().parents[1]
    / "cai_extensions"
    / "agent"
    / "scene_composer.py"
)


def test_scene_composer_creates_actors_through_editor_aggregate_api():
    source = SCENE_COMPOSER_SOURCE.read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditor.CoronaEngine.create_editor_actor" not in source
    assert "CoronaEditorApi.scene_tools.create_actor" in source
