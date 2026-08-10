from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[3]
PLUGIN_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "project_copy.py"
PLUGIN_COMPAT_COPY = EDITOR_ROOT / "plugins" / "ProjectLauncher" / "compat" / "legacy_project_copy.py"
RUNTIME_COPY = EDITOR_ROOT / "runtime" / "project_copy.py"
LEGACY_RUNTIME_COPY = EDITOR_ROOT / "runtime" / "legacy_project_copy.py"
RUNTIME_COMPAT_COPY = EDITOR_ROOT / "runtime" / "compat" / "legacy_project_copy.py"


def test_project_copy_implementation_lives_in_native_project_contract():
    native_manifest = (EDITOR_ROOT.parent / "src/systems/ui/editor_api/cef_editor_api.cpp").read_text(encoding="utf-8")
    native_handlers = (EDITOR_ROOT.parent / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")

    assert '"project.copyExistingToData"' in native_manifest
    assert "copy_existing_project_to_data_native" in native_handlers
    assert not LEGACY_RUNTIME_COPY.exists()
    assert not PLUGIN_COPY.exists()
    assert not PLUGIN_COMPAT_COPY.exists()
    assert not RUNTIME_COPY.exists()
    assert not RUNTIME_COMPAT_COPY.exists()


def test_project_copy_uses_canonical_template_support_without_legacy_facade():
    template_support = (EDITOR_ROOT / "runtime" / "project_templates.py").read_text(
        encoding="utf-8"
    )
    assert "def create_project_from_template" in template_support
    assert not (EDITOR_ROOT / "runtime" / "project_support.py").exists()
    assert not (
        EDITOR_ROOT / "runtime" / "compat" / "legacy_project_support.py"
    ).exists()
