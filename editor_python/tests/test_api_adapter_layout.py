from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_editor_api_is_the_only_python_manifest_factory():
    source = (EDITOR_ROOT / "api" / "editor_api.py").read_text(encoding="utf-8")
    assert "class CoronaEditorApi" in source
    assert "_invoke_cpp_editor_api" in source
    assert "runtime.legacy_" not in source


def test_lan_chat_manifest_adapters_are_removed():
    adapter = EDITOR_ROOT / "api" / "lan_chat_adapters.py"
    assert not adapter.exists()
    source = (EDITOR_ROOT / "api/editor_api.py").read_text(encoding="utf-8")
    assert "lan_chat" not in source


def test_project_context_has_the_canonical_active_path_resolver():
    source = (EDITOR_ROOT / "runtime" / "project_context.py").read_text(encoding="utf-8")
    assert "get_active_project_path" in source
