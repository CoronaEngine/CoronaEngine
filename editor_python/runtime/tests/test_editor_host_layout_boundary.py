from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_editor_host_has_a_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "editor_host.py"
    main_source = (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(encoding="utf-8")

    assert canonical.is_file()
    assert not any(
        path.is_file() and "__pycache__" not in path.parts
        for path in (EDITOR_ROOT / "CoronaCore").rglob("*")
    )
    assert "from runtime.editor_host import CoronaEditor" in main_source
