"""Optional workflow test-mode support.

The old workflow fixture modules were removed from the runtime tree.  Keeping
an empty, importable contract lets production workflows load normally while
making the retired ``workflow_test`` mode a no-op instead of a startup error.
"""

from __future__ import annotations

from typing import Any, Dict


DEFAULT_MODELS: list[Dict[str, str]] = []
DEFAULT_PROMPT = ""


def get_test_case(_test_case_key: str = "default") -> Dict[str, Any]:
    return {}


__all__ = ["DEFAULT_MODELS", "DEFAULT_PROMPT", "get_test_case"]
