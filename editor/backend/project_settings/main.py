"""Compatibility import for the canonical ProjectSettings plugin.

The service used to live under the historical ``backend`` package. Keep this
module for old hosts and generated imports; new registrations must use
``plugins.ProjectSettings.main``.
"""

from plugins.ProjectSettings.main import ProjectSettings

__all__ = ["ProjectSettings"]
