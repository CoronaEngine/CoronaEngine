from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]


def test_script_runtime_legacy_scene_adapter_is_removed():
    engine_source = (
        EDITOR_ROOT.parent / "CoronaEditor" / "editor" / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
    ).read_text(encoding="utf-8")
    blockly_source = (
        EDITOR_ROOT / "script_runtime" / "file_runner.py"
    ).read_text(encoding="utf-8")
    canonical = (
        EDITOR_ROOT / "script_runtime" / "compat" / "legacy_scene_adapter.py"
    )

    assert not canonical.is_file()
    assert "script_runtime.compat.legacy_scene_adapter" not in engine_source
    assert "script_runtime.compat.legacy_scene_adapter" not in blockly_source


def test_historical_runtime_scene_shims_are_removed():
    assert not (EDITOR_ROOT / "runtime" / "legacy_script_scene_adapter.py").exists()
    assert not (EDITOR_ROOT / "script_runtime" / "legacy_scene_datas_adapter.py").exists()


def test_script_runtime_compatibility_package_is_removed_after_adapter_migration():
    compatibility = EDITOR_ROOT / "script_runtime" / "compat"

    assert not compatibility.exists()
