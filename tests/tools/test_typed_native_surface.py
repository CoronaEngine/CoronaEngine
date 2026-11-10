"""The typed native surface is the only contract Web has.

With the JSON manifest retired there is exactly one way for the renderer to reach
C++: the `window.corona` object installed by `attach_corona_v8_binding`, plus its
single `bridge.call` entry to Python.  These guards pin that surface exactly, so
a namespace cannot quietly grow a second transport or lose a method.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V8 = ROOT / "src/systems/ui/cef/cef_v8_binding.cpp"
FRONTEND_SRC = ROOT / "CoronaEditor/editor/Frontend/src"

EXPECTED_SURFACE = {
    "app": {"closeProcess"},
    "audio": {"playActor", "playResource", "stopActor", "stopResource"},
    "bridge": {"call"},
    "files": {"openFile"},
    "gizmo": {"pointer", "setTarget"},
    "input": {"dispatch"},
    "network": {
        "clearDiscoveredPeers", "connectToPeer", "getDiscoveredPeers", "getSessionInfo",
        "searchLan", "setProjectRoot", "startSession", "stopSession",
    },
    "project": {
        "createMultiplayerProject", "createWorldProject", "getAppVersion",
        "getProjectLoadStatus", "getRecentProjects", "openProject", "openProjectFile",
    },
    "project_settings": {"saveActiveProjectInfo"},
    "scene": {
        "captureScreenshot", "createActor", "createCamera", "deleteActor", "deleteCamera",
        "focusActor", "getEnvironment", "importResourceFile", "isVisionAvailable",
        "readActive", "readActorProperties", "readActorRig", "readCameraRenderSettings",
        "readModelNodes", "readScene", "readTransforms", "rebindActorResource", "save",
        "selectActor", "selectModelFile", "setActorIkChains", "setActorTransform",
        "setCameraRenderSettings", "setEnvironment", "setModelNodeState", "updateActor",
        "updateCamera",
    },
    "viewport": {"configure", "pick"},
    "window": {
        "close", "closeCameraView", "closeTab", "createCameraView", "createPanel",
        "cycleCameraWindowMode", "detachPanel", "focusTab", "getCurrent",
        "onCloseRequested", "onClosed", "redockPanel", "resizeCameraView",
        "setDragRegions", "suspendCameraViews", "toggleCameraBorderless",
        "toggleCameraMaximized", "toggleMainBorderless",
    },
}


def _binding_source() -> str:
    return V8.read_text(encoding="utf-8-sig")


def _installed_surface() -> dict[str, set[str]]:
    import re

    pairs = re.findall(r'(\w+)->SetValue\("([^"]+)", CefV8Value::CreateFunction', _binding_source())
    surface: dict[str, set[str]] = {}
    for namespace, name in pairs:
        surface.setdefault(namespace, set()).add(name)
    return surface


def test_window_corona_surface_is_exactly_the_named_contract():
    assert _installed_surface() == EXPECTED_SURFACE


def test_corona_is_the_only_global_installed():
    source = _binding_source()
    assert source.count("GetGlobal()->SetValue(\"corona\"") == 1


def test_web_to_python_bridge_stays_one_function_wide():
    assert _installed_surface()["bridge"] == {"call"}


def test_no_json_transport_survives():
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".cpp", ".h"} or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for retired in ("cefQuery", "CefMessageRouter", "OnQuery"):
            assert retired not in text, f"{retired} came back in {path.relative_to(ROOT)}"


def test_frontend_reaches_native_only_through_the_facade():
    offenders = []
    for path in FRONTEND_SRC.rglob("*"):
        if path.suffix not in {".js", ".vue", ".mjs"} or not path.is_file():
            continue
        if path.name == "editorApi.js":
            continue
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        if "window.corona" in text:
            offenders.append(str(path.relative_to(ROOT)))
    assert not offenders, f"native calls outside the facade: {offenders}"
