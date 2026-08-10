from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_editor_business_modules_use_aggregate_api():
    for path in (EDITOR_ROOT / "plugins").rglob("*.py"):
        if "tests" in path.parts or "Quasar" in path.parts:
            continue
        source = path.read_text(encoding="utf-8-sig")
        assert "CoronaCore" not in source
        assert "runtime.legacy_" not in source


def test_public_api_has_no_legacy_fallback_imports():
    source = (EDITOR_ROOT / "api" / "editor_api.py").read_text(encoding="utf-8")
    assert "runtime.legacy_" not in source
    assert "_invoke_cpp_editor_api" in source


def test_script_runtime_isolated_from_editor_plugins():
    for path in (EDITOR_ROOT / "script_runtime").rglob("*.py"):
        if "tests" in path.parts:
            continue
        source = path.read_text(encoding="utf-8-sig")
        assert "CoronaCore" not in source
        assert "from plugins." not in source


def test_removed_compatibility_roots_contain_no_source_files():
    for name in ("backend", "CoronaCore", "CoronaPlugin", "utils", "scripts"):
        assert not any(
            path.is_file() and "__pycache__" not in path.parts
            for path in (EDITOR_ROOT / name).rglob("*")
        )
