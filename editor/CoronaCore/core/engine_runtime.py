"""Compatibility import path for the legacy engine adapter."""

from runtime.legacy_engine_adapter import (
    CoronaEngine,
    EditorEngineAdapter,
    get_editor_engine_adapter,
)

__all__ = ["CoronaEngine", "EditorEngineAdapter", "get_editor_engine_adapter"]
