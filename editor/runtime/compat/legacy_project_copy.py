"""Compatibility import for the legacy project copy facade.

The implementation lives in :mod:`runtime.legacy_project_copy`; this module
preserves the historical import path for older hosts and project tools.
"""

from runtime import legacy_project_copy as _legacy_project_copy
from runtime.legacy_project_copy import *  # noqa: F401,F403

core_path = _legacy_project_copy.core_path


class ProjectCopy(_legacy_project_copy.ProjectCopy):
    """Compatibility class that preserves the historical ``core_path`` hook."""

    @staticmethod
    def copy_existing_to_data(project_ini_path):
        _legacy_project_copy.core_path = core_path
        return _legacy_project_copy.ProjectCopy.copy_existing_to_data(project_ini_path)
