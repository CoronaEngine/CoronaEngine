"""Compatibility import for the historical SceneTools service path.

The implementation is owned by ``plugins.SceneTools.compat`` because this
Python facade is only used by explicitly registered legacy hosts.  New editor
code must use the C++ manifest aggregate APIs.
"""

from plugins.SceneTools.compat.legacy_scene_tools import SceneTools
from plugins.SceneTools.compat.legacy_vision_import_adapter import (
    import_embedded_vision_scene_into_current_scene,
    import_vision_scene_into_current_scene,
    prepare_external_live_vision_scene,
)

__all__ = [
    "SceneTools",
    "import_embedded_vision_scene_into_current_scene",
    "import_vision_scene_into_current_scene",
    "prepare_external_live_vision_scene",
]
