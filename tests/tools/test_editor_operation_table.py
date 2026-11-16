"""The one operation table is the only place an operation name is declared.

Every name the Web surface publishes is a row here, and the binding publishes the row's
own ``name`` - the wire spelling and the JS spelling are the same snake_case string. The
ScriptRuntime projection went with the script runtime and will be redesigned, so the table
is Web-only. This guard pins the table's own shape: 55 rows across the nine
``window.corona`` namespaces, each row naming the enum that owns it, unique (group, name),
a summary for every row, and a JS-safe identifier for every name.
"""
from pathlib import Path
import re
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


def test_every_name_is_published_verbatim_as_a_js_identifier():
    # The row name is the wire name now: it has to be a snake_case JS identifier, because the
    # binding installs it as a property and the facade calls it as `editorApi.<ns>.<name>`.
    bad = [f"{row['group']}.{row['name']}" for row in rows()
           if not re.fullmatch(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)*", row["name"])]
    assert not bad, f"row names that are not snake_case identifiers: {bad}"


def test_summaries_are_not_empty():
    missing = [f"{row['group']}.{row['name']}" for row in rows() if not row["summary"].strip()]
    assert not missing, f"rows without a summary: {missing}"
