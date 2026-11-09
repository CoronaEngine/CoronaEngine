from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_runtime_logging_has_a_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "logging.py"
    assert canonical.is_file()
    assert "from runtime.logging import" in (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(
        encoding="utf-8"
    )


def test_obsolete_script_cleanup_modules_are_not_part_of_the_runtime():
    assert not (EDITOR_ROOT / "utils" / "cleanup.py").exists()
    assert not (EDITOR_ROOT / "utils" / "hot_reload.py").exists()


def test_obsolete_scene_services_are_absent():
    for name in ("network_sync_policy.py", "scene_support.py", "project_templates.py"):
        assert not (EDITOR_ROOT / "runtime" / name).exists()


def test_runtime_project_support_has_no_compatibility_wrappers():
    runtime_root = EDITOR_ROOT / "runtime"
    compatibility = runtime_root / "compat"
    support_owner = compatibility / "legacy_project_support.py"

    assert not support_owner.exists()
    assert not (runtime_root / "project_support.py").exists()
    assert not (runtime_root / "project_copy.py").exists()
