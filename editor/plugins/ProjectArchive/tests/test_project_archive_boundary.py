from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = PLUGIN_ROOT / "main.py"
COMPAT_SOURCE_PATH = PLUGIN_ROOT / "compat" / "legacy_project_archive.py"


def test_project_archive_has_a_local_boundary_document():
    boundary = PLUGIN_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    assert "runtime.archive" in source
    assert "project.migrateLegacyScene" in source
    assert "compat/legacy_project_archive.py" in source
    assert "load_policy" in source
    assert "canonical owner" in source


def test_project_archive_is_a_thin_facade_over_the_runtime_parser():
    source = SOURCE_PATH.read_text(encoding="utf-8")

    compat_source = COMPAT_SOURCE_PATH.read_text(encoding="utf-8")

    assert "from plugins.ProjectArchive.compat.legacy_project_archive import" in source
    assert "@PluginBase.register_web(\"ProjectArchive\")" in compat_source
    assert "def parse(" in compat_source
    assert "def parse(" not in source
    assert "CoronaCore" not in source
    assert "scene_manager" not in source


def test_project_archive_keeps_diagnostics_and_load_policy_at_the_facade_boundary():
    source = COMPAT_SOURCE_PATH.read_text(encoding="utf-8")

    for marker in (
        "load_policy",
        "decision_required",
        "ready_degraded",
        "to_diagnostic",
    ):
        assert marker in source
