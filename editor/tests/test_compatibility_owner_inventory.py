import os
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def _files_under_editor(suffix):
    result = []
    for current_root, directory_names, file_names in os.walk(EDITOR_ROOT):
        directory_names[:] = [
            name
            for name in directory_names
            if name not in {"__pycache__", "node_modules", "Quasar"}
        ]
        result.extend(
            Path(current_root) / name
            for name in file_names
            if name.endswith(suffix)
        )
    return result


def test_global_inventory_declares_legacy_compat_registration_rule():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    assert "每个 `compat/legacy_*.py` 必须登记 canonical owner" in source


def test_every_legacy_compat_implementation_is_documented():
    documents = {
        path: path.read_text(encoding="utf-8", errors="ignore")
        for path in _files_under_editor(".md")
    }

    undocumented = []
    for path in _files_under_editor(".py"):
        if "compat" not in path.parts or not path.name.startswith("legacy_"):
            continue
        relative_path = path.relative_to(EDITOR_ROOT).as_posix()
        if not any(
            relative_path in source or path.name in source
            for source in documents.values()
        ):
            undocumented.append(relative_path)

    assert undocumented == []
