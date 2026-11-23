"""Typed refusals raised by the Python bridge services.

A refusal names the business code the Web caller branches on. ``CoronaEditor.
dispatch_script_values_from_cpp`` returns it in the ``code`` field of its failure
envelope, next to a human-readable message; the C++ bridge copies ``code`` into the
Promise rejection and never parses the message. Codes are declared once in
``tests/tools/rejection_codes.py`` so the guard and the contract agree.
"""


class BridgeError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code or "SERVICE_FAILED"
        self.message = message or ""


__all__ = ["BridgeError"]
