from pathlib import Path


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = PLUGIN_ROOT / "main.py"


def test_project_archive_has_a_local_boundary_document():
    boundary = PLUGIN_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    assert "runtime.archive" in source
    assert "project.migrateLegacyScene" in source
    assert "main.py" in source
    assert "load_policy" in source
    assert "canonical owner" in source


def test_project_archive_service_owner_lives_in_main():
    source = SOURCE_PATH.read_text(encoding="utf-8")
    assert "@PluginBase.register_web(\"ProjectArchive\")" in source
    assert "def parse(" in source
    assert "CoronaCore" not in source
    assert "scene_manager" not in source
    assert not (PLUGIN_ROOT / "compat" / "legacy_project_archive.py").exists()
    assert not (PLUGIN_ROOT / "compat" / "__init__.py").exists()


def test_project_archive_keeps_diagnostics_and_load_policy_at_the_facade_boundary():
    source = SOURCE_PATH.read_text(encoding="utf-8")

    for marker in (
        "load_policy",
        "decision_required",
        "ready_degraded",
        "to_diagnostic",
    ):
        assert marker in source
