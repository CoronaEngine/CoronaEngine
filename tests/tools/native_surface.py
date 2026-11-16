"""The exact Web surface ``window.corona`` publishes.

Written out by hand on purpose. Every name here is also a row in
``editor_operation_table.h``, and ``test_typed_native_surface.py`` asserts the table
reproduces this list exactly - if both sides were derived from the table, a wrong row
would silently redefine the surface instead of failing. Read by
``test_typed_native_surface.py`` (what the binding has to install) and
``test_editor_operation_table.py`` (the rows the camelCase spelling is derived from).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from operation_table import camel_case as _camel_case  # noqa: E402  (one rule, one home)

EXPECTED_SURFACE = {
    "app": {"closeProcess", "isVisionAvailable"},
    # One verb per action; the identity names a resource or an actor of a scene.
    "audio": {"play", "stop"},
    "bridge": {"call"},
    "input": {"dispatch"},
    "network": {
        "clearDiscoveredPeers", "connectToPeer", "getDiscoveredPeers", "getSessionInfo",
        "searchLan", "setProjectRoot", "startSession", "stopSession",
    },
    "project": {
        "createMultiplayerProject", "createWorldProject",
        "getProjectLoadStatus", "getRecentProjects", "openDocument", "openProject",
        "openProjectFile", "saveActiveProjectInfo",
    },
    "scene": {
        "createActor", "createCamera", "deleteActor", "deleteCamera", "focusActor",
        "importResourceFile", "readActive", "readActorProperties", "readActorRig",
        "readCameraRenderSettings", "readEnvironment", "readModelNodes", "readScene",
        "readTransforms", "rebindActorResource", "save", "selectActor", "selectModelFile",
        "setActorIkChains", "setActorTransform", "setCameraRenderSettings", "setEnvironment",
        "setModelNodeState", "updateActor", "updateCamera",
    },
    "viewport": {"captureScreenshot", "configure"},
    "window": {
        "close", "createPanel", "focus",
        "onCloseRequested", "onClosed", "setWindowPresentation",
    },
}


def camel_case(snake: str) -> str:
    """The derivation the C++ table uses: "read_actor_rig" -> "readActorRig"."""
    return _camel_case(snake)
