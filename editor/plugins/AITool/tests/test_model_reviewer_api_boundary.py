from pathlib import Path


MODEL_REVIEWER_SOURCE = (
    Path(__file__).resolve().parents[1]
    / "cai_extensions"
    / "agent"
    / "model_reviewer.py"
)


def test_model_reviewer_uses_editor_aggregate_api_for_review_actors():
    source = MODEL_REVIEWER_SOURCE.read_text(encoding="utf-8")

    assert "from api.editor_api import CoronaEditorApi" in source
    assert "CoronaEditor.CoronaEngine.create_editor_actor" not in source
    assert "CoronaEditor.CoronaEngine.remove_editor_actor" not in source
    assert "CoronaEditorApi.scene_tools.create_actor" in source
    assert "CoronaEditorApi.scene_tools.remove_actor" in source
    assert "resolve_native_scene_value" in source
    assert "get_legacy_scene" not in source
