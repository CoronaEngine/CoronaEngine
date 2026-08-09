"""Compatibility import for the historical project copy path.

The proxy preserves the legacy module-level ``core_path`` injection used by
older hosts while keeping the implementation in the compat owner.
"""

from runtime.compat import legacy_project_copy as _legacy_project_copy
from runtime.compat.legacy_project_copy import *  # noqa: F401,F403

core_path = _legacy_project_copy.core_path


class _ProjectCopyShim(_legacy_project_copy.ProjectCopy):
    """Forward the historical path injection to the compatibility owner."""

    @staticmethod
    def copy_existing_to_data(project_ini_path):
        _legacy_project_copy.core_path = core_path
        return _legacy_project_copy.ProjectCopy.copy_existing_to_data(project_ini_path)


ProjectCopy = _ProjectCopyShim
