"""Declared request shapes for the Python services.

The C++ reader rejects unknown keys (``object_keys``); this is the same rule on the Python
side. A misspelled key is an error on both boundaries instead of being silently ignored
here, which is what made the two layers feel different.

No service uses this yet beyond ``file_manager``: the remaining methods migrate one commit
at a time, and the guard test keeps the two sides' strictness in sync.
"""
from __future__ import annotations

from runtime.bridge_errors import BridgeError


def require_keys(request, required=(), optional=()):
    """Return ``request`` after checking it only carries declared keys.

    ``required`` are the keys the operation cannot run without; ``optional`` are the ones
    it reads when present. Anything else is a mistake on the caller's side and is refused
    with the same code the C++ readers use.
    """
    if not isinstance(request, dict):
        raise BridgeError("INVALID_ARGUMENT", "Request must be an object")
    allowed = set(required) | set(optional)
    unknown = sorted(set(request) - allowed)
    if unknown:
        raise BridgeError("INVALID_ARGUMENT", f"Unexpected request key: {', '.join(unknown)}")
    missing = [key for key in required if key not in request]
    if missing:
        raise BridgeError("INVALID_ARGUMENT", f"Missing request key: {', '.join(missing)}")
    return request


def require_choice(request, key, choices):
    """Return ``request[key]``, refusing a value outside ``choices``."""
    value = request.get(key)
    if value not in choices:
        rendered = ", ".join(sorted(choices))
        raise BridgeError("INVALID_ARGUMENT", f"{key} must be one of: {rendered}")
    return value
