"""The failure a script operation raises.

`CoronaEngine.<group>.<name>` raises this with the same symbolic code the Web surface
rejects with (`PROJECT_CONTEXT_CHANGED`, `ACTOR_NOT_FOUND`, `READ_ONLY`, ...). Branch on
`code`; `message` is for logs. The numeric classes the C++ layer also carries
(400/403/404/409) are deliberately not exposed - they exist for the C++ tests and the
transport, not for scripts.
"""


class EditorApiError(Exception):
    def __init__(self, code, message):
        self.code = code
        self.message = message
        super().__init__(f"{code}: {message}" if code else message)


__all__ = ["EditorApiError"]
