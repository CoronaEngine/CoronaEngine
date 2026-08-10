"""AITool integration factories owned by the composition root.

The service modules consume the resulting objects through explicit injection.
This module is the only composition boundary for optional Quasar/Agent
implementations used by the LANChat worker.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from typing import Any


def create_engine_write_gate() -> Any:
    """Construct the optional engine gate at the integration boundary."""
    try:
        from .engine_write_gate import get_engine_write_gate

        return get_engine_write_gate()
    except Exception:
        return None


def create_scene_element_classifier() -> Any:
    """Load the pure scene classifier without importing the heavy Agent package."""
    try:
        module_name = "_aitool_scene_element_classifier"
        module_path = (
            Path(__file__).resolve().parents[1]
            / "services"
            / "scene_element_classifier.py"
        )
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        if spec is None or spec.loader is None:
            return None
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return module
    except Exception:
        return None


__all__ = [
    "create_engine_write_gate",
    "create_scene_element_classifier",
]
