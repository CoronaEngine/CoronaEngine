from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_architecture_documents_the_three_layer_boundary():
    source = (EDITOR_ROOT / "ARCHITECTURE.md").read_text(encoding="utf-8")
    for marker in ("Vue", "C++", "Python", "manifest", "camera()", "geometry()"):
        assert marker in source


def test_editor_readme_documents_canonical_directories():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")
    for marker in ("api", "runtime", "script_runtime", "plugins", "runtime/generated"):
        assert marker in source


def test_api_ownership_documents_single_contract_owner():
    source = (EDITOR_ROOT / "API_OWNERSHIP.md").read_text(encoding="utf-8")
    assert "manifest/schema" in source
    assert "跨层契约唯一来源" in source
