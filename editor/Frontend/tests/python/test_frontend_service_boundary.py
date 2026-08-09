from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SERVICES_ROOT = FRONTEND_ROOT / "src" / "services"
COMPAT_ROOT = FRONTEND_ROOT / "src" / "compat"

EXPECTED_SERVICES = (
    "sceneService.js",
    "projectService.js",
    "appService.js",
    "lanChatService.js",
    "networkService.js",
    "scriptingService.js",
    "aiService.js",
    "projectLauncherService.js",
    "fileService.js",
    "projectSettingsService.js",
    "resourceService.js",
    "logService.js",
    "nodeGraphGenerationService.js",
    "nodeGraphReviewService.js",
    "nodeGraphRuntimeService.js",
    "cabbageAssistantContextService.js",
    "cabbageGuidanceService.js",
    "cabbageTutorialSessionService.js",
)


def test_frontend_services_have_a_local_boundary_inventory():
    boundary = SERVICES_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "Manifest facade",
        "节点图领域编排",
        "Cabbage UI 状态",
        "editorApi",
        "删除条件",
    ):
        assert marker in source
    for service_name in EXPECTED_SERVICES:
        assert service_name in source


def test_frontend_services_do_not_define_transport_or_raw_cef_protocols():
    for source_path in SERVICES_ROOT.glob("*.js"):
        source = source_path.read_text(encoding="utf-8")
        assert "window.cefQuery" not in source, source_path
        assert "cefQuery(" not in source, source_path
        assert "utils/bridge" not in source, source_path


def test_compatibility_facades_delegate_to_editor_api():
    for service_name in (
        "sceneService.js",
        "projectService.js",
        "scriptingService.js",
        "fileService.js",
        "projectSettingsService.js",
    ):
        source = (COMPAT_ROOT / service_name).read_text(encoding="utf-8")
        assert "editorApi" in source, service_name

    for service_name in (
        "lanChatService.js",
        "networkService.js",
        "aiService.js",
        "projectLauncherService.js",
        "resourceService.js",
    ):
        source = (SERVICES_ROOT / service_name).read_text(encoding="utf-8")
        assert "editorApi" in source, service_name


def test_compatibility_facade_paths_in_services_are_thin_wrappers():
    for service_name in (
        "appService.js",
        "sceneService.js",
        "projectService.js",
        "scriptingService.js",
        "fileService.js",
        "projectSettingsService.js",
        "logService.js",
    ):
        source = (SERVICES_ROOT / service_name).read_text(encoding="utf-8")
        assert "../compat/" in source, service_name
        assert "export const " not in source, service_name
