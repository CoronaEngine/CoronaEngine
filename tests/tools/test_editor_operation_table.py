"""The one operation table is the only place an operation name is declared.

Every name the Web surface publishes is a row here, and the binding installs
``camel_case(name)`` for it. The ScriptRuntime projection went with the script runtime and
will be redesigned, so the table is Web-only. This guard pins the table's own shape: 55
rows across the nine ``window.corona`` namespaces, each row naming the enum that owns it,
unique (group, name), a summary for every row, and the camelCase derivation.
"""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import operation_table as table

rows = table.rows

# Window, project, network, input and bridge used to be installed by hand in the binding;
# they are table rows now, which is what took the count from 29 to 55.
NAMESPACES = {"app", "audio", "bridge", "input", "network", "project", "scene",
              "viewport", "window"}

# The enums `EditorMethod` (cef_v8_binding.cpp's std::visit) can dispatch, plus the window
# event kind that installs a subscription instead of an operation.
METHOD_OWNERS = {"SceneMethod", "NetworkMethod", "ProjectLauncherMethod", "FilesMethod",
                 "ProjectSettingsMethod", "WindowMethod", "ScriptInputMethod",
                 "BridgeMethod", "NativeEventKind"}


def test_the_table_is_fifty_five_rows():
    assert len(rows()) == 55, f"expected 55 rows, found {len(rows())}"


def test_every_row_names_a_method_owner_the_binding_dispatches():
    # The parser only yields rows with a `Enum::Member` method, so a row whose enum is
    # misspelt (`ScenMethod::ReadScene`) still parses - and only the build would catch it.
    unknown = sorted({row["enum"] for row in rows()} - METHOD_OWNERS)
    assert not unknown, f"rows naming an enum the binding cannot dispatch: {unknown}"


def test_the_table_covers_the_nine_web_namespaces():
    groups = {row["group"] for row in rows()}
    assert groups == NAMESPACES, (
        f"namespaces drifted: missing {sorted(NAMESPACES - groups)}, "
        f"unknown {sorted(groups - NAMESPACES)}")


def test_group_and_name_are_unique():
    keys = [(row["group"], row["name"]) for row in rows()]
    assert len(keys) == len(set(keys)), "duplicate (group, name) rows"


def test_camel_case_matches_the_published_js_spelling():
    # The C++ camel_case() must produce exactly these; they are the spellings
    # test_typed_native_surface.EXPECTED_SURFACE already pins on the Web side.
    expected = {
        "read_scene": "readScene",
        "read_active": "readActive",
        "read_actor_properties": "readActorProperties",
        "read_actor_rig": "readActorRig",
        "set_actor_ik_chains": "setActorIkChains",
        "select_model_file": "selectModelFile",
        "is_vision_available": "isVisionAvailable",
        "capture_screenshot": "captureScreenshot",
        "read_camera_render_settings": "readCameraRenderSettings",
        "create_panel": "createPanel",
        "save_active_project_info": "saveActiveProjectInfo",
        "on_close_requested": "onCloseRequested",
        "set_project_root": "setProjectRoot",
    }
    derived = {name: table.camel_case(name) for name in expected}
    assert derived == expected, f"camelCase rule drift: {derived}"


def test_summaries_are_not_empty():
    missing = [f"{row['group']}.{row['name']}" for row in rows() if not row["summary"].strip()]
    assert not missing, f"rows without a summary: {missing}"
