from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_compatibility_inventory_contains_a_production_caller_audit():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    for marker in (
        "仓内生产引用",
        "静态审计",
        "外部调用未知",
        "main.js",
        "index.html",
        "runtime.runner",
    ):
        assert marker in source


def test_compatibility_audit_distinguishes_legacy_fallback_from_package_imports():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    assert "backend/runScript.py" in source
    assert "只读回退" in source
    assert "未发现直接 import" in source
    assert "不能证明外部调用方不存在" in source


def test_project_script_output_ownership_is_distinguished_from_preview_output():
    source = (EDITOR_ROOT / "API_OWNERSHIP.md").read_text(encoding="utf-8")

    assert "Scripts/blockly" in source
    assert "runtime/generated" in source
    assert "canonical runtime-data 目录" not in source


def test_compatibility_audit_keeps_frontend_legacy_bootstrap_visible():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    assert "legacyEditorAdapter.js" in source
    assert "legacyCameraLockPanel.js" in source
    assert "仍被启动加载" in source
