from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"
COMPAT_ROOT = SRC_ROOT / "compat"


def test_frontend_compatibility_directory_has_a_local_boundary_document():
    boundary = COMPAT_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "src/api",
        "src/services",
        "utils/bridge.js",
        "legacyEditorAdapter.js",
        "legacyCameraLockPanel.js",
        "删除条件",
    ):
        assert marker in source


def test_frontend_bridge_only_reexports_canonical_api_and_service_owners():
    bridge = (SRC_ROOT / "utils" / "bridge.js").read_text(encoding="utf-8")

    assert "../api/editorApi.js" in bridge
    for service in (
        "appService",
        "sceneService",
        "projectService",
        "scriptingService",
        "fileService",
        "projectSettingsService",
    ):
        assert f"../compat/{service}.js" in bridge
    for service in ("lanChatService", "networkService", "aiService", "projectLauncherService", "resourceService"):
        assert f"../services/{service}.js" in bridge
    assert "cefQuery" not in bridge


def test_frontend_raw_cef_compatibility_is_isolated_to_compat_directory():
    legacy_adapter = (COMPAT_ROOT / "legacyEditorAdapter.js").read_text(encoding="utf-8")
    main_source = (SRC_ROOT / "main.js").read_text(encoding="utf-8")
    legacy_import = (SRC_ROOT / "utils" / "legacyEditorAdapter.js").read_text(encoding="utf-8")

    assert "window.cefQuery" in legacy_adapter
    assert "./compat/legacyEditorAdapter.js" in main_source
    assert "../compat/legacyEditorAdapter.js" in legacy_import

    active_sources = []
    for source_path in SRC_ROOT.rglob("*"):
        if not source_path.is_file() or source_path.suffix not in {".js", ".vue"}:
            continue
        if (
            "compat" in source_path.parts
            or "api" in source_path.parts
            or source_path == SRC_ROOT / "utils" / "legacyEditorAdapter.js"
        ):
            continue
        source = source_path.read_text(encoding="utf-8")
        if "window.cefQuery" in source or "from './utils/bridge" in source:
            active_sources.append(source_path)

    assert active_sources == []
