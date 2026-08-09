from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_editor_host_has_a_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "editor_host.py"
    compatibility = EDITOR_ROOT / "CoronaCore" / "core" / "corona_editor.py"
    main_source = (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from runtime.editor_host import CoronaEditor" in main_source
