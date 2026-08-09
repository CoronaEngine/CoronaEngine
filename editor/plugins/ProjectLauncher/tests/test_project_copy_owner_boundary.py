from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[3]
PLUGIN_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "project_copy.py"
PLUGIN_COMPAT_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "compat" / "legacy_project_copy.py"
RUNTIME_COPY = EDITOR_ROOT / "runtime" / "project_copy.py"
LEGACY_RUNTIME_COPY = EDITOR_ROOT / "runtime" / "legacy_project_copy.py"
RUNTIME_COMPAT_COPY = EDITOR_ROOT / "runtime" / "compat" / "legacy_project_copy.py"


def test_project_copy_implementation_lives_in_runtime():
    runtime_source = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")
    compat_source = RUNTIME_COMPAT_COPY.read_text(encoding="utf-8")
    plugin_source = PLUGIN_COPY.read_text(encoding="utf-8")
    plugin_compat_source = PLUGIN_COMPAT_COPY.read_text(encoding="utf-8")
    compatibility_source = RUNTIME_COPY.read_text(encoding="utf-8")

    assert "class ProjectCopy" in runtime_source
    assert "from plugins.ProjectLauncher.compat.legacy_project_copy import" in plugin_source
    assert "class ProjectCopy" not in plugin_source
    assert "from runtime.compat.legacy_project_copy import" in plugin_compat_source
    assert "class ProjectCopy" not in plugin_compat_source
    assert "from runtime.legacy_project_copy import" in compat_source
    assert "class ProjectCopy" in compat_source
    assert "from runtime.compat.legacy_project_copy import" in compatibility_source
    assert "class ProjectCopy" not in compatibility_source
    assert "def create_from_template" not in compatibility_source


def test_project_copy_runtime_has_no_plugin_import_dependency():
    source = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")
    assert "from plugins." not in source
    assert "from CoronaCore" not in source


def test_project_copy_uses_canonical_template_support_and_keeps_legacy_facade():
    template_support = (EDITOR_ROOT / "runtime" / "project_templates.py").read_text(
        encoding="utf-8"
    )
    project_support = (
        EDITOR_ROOT / "runtime" / "compat" / "legacy_project_support.py"
    ).read_text(
        encoding="utf-8"
    )
    runtime_copy = LEGACY_RUNTIME_COPY.read_text(encoding="utf-8")

    assert "def create_project_from_template" in template_support
    assert "from runtime.project_templates import" in runtime_copy
    assert "from runtime.project_templates import" in project_support


def test_project_copy_import_path_remains_a_compatibility_wrapper():
    source = RUNTIME_COPY.read_text(encoding="utf-8")
    plugin_source = PLUGIN_COPY.read_text(encoding="utf-8")

    assert "Compatibility" in source
    assert "from runtime.compat.legacy_project_copy import" in source
    assert "from plugins.ProjectLauncher.compat.legacy_project_copy import" in plugin_source
