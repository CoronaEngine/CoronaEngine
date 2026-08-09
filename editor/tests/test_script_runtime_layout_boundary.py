from pathlib import Path
import importlib
import sys


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_editor_manifest_contract_has_an_explicit_api_package_owner():
    canonical = EDITOR_ROOT / "api" / "editor_api.py"
    compatibility = EDITOR_ROOT / "CoronaCore" / "core" / "editor_api.py"

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")


def test_cpp_editor_manifest_owner_is_outside_the_cef_host_directory():
    canonical = EDITOR_ROOT.parent / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
    legacy = EDITOR_ROOT.parent / "src" / "systems" / "ui" / "cef" / "cef_editor_api.cpp"
    cmake = (EDITOR_ROOT.parent / "src" / "systems" / "ui" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )

    assert canonical.is_file()
    assert not legacy.exists()
    assert "editor_api/cef_editor_api.cpp" in cmake
    assert "cef/cef_editor_api.cpp" not in cmake


def test_compatibility_package_does_not_own_canonical_runtime_tests():
    canonical_tests = (
        EDITOR_ROOT / "api" / "tests" / "test_editor_api_aggregate.py",
        EDITOR_ROOT / "runtime" / "tests" / "test_legacy_scene_store_boundary.py",
        EDITOR_ROOT / "script_runtime" / "tests" / "test_scratch_camera_adapter.py",
    )
    compatibility_tests = EDITOR_ROOT / "CoronaCore" / "core" / "tests"

    assert all(path.is_file() for path in canonical_tests)
    assert not any(compatibility_tests.glob("test_*.py"))


def test_backend_compatibility_package_does_not_own_runtime_tests():
    runtime_tests = (
        EDITOR_ROOT / "runtime" / "tests" / "test_registry.py",
        EDITOR_ROOT / "runtime" / "tests" / "test_runtime_registry_boundary.py",
        EDITOR_ROOT / "runtime" / "tests" / "test_editor_host_layout_boundary.py",
    )
    script_tests = EDITOR_ROOT / "script_runtime" / "tests" / "test_script_runtime_runner.py"
    backend_tests = EDITOR_ROOT / "backend" / "tests"

    assert all(path.is_file() for path in runtime_tests)
    assert script_tests.is_file()
    assert not (backend_tests / "test_registry.py").exists()
    assert not (backend_tests / "test_script_runtime_runner.py").exists()


def test_legacy_editor_api_import_is_an_alias_of_the_canonical_module():
    editor_root = str(EDITOR_ROOT)
    if editor_root not in sys.path:
        sys.path.insert(0, editor_root)

    legacy = importlib.import_module("CoronaCore.core.editor_api")
    canonical = importlib.import_module("api.editor_api")

    assert legacy is canonical


def test_blockly_runtime_has_a_script_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "script_runtime" / "blockly" / "main.py"
    compatibility = EDITOR_ROOT / "backend" / "blockly" / "main.py"
    registry = (EDITOR_ROOT / "runtime" / "registry.py").read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert '_BLOCKLY_PACKAGE = "script_runtime"' in registry
    assert '"ScratchTool": (f"{_BLOCKLY_PACKAGE}.blockly.main", "ScratchTool")' in registry


def test_aitool_node_graph_loader_prefers_script_runtime_contract_package():
    source = (
        EDITOR_ROOT / "plugins" / "AITool" / "services" / "node_graph_generation_service.py"
    ).read_text(encoding="utf-8")

    assert "from script_runtime.blockly.ai_node_graph_contract import" in source


def test_aitool_node_graph_loader_does_not_depend_on_backend_compatibility_package():
    source = (
        EDITOR_ROOT / "plugins" / "AITool" / "services" / "node_graph_generation_service.py"
    ).read_text(encoding="utf-8")

    assert "backend.blockly" not in source


def test_main_view_delegates_generated_script_execution_to_script_runtime():
    runner = EDITOR_ROOT / "script_runtime" / "runner.py"
    main_view_source = (
        EDITOR_ROOT
        / "plugins"
        / "MainView"
        / "main.py"
    ).read_text(encoding="utf-8")

    assert runner.is_file()
    assert "from script_runtime.runner import run_generated_script" in main_view_source
    assert "from backend import runScript" not in main_view_source


def test_character_script_engine_has_a_script_runtime_canonical_owner():
    canonical = EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py"
    legacy = EDITOR_ROOT / "CoronaCore" / "core" / "scripts_system" / "corona_engine.py"
    generator_constants = (
        EDITOR_ROOT / "Frontend" / "src" / "blockly" / "generators" / "constants.js"
    ).read_text(encoding="utf-8")
    generator_prelude = (
        EDITOR_ROOT / "Frontend" / "src" / "blockly" / "generators" / "prelude.js"
    ).read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "Compatibility" in legacy.read_text(encoding="utf-8")
    assert "from script_runtime.engine import corona_engine" in generator_constants
    assert "from script_runtime.engine import corona_engine" in generator_prelude
    assert "CoronaCore.core.scripts_system" not in generator_constants
    assert "CoronaCore.core.scripts_system" not in generator_prelude


def test_script_runtime_does_not_invoke_editor_public_scene_tools_directly():
    source = (EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py").read_text(
        encoding="utf-8"
    )

    assert "CoronaEditorApi.scene_tools" not in source
    assert "CoronaEditorApi.scene_datas" in source
    assert "_invoke_cpp_editor_api" not in source


def test_script_runtime_uses_restricted_manifest_adapter_for_native_editor_state():
    editor_api_source = (EDITOR_ROOT / "api" / "editor_api.py").read_text(
        encoding="utf-8"
    )
    adapter = EDITOR_ROOT / "script_runtime" / "manifest_adapter.py"
    compatibility = EDITOR_ROOT / "CoronaCore" / "core" / "script_runtime_editor_api.py"
    cpp_source = (EDITOR_ROOT.parent / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp").read_text(
        encoding="utf-8"
    )

    assert "get_script_runtime_editor_api" in editor_api_source
    assert "from script_runtime.manifest_adapter import ScriptRuntimeEditorApi" in editor_api_source
    assert adapter.is_file()
    assert compatibility.is_file()
    assert "from script_runtime.manifest_adapter import" in compatibility.read_text(
        encoding="utf-8"
    )
    adapter_source = adapter.read_text(encoding="utf-8")
    for method in (
        "scene.list_routes",
        "scene.switch",
        "scene.get_snapshot",
        "scene.get_environment",
        "scene.set_environment",
        "scene.set_actor_transform",
        "scene_tools.create_actor",
        "scene_tools.remove_actor",
        "viewport.set_camera_pose",
    ):
        assert method in adapter_source
        assert f"{method}\"" in cpp_source
    assert "cef_and_script_runtime_callers())" in cpp_source


def test_script_runtime_native_capability_adapter_has_a_script_runtime_owner():
    canonical = EDITOR_ROOT / "script_runtime" / "native_engine_adapter.py"
    editor_api_source = (EDITOR_ROOT / "api" / "editor_api.py").read_text(
        encoding="utf-8"
    )
    engine_source = (
        EDITOR_ROOT / "script_runtime" / "engine" / "corona_engine.py"
    ).read_text(encoding="utf-8")

    assert canonical.is_file()
    assert "class _ScriptRuntimeAdapter" not in editor_api_source
    assert "from script_runtime.native_engine_adapter import" in engine_source


def test_legacy_script_engine_import_resolves_to_canonical_module():
    editor_root = str(EDITOR_ROOT)
    if editor_root not in sys.path:
        sys.path.insert(0, editor_root)
    legacy = importlib.import_module("CoronaCore.core.scripts_system.corona_engine")
    canonical = importlib.import_module("script_runtime.engine.corona_engine")

    assert legacy is canonical
