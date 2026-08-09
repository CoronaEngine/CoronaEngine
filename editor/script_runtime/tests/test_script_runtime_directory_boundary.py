from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_RUNTIME_ROOT = EDITOR_ROOT / "script_runtime"


def _script_runtime_sources():
    return (
        path
        for path in SCRIPT_RUNTIME_ROOT.rglob("*.py")
        if "tests" not in path.parts
    )


def test_script_runtime_has_a_local_boundary_inventory():
    boundary = SCRIPT_RUNTIME_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "受限角色脚本运行时",
        "Blockly/Scratch",
        "manifest adapter",
        "生成脚本 runner",
        "Scripts/blockly",
        "runtime/generated",
        "编辑器公共 API",
        "删除条件",
    ):
        assert marker in source
    for path in (
        "engine/",
        "blockly/",
        "manifest_adapter.py",
        "native_engine_adapter.py",
        "runner.py",
    ):
        assert path in source


def test_script_runtime_has_explicit_restricted_entrypoints():
    manifest_adapter = (SCRIPT_RUNTIME_ROOT / "manifest_adapter.py").read_text(
        encoding="utf-8"
    )
    runner = (SCRIPT_RUNTIME_ROOT / "runner.py").read_text(encoding="utf-8")

    assert "ScriptRuntimeEditorApi" in manifest_adapter
    assert "ScriptRuntimeSceneAdapter" in manifest_adapter
    assert "run_generated_script" in runner
    assert "runtime/generated" in runner


def test_script_runtime_does_not_import_editor_plugins_or_legacy_backend():
    offenders = []
    for path in _script_runtime_sources():
        source = path.read_text(encoding="utf-8")
        if (
            "from plugins" in source
            or "import plugins" in source
            or "from backend" in source
            or "import backend" in source
        ):
            offenders.append(path)
    assert offenders == []


def test_script_runtime_engine_is_not_the_editor_manifest_owner():
    boundary = (SCRIPT_RUNTIME_ROOT / "BOUNDARY.md").read_text(encoding="utf-8")
    engine = (SCRIPT_RUNTIME_ROOT / "engine" / "corona_engine.py").read_text(
        encoding="utf-8"
    )

    assert "Script Runtime" in boundary
    assert "不得" in boundary
    assert "editorApi" not in engine
    assert "window.cefQuery" not in engine


def test_blockly_preview_names_the_runtime_generated_output_owner_explicitly():
    blockly = (SCRIPT_RUNTIME_ROOT / "blockly" / "main.py").read_text(
        encoding="utf-8"
    )

    assert "generated_script_dir = core_path.generated_script_dir" in blockly
    assert "\n    script_dir = core_path.generated_script_dir" not in blockly
