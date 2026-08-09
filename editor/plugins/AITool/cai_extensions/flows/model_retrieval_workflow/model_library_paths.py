"""Filesystem ownership for the persistent project model library."""

from __future__ import annotations

from pathlib import Path


_CANONICAL_RELATIVE_ROOT = Path("Resource") / "local_model_library"
_LEGACY_RELATIVE_ROOT = Path("assets") / "local_model_library"


def canonical_model_library_root(project_root: str | Path) -> Path:
    return Path(project_root).resolve() / _CANONICAL_RELATIVE_ROOT


def legacy_model_library_root(project_root: str | Path) -> Path:
    return Path(project_root).resolve() / _LEGACY_RELATIVE_ROOT


def resolve_model_library_root(
    project_root: str | Path,
    *,
    for_write: bool = False,
) -> Path:
    """Resolve the model library, writing only to the canonical resource root.

    Existing libraries under ``assets/local_model_library`` remain readable so
    projects can migrate without losing their cached models.
    """
    canonical = canonical_model_library_root(project_root)
    legacy = legacy_model_library_root(project_root)
    if not for_write and not canonical.exists() and legacy.exists():
        return legacy
    return canonical


__all__ = [
    "canonical_model_library_root",
    "legacy_model_library_root",
    "resolve_model_library_root",
]
