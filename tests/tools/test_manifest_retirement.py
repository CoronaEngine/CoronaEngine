"""The retired JSON manifest must not come back.

The manifest (api-name table, caller masks, `EditorApiRegistry`, `NativeApiRegistry`
and the two `_invoke_cpp_*` bindings) is deleted. Web talks to C++ through the typed
V8 contract; the ScriptRuntime talks to it through sixteen named `script_scene_*`
bindings. These guards pin both halves and the absence of the third.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
PY = ROOT / "embedded_python"
BINDINGS = ROOT / "src/systems/ui/cef/cef_py_bind.cpp"
OPERATIONS = ROOT / "src/systems/ui/cef/script_runtime_api.h"

RETIRED_SYMBOLS = [
    "EditorApiRegistry",
    "NativeApiRegistry",
    "NativeHandlerFn",
    "NativeMethodTable",
    "kEditorApiMethods",
    "kEditorApiEvents",
    "register_builtin_native_api_handlers",
    "dispatch_method",
    "_invoke_cpp_editor_api",
    "_invoke_cpp_script_api",
    "EditorApiCallbackRegistry",
]

SCRIPT_RUNTIME_OPERATIONS = [
    "script_scene_get_environment",
    "script_scene_read_active_scene",
    "script_scene_read_actor",
    "script_scene_read_transforms",
    "script_scene_read_archive_scene",
    "script_scene_get_scene_snapshot",
    "script_scene_set_actor_transform",
    "script_scene_set_environment",
    "script_scene_set_actor_state",
    "script_scene_set_actor_animation",
    "script_scene_pause_actor_animation",
    "script_scene_resume_actor_animation",
    "script_scene_set_actor_physics",
    "script_scene_set_camera_pose",
    "script_scene_create_actor",
    "script_scene_remove_actor",
]


def _sources():
    for path in SRC.rglob("*"):
        if path.suffix in {".cpp", ".h"} and path.is_file():
            yield path, path.read_text(encoding="utf-8-sig", errors="replace")


def test_no_manifest_symbol_survives_in_cpp():
    hits = []
    for path, text in _sources():
        for symbol in RETIRED_SYMBOLS:
            if re.search(rf"\b{symbol}\b", text):
                hits.append(f"{path.relative_to(ROOT)}:{symbol}")
    assert not hits, f"retired manifest symbols came back: {hits}"


def test_the_manifest_files_are_gone():
    # The registry translation unit survives as the value-type home (request/result
    # structs, argument readers, the in-flight watchdog) but carries no registry.
    header = (ROOT / "src/systems/ui/cef/native_api_types.h").read_text(encoding="utf-8-sig")
    assert "class NativeApiRegistry" not in header
    assert "NativeHandlerFn" not in header
    source = (ROOT / "src/systems/ui/cef/native_api_types.cpp").read_text(encoding="utf-8-sig")
    assert "NativeApiRegistry" not in source
    assert "register_module" not in source
    # the editor-side manifest unit is gone entirely
    assert not (ROOT / "src/systems/ui/editor_api/cef_editor_api_table.cpp").exists()


def test_python_has_no_manifest_channel():
    offenders = []
    for path in PY.rglob("*.py"):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for symbol in ["_invoke_cpp_editor_api", "_invoke_cpp_script_api", "CoronaEditorApi",
                       "manifest_adapter", "_invoke_manifest"]:
            if symbol in text:
                offenders.append(f"{path.relative_to(ROOT)}:{symbol}")
    assert not offenders, f"Python still speaks the manifest: {offenders}"


def test_script_runtime_contract_is_the_named_bindings():
    header = OPERATIONS.read_text(encoding="utf-8-sig")
    declared = re.findall(r"NativeResult (script_scene_\w+)\(", header)
    assert sorted(declared) == sorted(SCRIPT_RUNTIME_OPERATIONS), (
        f"the named contract changed: missing {sorted(set(SCRIPT_RUNTIME_OPERATIONS) - set(declared))}, "
        f"extra {sorted(set(declared) - set(SCRIPT_RUNTIME_OPERATIONS))}")

    bindings = BINDINGS.read_text(encoding="utf-8-sig")
    bound = re.findall(r'bind_script_operation\("(script_scene_\w+)"', bindings)
    assert sorted(bound) == sorted(SCRIPT_RUNTIME_OPERATIONS), (
        "every named operation must be bound exactly once")


def test_web_still_cannot_reach_any_editor_manifest():
    # The typed V8 surface is the only Web contract; there is no name-dispatched
    # bridge left for a document to reach.
    for path, text in _sources():
        if "cef_v8_binding.cpp" not in path.name:
            continue
        assert "EditorApi" not in text, "the V8 binding must not grow a manifest escape hatch"
