from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]


def test_project_copy_is_owned_by_runtime_not_compatibility_paths():
    canonical = (
        PLUGIN_ROOT.parents[1] / "runtime" / "legacy_project_copy.py"
    )
    compatibility_owner = (
        PLUGIN_ROOT.parents[1] / "runtime" / "compat" / "legacy_project_copy.py"
    )
    compatibility = PLUGIN_ROOT / "compat" / "legacy_project_copy.py"
    legacy_root = PLUGIN_ROOT / "project_copy.py"
    legacy_utils = PLUGIN_ROOT / "utils" / "project_copy.py"

    assert canonical.is_file()
    assert compatibility_owner.is_file()
    assert "from runtime.legacy_project_copy import" in compatibility_owner.read_text(
        encoding="utf-8"
    )
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from plugins.ProjectLauncher.compat.legacy_project_copy import" in legacy_root.read_text(encoding="utf-8")
    assert "from plugins.ProjectLauncher.compat.legacy_project_copy import" in legacy_utils.read_text(encoding="utf-8")
