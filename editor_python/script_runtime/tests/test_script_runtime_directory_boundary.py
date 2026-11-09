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
        "scripts.runFile",
        "Scripts/files",
        "requestId",
        "编辑器公共 API",
        "runId",
    ):
        assert marker in source
    for path in (
        "file_api.py",
        "scene_state.py",
        "manifest_adapter.py",
        "native_engine_adapter.py",
        "file_runner.py",
    ):
        assert path in source


def test_script_runtime_has_explicit_restricted_entrypoints():
    manifest_adapter = (SCRIPT_RUNTIME_ROOT / "manifest_adapter.py").read_text(
        encoding="utf-8"
    )
    runner = (SCRIPT_RUNTIME_ROOT / "file_runner.py").read_text(encoding="utf-8")

    assert "ScriptRuntimeEditorApi" in manifest_adapter
    assert "ScriptRuntimeSceneAdapter" in manifest_adapter
    assert "def run_file(" in runner
    assert "def execute_python_code(" not in runner


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
    engine = (SCRIPT_RUNTIME_ROOT / "file_runner.py").read_text(
        encoding="utf-8"
    )

    assert "Script Runtime" in boundary
    assert "不得" in boundary
    assert "editorApi" not in engine
    assert "window.cefQuery" not in engine
