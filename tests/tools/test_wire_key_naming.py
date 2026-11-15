"""One spelling for every key that crosses the native boundary: snake_case.

The Web surface used to mix them - `scene.*` and every reply were snake_case, while the
window/panel requests and the window event payload were camelCase. Two spellings for the
same concept (`panel_id` in a reply, `panelId` in the request that produces it) is the bug
this guard prevents from coming back.

Web keeps camelCase *internally*; `appService.js` is the translator. Method names are not
covered here - those are camelCase JS identifiers, not data keys.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
BINDING = ROOT / "src/systems/ui/cef/cef_v8_binding.cpp"
APP_SERVICE = ROOT / "CoronaEditor/editor/Frontend/src/services/appService.js"
WINDOW_SESSION = ROOT / "CoronaEditor/editor/Frontend/src/services/editorWindowSession.js"

# A data key, wherever a reader looks one up or writes one out. Method installation
# (`SetValue("readScene", CefV8Value::CreateFunction(...))`) is excluded below.
KEY_SITES = re.compile(
    r'(?:object_keys\([^;]*?\{|option_string\(\s*\w+\s*,|option_integer\(\s*\w+\s*,|'
    r'HasValue\(|GetValue\(|SetValue\()\s*"([A-Za-z_][A-Za-z0-9_]*)"')
CAMEL = re.compile(r"[a-z][A-Z]")

# The one place a camelCase literal is still correct on this side: installing a Web method.
METHOD_INSTALL = "CefV8Value::CreateFunction"


def test_every_wire_key_in_the_binding_is_snake_case():
    offenders = []
    for match in KEY_SITES.finditer(BINDING.read_text(encoding="utf-8-sig")):
        key = match.group(1)
        if not CAMEL.search(key):
            continue
        # Skip method installation: `SetValue("readScene", CefV8Value::CreateFunction(...))`.
        tail = match.string[match.end():match.end() + 80]
        if METHOD_INSTALL in tail and key[:1].islower() and "." not in tail[:8]:
            continue
        offenders.append(key)
    assert not offenders, (
        f"camelCase keys on the native boundary: {sorted(set(offenders))}. Web keys are "
        "snake_case; translate internally, not at the wire")


def test_the_web_command_carries_the_wire_spelling():
    """The command object IS the wire object: no translation helper, no second vocabulary."""
    source = APP_SERVICE.read_text(encoding="utf-8-sig")
    assert "panelWire" not in source, "a translation helper came back; name the keys at the source"
    for key in ("panel_id:", "scene_id:", "camera_id:", "archive_session:", "route_path:"):
        assert key in source, f"appService.js no longer builds the '{key.rstrip(':')}' wire key"
    session = WINDOW_SESSION.read_text(encoding="utf-8-sig")
    for key in ("panel_id:", "scene_id:"):
        assert key in session, f"editorWindowSession.js still speaks its own spelling for '{key.rstrip(':')}'"


def test_window_ops_forward_the_command_untouched():
    source = APP_SERVICE.read_text(encoding="utf-8-sig")
    for call in ("editorApi.window.close(parameters)", "editorApi.window.focus(parameters)",
                 "editorApi.window.createPanel(parameters)",
                 "editorApi.window.setWindowPresentation(parameters)"):
        assert call in source, f"the window op re-maps its arguments instead of forwarding: {call}"


def test_web_window_calls_never_pass_a_camel_case_key():
    source = APP_SERVICE.read_text(encoding="utf-8-sig")
    camel_key = re.compile(r"[{,]\s*(panelId|sceneId|cameraId|archiveSession|routePath|windowToken)\s*:")
    for match in re.finditer(r"editorApi\.window\.(?:close|focus|createPanel|setWindowPresentation)\(", source):
        start = source.index("(", match.start())
        depth, index = 0, start
        while index < len(source):
            if source[index] == "(":
                depth += 1
            elif source[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        call = source[start:index + 1]
        found = camel_key.findall(call)
        assert not found, f"{match.group(0)} still passes camelCase keys {found}"
