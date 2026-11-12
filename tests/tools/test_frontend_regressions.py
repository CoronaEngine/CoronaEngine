"""Run the frontend test files that guard the Web -> native window/audio/facade area.

The frontend suite cannot be run in bulk on this checkout — several files use
Node 22 APIs or jsdom and hang or crash under plain `node --test` — so the engine
repo had no regression net for the area it keeps changing. This guard runs the
curated files that are fast and deterministic one at a time, each under its own
timeout, and reports the first failing one.

It skips when the editor checkout or node is missing: the checkout is a build
input pinned by build_tools/source_workspaces.py, not a repo file.
"""

from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
FRONTEND = ROOT / "CoronaEditor" / "editor" / "Frontend"

# Every file here covers the facade, the window session, audio, or a native
# binding the interface contract promises. Add a file only after it is green
# when run alone.
FILES = (
    "editorApiWindowWrapper.test.mjs",
    "nativeWindowBindings.test.mjs",
    "editorWindowSession.test.mjs",
    "windowRetirement.test.mjs",
    "appDockLayout.test.mjs",
    "appWorldInitialization.test.mjs",
    "audioTypedBoundary.test.mjs",
    "nativeFiles.test.mjs",
    "nativeScriptInput.test.mjs",
    "autosave.test.mjs",
    "viewportGizmo.test.mjs",
)
TIMEOUT_SECONDS = 180


def _skip_reasons() -> list[str]:
    reasons = []
    if not (FRONTEND / "tests" / "js").is_dir():
        reasons.append(f"{FRONTEND} is missing (run `python build_tools/dev.py editor-sync`)")
    if shutil.which("node") is None:
        reasons.append("node is not on PATH")
    return reasons


@pytest.mark.parametrize("name", FILES)
def test_frontend_file_still_passes(name):
    for reason in _skip_reasons():
        pytest.skip(reason)
    path = FRONTEND / "tests" / "js" / name
    assert path.is_file(), f"{name} is missing from the pinned checkout"
    result = subprocess.run(["node", "--test", str(path)], cwd=FRONTEND,
                            capture_output=True, text=True, timeout=TIMEOUT_SECONDS)
    assert result.returncode == 0, (
        f"{name} failed:\n{result.stdout[-4000:]}\n{result.stderr[-2000:]}")
