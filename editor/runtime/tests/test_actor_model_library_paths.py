import sys
from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

from runtime.legacy.entities.actor import Actor


def test_actor_rewrites_canonical_model_library_resources():
    assert (
        Actor._local_model_library_resource_subdir(
            "Resource/local_model_library/models/chair/runtime/base.glb"
        )
        == "local_model_library/models/chair/runtime"
    )


def test_actor_keeps_legacy_model_library_resources_compatible():
    assert (
        Actor._local_model_library_resource_subdir(
            "assets/local_model_library/models/chair/runtime/base.glb"
        )
        == "local_model_library/models/chair/runtime"
    )
