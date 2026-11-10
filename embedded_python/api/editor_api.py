"""The Python side of the editor contract.

The JSON manifest is retired. C++ now exposes one named binding per operation
(`script_scene_<operation>`), and the restricted adapter in
`script_runtime/native_scene_adapter.py` is the only Python consumer. Ordinary
editor Python has no editor API: the Web talks to C++ through the typed V8
contract, and role scripts are confined to the restricted surface.
"""

from script_runtime.native_scene_adapter import ScriptRuntimeEditorApi


def get_script_runtime_editor_api():
    """Return the restricted ScriptRuntime editor API."""
    return ScriptRuntimeEditorApi()
