"""The exact Web surface ``window.corona`` publishes.

Shared by the two guards that police it: ``test_typed_native_surface.py`` (what the binding
installs) and ``test_editor_operation_table.py`` (the canonical names the camelCase spelling
is derived from). One list, so a namespace cannot grow a method in one test and not the other.
"""

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
    head, *tail = snake.split("_")
    return head + "".join(part.capitalize() for part in tail)
