from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]


def test_project_copy_is_owned_by_runtime_not_compatibility_paths():
    canonical = (
        PLUGIN_ROOT.parents[1] / "runtime" / "legacy_project_copy.py"
    )
    compatibility_owner = (
        PLUGIN_ROOT.parents[1] / "runtime" / "compat" / "legacy_project_copy.py"
    )
    legacy_root = PLUGIN_ROOT / "project_copy.py"
    legacy_utils = PLUGIN_ROOT / "utils" / "project_copy.py"

    assert canonical.is_file()
    assert not compatibility_owner.exists()
    assert not (PLUGIN_ROOT / "compat" / "legacy_project_copy.py").exists()
    assert not legacy_root.exists()
    assert not legacy_utils.exists()
