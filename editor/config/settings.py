"""Compatibility facade for legacy imports of editor project state.

Project state is implemented in :mod:`config.project_state`.  New code should
import that module directly; this path remains for older hosts and scripts.
"""

from .paths_config import PathsConfig, get_default_paths
from .project_state import CoronaSettings, settings_manager, version

core_path = get_default_paths()

__all__ = [
    "CoronaSettings",
    "PathsConfig",
    "core_path",
    "get_default_paths",
    "settings_manager",
    "version",
]
