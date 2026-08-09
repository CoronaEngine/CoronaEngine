from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1].parent


def test_runtime_logging_has_a_canonical_owner():
    canonical = EDITOR_ROOT / "runtime" / "logging.py"
    compatibility = EDITOR_ROOT / "utils" / "compat" / "legacy_logging.py"

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from runtime.logging import" in (EDITOR_ROOT / "runtime" / "bootstrap.py").read_text(
        encoding="utf-8"
    )


def test_obsolete_script_cleanup_modules_are_not_part_of_the_runtime():
    assert not (EDITOR_ROOT / "utils" / "cleanup.py").exists()
    assert not (EDITOR_ROOT / "utils" / "hot_reload.py").exists()


def test_scene_support_owns_scene_manifest_and_autosave_helpers():
    scene_support = EDITOR_ROOT / "runtime" / "scene_support.py"
    project_support = EDITOR_ROOT / "runtime" / "project_support.py"
    script_runtime = EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py"
    blockly_runtime = EDITOR_ROOT / "script_runtime" / "blockly" / "main.py"

    assert scene_support.is_file()
    scene_source = scene_support.read_text(encoding="utf-8")
    project_source = project_support.read_text(encoding="utf-8")
    project_compat_source = (
        EDITOR_ROOT / "runtime" / "compat" / "legacy_project_support.py"
    ).read_text(encoding="utf-8")
    assert "def get_project_scenes" in scene_source
    assert "def auto_save" in scene_source
    assert "from runtime.scene_support import" in script_runtime.read_text(encoding="utf-8")
    assert "from runtime.scene_support import" in blockly_runtime.read_text(encoding="utf-8")
    assert "from runtime.scene_support import" in project_compat_source
    assert "def get_project_scenes" not in project_source
    assert "def auto_save" not in project_source


def test_runtime_project_compatibility_wrappers_have_a_dedicated_owner():
    runtime_root = EDITOR_ROOT / "runtime"
    compatibility = runtime_root / "compat"
    support_owner = compatibility / "legacy_project_support.py"
    copy_owner = compatibility / "legacy_project_copy.py"

    assert support_owner.is_file()
    assert copy_owner.is_file()
    assert "from runtime.compat.legacy_project_support import" in (
        runtime_root / "project_support.py"
    ).read_text(encoding="utf-8")
    assert "from runtime.compat.legacy_project_copy import" in (
        runtime_root / "project_copy.py"
    ).read_text(encoding="utf-8")


def test_project_templates_owns_template_and_project_ini_helpers():
    templates = EDITOR_ROOT / "runtime" / "project_templates.py"
    compatibility = EDITOR_ROOT / "runtime" / "project_support.py"

    assert templates.is_file()
    template_source = templates.read_text(encoding="utf-8")
    compatibility_source = compatibility.read_text(encoding="utf-8")
    assert "def create_project_from_template" in template_source
    assert "def normalize_project_runtime_paths" in template_source
    assert "def update_project_config" in template_source
    assert "def create_project_from_template" not in compatibility_source


def test_legacy_entities_use_canonical_scene_support_for_auto_save():
    for relative_path in (
        "legacy/entities/actor.py",
        "legacy/entities/scene.py",
    ):
        source = (EDITOR_ROOT / "runtime" / relative_path).read_text(encoding="utf-8")
        assert "from runtime.scene_support import auto_save" in source
        assert "from runtime.project_support import auto_save" not in source
