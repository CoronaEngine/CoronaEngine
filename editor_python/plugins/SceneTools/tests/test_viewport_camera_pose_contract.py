from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
MANIFEST_SOURCE = REPO_ROOT / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
HANDLER_SOURCE = REPO_ROOT / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"


def test_viewport_camera_pose_is_registered_as_one_public_contract():
    manifest = MANIFEST_SOURCE.read_text(encoding="utf-8")
    handlers = HANDLER_SOURCE.read_text(encoding="utf-8")

    assert "kEditorApiMethods" in manifest
    assert "kViewportSetCameraPoseParams" in manifest
    assert '"", "viewport.set_camera_pose"' in manifest
    assert '"set_camera_pose", [](const NativeRequest& request' in handlers
    assert "return apply_non_web_camera_properties(request, true);" in handlers
    assert "apply_native_camera_properties(write, false)" in handlers
