"""The workspace pins are text in build_tools/source_workspaces.py.

``dev.py pin`` rewrites that text, so this guard round-trips the rewrite on a
copy: it must change exactly one workspace, keep the others, be idempotent and
reject anything that is not a full lowercase commit.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from build_tools.source_workspaces import WORKSPACES, find_workspace, rewrite_pin

SOURCE = ROOT / "build_tools/source_workspaces.py"


def test_every_pin_is_a_full_lowercase_commit():
    for workspace in WORKSPACES:
        assert len(workspace.commit) == 40, workspace
        assert workspace.commit == workspace.commit.lower(), workspace


def test_rewrite_pin_round_trips_a_copy(tmp_path):
    copy = tmp_path / "source_workspaces.py"
    copy.write_text(SOURCE.read_text(encoding="utf-8"), encoding="utf-8")
    first = WORKSPACES[0]
    replacement = "a" * 40

    assert rewrite_pin(first.key, replacement, copy) is True
    text = copy.read_text(encoding="utf-8")
    assert f'commit="{replacement}"' in text
    for other in WORKSPACES[1:]:
        assert f'commit="{other.commit}"' in text, f"{other.key} pin must not move"

    assert rewrite_pin(first.key, replacement, copy) is False, "rewriting the same pin changes nothing"
    assert rewrite_pin(first.key, first.commit, copy) is True, "the original pin comes back"
    assert f'commit="{first.commit}"' in copy.read_text(encoding="utf-8")


def test_rewrite_pin_refuses_anything_but_a_full_commit(tmp_path):
    copy = tmp_path / "source_workspaces.py"
    copy.write_text(SOURCE.read_text(encoding="utf-8"), encoding="utf-8")
    for bad in ("", "abc", "A" * 40, "0" * 41, "0" * 40 + " "):
        try:
            rewrite_pin("editor", bad, copy)
        except ValueError:
            continue
        raise AssertionError(f"{bad!r} must be rejected")
    try:
        rewrite_pin("nope", "a" * 40, copy)
    except ValueError:
        pass
    else:
        raise AssertionError("an unknown workspace key must be rejected")


def test_find_workspace_knows_every_key():
    for workspace in WORKSPACES:
        assert find_workspace(workspace.key) is workspace
