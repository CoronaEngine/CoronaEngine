"""Compatibility facade for historical project support imports.

Canonical implementations are split by responsibility between
``project_templates`` and ``scene_support``.  Older hosts may continue to
import this module while they migrate.
"""

from runtime.project_templates import (
    create_actor_from_template,
    create_project_from_template,
    create_scene_from_template,
    normalize_project_runtime_paths,
    update_config_name,
    update_project_config,
)
from runtime.scene_support import (
    append_project_scene,
    auto_save,
    cancel_pending_auto_saves,
    flush_pending_auto_saves,
    get_project_scenes,
    set_project_scenes,
)

__all__ = [
    "append_project_scene",
    "auto_save",
    "cancel_pending_auto_saves",
    "create_actor_from_template",
    "create_project_from_template",
    "create_scene_from_template",
    "flush_pending_auto_saves",
    "get_project_scenes",
    "normalize_project_runtime_paths",
    "set_project_scenes",
    "update_config_name",
    "update_project_config",
]
