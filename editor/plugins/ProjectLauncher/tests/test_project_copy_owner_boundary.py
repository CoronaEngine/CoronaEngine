from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[3]
PLUGIN_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "project_copy.py"
PLUGIN_COMPAT_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "compat" / "legacy_project_copy.py"
RUNTIME_COPY = EDITOR_ROOT / "runtime" / "project_copy.py"
LEGACY_RUNTIME_COPY = EDITOR_ROOT / "runtime" / "legacy_project_copy.py"
RUNTIME_COMPAT_COPY = EDITOR_ROOT / "runtime" / "compat" / "legacy_project_copy.py"


def test_project_copy_implementation_lives_in_runtime():
    runtime_source = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")

    assert "class ProjectCopy" in runtime_source
    assert not PLUGIN_COPY.exists()
    assert not PLUGIN_COMPAT_COPY.exists()
    assert not RUNTIME_COPY.exists()
    assert not RUNTIME_COMPAT_COPY.exists()


def test_project_copy_runtime_has_no_plugin_import_dependency():
    source = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")
    assert "from plugins." not in source
    assert "from CoronaCore" not in source


def test_project_copy_uses_canonical_template_support_without_legacy_facade():
    template_support = (EDITOR_ROOT / "runtime" / "project_templates.py").read_text(
        encoding="utf-8"
    )
    runtime_copy = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")

    assert "def create_project_from_template" in template_support
    assert "from runtime.project_templates import" in runtime_copy
    assert not (EDITOR_ROOT / "runtime" / "project_support.py").exists()
    assert not (
        EDITOR_ROOT / "runtime" / "compat" / "legacy_project_support.py"
    ).exists()


def test_project_copy_uses_explicit_data_root_in_canonical_owner():
    source = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")

    assert "data_root=None" in source
    assert "get_legacy_project_data_dir(core_path.repo_root)" in source
