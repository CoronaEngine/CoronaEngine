from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"
COMPAT_ROOT = SRC_ROOT / "compat"


def test_frontend_compatibility_directory_has_been_removed_after_migration():
    assert not list(COMPAT_ROOT.glob("*"))


def test_removed_frontend_bridge_is_not_supported():
    assert not (SRC_ROOT / "utils" / "bridge.js").exists()


def test_frontend_raw_cef_compatibility_is_isolated_to_compat_directory():
    main_source = (SRC_ROOT / "main.js").read_text(encoding="utf-8")
    assert "window.cefQuery" not in main_source
    assert not list(COMPAT_ROOT.glob("*.js"))
    assert not (SRC_ROOT / "utils" / "legacyEditorAdapter.js").exists()

    active_sources = []
    for source_path in SRC_ROOT.rglob("*"):
        if not source_path.is_file() or source_path.suffix not in {".js", ".vue"}:
            continue
        if (
            "compat" in source_path.parts
            or "api" in source_path.parts
        ):
            continue
        source = source_path.read_text(encoding="utf-8")
        if "window.cefQuery" in source or "from './utils/bridge" in source:
            active_sources.append(source_path)

    assert active_sources == []


def test_canonical_entrypoint_does_not_boot_legacy_camera_panel():
    index_source = (FRONTEND_ROOT / "index.html").read_text(encoding="utf-8")

    assert "legacyCameraLockPanel.js" not in index_source
    assert "legacyCameraLockPanel.css" not in index_source


def test_migrated_camera_lock_panel_files_are_removed():
    assert not (COMPAT_ROOT / "legacyCameraLockPanel.js").exists()
    assert not (COMPAT_ROOT / "legacyCameraLockPanel.css").exists()


def test_migrated_raw_cef_frontend_adapter_is_removed():
    assert not list(COMPAT_ROOT.glob("*.js"))
    assert not (SRC_ROOT / "utils" / "legacyEditorAdapter.js").exists()


def test_camera_lock_controls_have_a_canonical_vue_owner():
    object_source = (SRC_ROOT / "views" / "sidebar" / "Object.vue").read_text(
        encoding="utf-8"
    )

    assert "actor.cameraLock.enabled" in object_source
    assert "actor.cameraLock.position" in object_source
    assert "editorApi.sceneTools.setActorCameraLock" in object_source
