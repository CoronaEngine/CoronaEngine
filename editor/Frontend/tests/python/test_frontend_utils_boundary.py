from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"
UTILS_ROOT = SRC_ROOT / "utils"

ACTIVE_UTILS = (
    "cameraDragRegions.js",
    "constants.js",
    "eventBus.js",
    "panelWindows.js",
    "serviceInitialization.js",
    "viewportGizmo.js",
    "viewportPick.js",
    "viewportUiMode.js",
)
COMPATIBILITY_UTILS = ("bridge.js", "legacyEditorAdapter.js")


def test_utils_have_a_local_boundary_inventory():
    boundary = UTILS_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "低延迟输入 adapter",
        "历史兼容转发",
        "src/api/editorApi.js",
        "src/services",
        "删除条件",
    ):
        assert marker in source
    for file_name in ACTIVE_UTILS + COMPATIBILITY_UTILS:
        assert file_name in source


def test_active_utils_do_not_depend_on_compatibility_bridge_or_raw_cef():
    for file_name in ACTIVE_UTILS:
        source = (UTILS_ROOT / file_name).read_text(encoding="utf-8")
        assert "window.cefQuery" not in source, file_name
        assert "cefQuery(" not in source, file_name
        assert "utils/bridge" not in source, file_name


def test_utils_compatibility_wrappers_have_canonical_owners():
    bridge = (UTILS_ROOT / "bridge.js").read_text(encoding="utf-8")
    legacy_adapter = (UTILS_ROOT / "legacyEditorAdapter.js").read_text(
        encoding="utf-8"
    )

    assert "../api/editorApi.js" in bridge
    assert "../services/" in bridge
    assert "../compat/legacyEditorAdapter.js" in legacy_adapter


def test_utils_files_are_not_hidden_test_or_build_owners():
    assert not list(UTILS_ROOT.glob("*.test.*"))
    assert not list(UTILS_ROOT.glob("*.spec.*"))
    assert not (UTILS_ROOT / "node_modules").exists()
