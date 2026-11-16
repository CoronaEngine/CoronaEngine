"""The retired JSON manifest must not come back.

The manifest (api-name table, caller masks, `EditorApiRegistry`, `NativeApiRegistry`
and the two `_invoke_cpp_*` bindings) is deleted. Web talks to C++ through the typed
bindings. These guards pin both halves and the absence of the third.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
PY = ROOT / "embedded_python"
BINDINGS = ROOT / "src/systems/ui/cef/cef_py_bind.cpp"

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

# The public spelling: (namespace, operation) pairs the nanobind module installs. The names
# come from editor_operation_table.h; the handlers above are the internal implementation.
SCRIPT_BINDINGS = [
    ("scene", "read_scene"),
    ("scene", "read_active"),
    ("scene", "create_actor"),
    ("scene", "read_actor"),
    ("scene", "read_transforms"),
    ("scene", "set_actor_transform"),
    ("scene", "update_actor"),
    ("scene", "delete_actor"),
    ("scene", "read_environment"),
    ("scene", "set_environment"),
    ("scene", "read_archive_scene"),
    ("scene", "set_actor_animation"),
    ("scene", "pause_actor_animation"),
    ("scene", "resume_actor_animation"),
    ("scene", "set_actor_physics"),
    ("audio", "play"),
    ("audio", "stop"),
    ("viewport", "set_camera_pose"),
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


def test_web_still_cannot_reach_any_editor_manifest():
    # The typed V8 surface is the only Web contract; there is no name-dispatched
    # bridge left for a document to reach.
    for path, text in _sources():
        if "cef_v8_binding.cpp" not in path.name:
            continue
        assert "EditorApi" not in text, "the V8 binding must not grow a manifest escape hatch"
