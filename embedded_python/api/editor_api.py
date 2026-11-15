"""The Python side of the editor contract.

C++ exposes `CoronaEngine.scene` / `.audio` / `.viewport`. Their methods mirror
`window.corona.*` one-for-one - same namespace, same name in snake_case, same argument
positions - and raise `api.errors.EditorApiError` with the symbolic code the Web surface
rejects with. This module is the only entry point role scripts are meant to use; ordinary
editor Python has no editor API at all, and the Web talks to C++ through the typed V8
contract.
"""

from script_runtime.native_scene_adapter import ScriptRuntimeEditorApi


def get_script_runtime_editor_api():
    """Return the restricted ScriptRuntime editor API."""
    return ScriptRuntimeEditorApi()
