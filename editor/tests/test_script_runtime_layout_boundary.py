from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_manifest_and_script_runtime_have_canonical_owners():
    assert (EDITOR_ROOT / "api" / "editor_api.py").is_file()
    assert (EDITOR_ROOT / "script_runtime" / "manifest_adapter.py").is_file()
    assert (EDITOR_ROOT / "script_runtime" / "native_engine_adapter.py").is_file()
    assert not any(
        path.is_file() and "__pycache__" not in path.parts
        for path in (EDITOR_ROOT / "CoronaCore").rglob("*")
    )


def test_cpp_editor_manifest_owner_is_outside_the_cef_host_directory():
    root = EDITOR_ROOT.parent
    canonical = root / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
    cmake = (root / "src" / "systems" / "ui" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert canonical.is_file()
    assert "editor_api/cef_editor_api.cpp" in cmake


def test_blockly_runtime_and_generated_scripts_use_script_runtime():
    registry = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")
    runner = (EDITOR_ROOT / "script_runtime" / "runner.py").read_text(encoding="utf-8")
    assert (EDITOR_ROOT / "script_runtime" / "blockly" / "main.py").is_file()
    assert '"ScratchTool": (f"{_BLOCKLY_PACKAGE}.blockly.main", "ScratchTool")' in registry
    assert "runtime/generated" in runner
    assert "backend/runScript" not in runner


def test_script_runtime_uses_restricted_manifest_adapter():
    source = (EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py").read_text(encoding="utf-8")
    adapter = (EDITOR_ROOT / "script_runtime" / "manifest_adapter.py").read_text(encoding="utf-8")
    assert "get_script_runtime_editor_api" in source
    assert "scene.get_snapshot" in adapter
    assert "scene_tools.create_actor" in adapter
