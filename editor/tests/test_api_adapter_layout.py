from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]
API_SOURCE = EDITOR_ROOT / "api" / "editor_api.py"
NETWORK_SOURCE = EDITOR_ROOT / "runtime" / "legacy_network_adapters.py"
EDITOR_ADAPTER_SOURCE = EDITOR_ROOT / "runtime" / "legacy_editor_adapters.py"
PROJECT_CONTEXT_SOURCE = EDITOR_ROOT / "runtime" / "project_context.py"
API_BOUNDARY = EDITOR_ROOT / "api" / "BOUNDARY.md"


def test_lanchat_compatibility_implementations_live_in_runtime_adapter_module():
    api_source = API_SOURCE.read_text(encoding="utf-8")
    network_source = NETWORK_SOURCE.read_text(encoding="utf-8")

    for symbol in ("_LanChatTransportAdapter", "_LanChatQueueAdapter"):
        assert f"class {symbol}" not in api_source
        assert f"class {symbol}" in network_source


def test_editor_api_remains_the_public_lanchat_factory_boundary():
    api_source = API_SOURCE.read_text(encoding="utf-8")

    assert "def get_lan_chat_transport_adapter" in api_source
    assert "def get_lan_chat_queue_adapter" in api_source
    assert "CoronaEditorApi.lan_chat" in api_source


def test_legacy_editor_context_implementations_live_in_runtime_adapter_module():
    api_source = API_SOURCE.read_text(encoding="utf-8")
    adapter_source = EDITOR_ADAPTER_SOURCE.read_text(encoding="utf-8")

    for symbol in (
        "def get_active_project_path",
        "def set_compat_active_project_path",
        "def emit_compat_editor_event",
        "def set_compat_editor_camera_input_enabled",
        "def get_compat_editor_selection",
    ):
        assert symbol not in api_source
        if symbol in ("def get_active_project_path", "def set_compat_active_project_path"):
            assert symbol not in adapter_source
        else:
            assert symbol in adapter_source


def test_project_context_has_the_canonical_active_path_resolver():
    source = PROJECT_CONTEXT_SOURCE.read_text(encoding="utf-8")
    adapter_source = EDITOR_ADAPTER_SOURCE.read_text(encoding="utf-8")

    assert "def get_active_project_path" in source
    assert "def set_compat_active_project_path" in source
    assert "from runtime.project_context import" in adapter_source


def test_runtime_project_consumers_do_not_read_config_settings_directly():
    consumers = (
        EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py",
        EDITOR_ROOT / "runtime" / "legacy" / "entities" / "actor.py",
        EDITOR_ROOT / "plugins" / "AITool" / "services" / "cabbage_context_service.py",
        EDITOR_ROOT
        / "plugins"
        / "AITool"
        / "cai_extensions"
        / "scene_placement"
        / "tools"
        / "placement_tools.py",
    )

    for path in consumers:
        source = path.read_text(encoding="utf-8")
        assert "from config.settings import settings_manager" not in source
        assert "runtime.project_context" in source


def test_api_directory_documents_contract_and_legacy_factory_boundary():
    source = API_BOUNDARY.read_text(encoding="utf-8")

    for marker in (
        "公共契约",
        "manifest",
        "Scene/Viewport",
        "runtime/legacy_scene_adapters.py",
        "不负责",
        "删除条件",
    ):
        assert marker in source
