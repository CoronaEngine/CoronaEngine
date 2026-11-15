"""The ScriptRuntime's restricted view of the editor.

C++ installs `CoronaEngine.scene` / `.audio` / `.viewport`; this module is the only place a
role script is meant to reach them through, so the permission boundary stays a module import
rather than a runtime check. Ordinary editor Python never imports it.

The methods mirror `window.corona.*` one-for-one: same namespace, same argument positions,
snake_case spelling. A failure raises `api.errors.EditorApiError` carrying the symbolic code
the Web surface rejects with.
"""


class ScriptRuntimeEditorApi:
    """`scene` / `audio` / `viewport`, exactly the namespaces the C++ module publishes.

    Nothing is wrapped here on purpose: a wrapper would be a second place for a name or an
    argument order to drift, which is the bug this module replaced.
    """

    def __init__(self):
        import CoronaEngine

        self.scene = CoronaEngine.scene
        self.audio = CoronaEngine.audio
        self.viewport = CoronaEngine.viewport


__all__ = ["ScriptRuntimeEditorApi"]
