from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = (ROOT / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")


def test_actor_skeleton_leaves_is_not_exposed_to_cef():
    assert '"sceneTools.getActorSkeletonLeaves"' not in MANIFEST
    assert (
        'SceneTools, get_actor_skeleton_leaves'
        in MANIFEST
        and 'animation_callers() & ~caller_mask(EditorApiCaller::Cef)' in MANIFEST
    )
