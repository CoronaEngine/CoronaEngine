"""Typed refusals raised by the Web bridge services.

A refusal names the business code the Web caller branches on. ``CoronaEditor.
dispatch_script_values_from_cpp`` turns it into a ``"CODE: message"`` failure, and the
C++ bridge extracts that prefix so the Promise rejects with the code instead of the
generic ``SERVICE_FAILED`` bucket.
"""


class BridgeError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code or "SERVICE_FAILED"
        self.message = message or ""


__all__ = ["BridgeError"]
