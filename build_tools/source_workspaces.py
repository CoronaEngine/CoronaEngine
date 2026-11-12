"""Source dependency baselines. Change these commits to update a pinned version.

Build preparation aligns local HEAD to these pins, preserving nonconflicting
edits. A different live remote branch tip only produces a warning.
Each pinned commit must already contain the functionality the engine requires;
source preparation does not apply engine-owned patches to either workspace.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class SourceWorkspace:
    key: str
    name: str
    directory: Path
    url: str
    ref: str
    commit: str

    def __post_init__(self) -> None:
        if not re.fullmatch(r"[a-z][a-z0-9-]*", self.key):
            raise ValueError(f"Invalid workspace key: {self.key}")
        if not self.name or not self.url or not self.ref or not re.fullmatch(r"[0-9a-f]{40}", self.commit):
            raise ValueError(f"Invalid source baseline for {self.key}; use a full lowercase commit SHA")
        if self.directory.is_absolute() or not self.directory.parts or ".." in self.directory.parts:
            raise ValueError(f"Workspace directory must be inside the repository: {self.directory}")

    @property
    def sync_command(self) -> str:
        return f"python build_tools/dev.py {self.key}-sync"

    @property
    def lock_hint(self) -> str:
        return f"edit build_tools/source_workspaces.py and run '{self.sync_command}'"


HORIZON = SourceWorkspace(
    key="horizon",
    name="Horizon",
    directory=Path("Horizon"),
    url="https://github.com/CoronaEngine/Horizon.git",
    ref="main",
    commit="d8c140168c386c28ae027eea0938fd1de42dbae3",
)

EDITOR = SourceWorkspace(
    key="editor",
    name="CoronaEditor",
    directory=Path("CoronaEditor"),
    url="https://github.com/CoronaEngine/CoronaEditor.git",
    ref="codex/remove-story-world",
    commit="d4c6882ddf113165d7e6e5e9a93bd402270f0591",
)

WORKSPACES = (HORIZON, EDITOR)


def find_workspace(key: str) -> SourceWorkspace:
    for workspace in WORKSPACES:
        if workspace.key == key:
            return workspace
    raise ValueError(f"Unknown workspace: {key}; choose one of {', '.join(w.key for w in WORKSPACES)}")


def rewrite_pin(key: str, commit: str, path: Path | None = None) -> bool:
    """Point one workspace's pin at ``commit``.

    ``path`` defaults to this file and exists so the guard can round-trip a copy.
    Returns whether the file changed; the pin lives in the source itself, so the
    caller must re-import the module (or start a new process) to see the new value.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError(f"Pin must be a full lowercase commit: {commit}")
    find_workspace(key)
    path = (path or Path(__file__)).resolve()
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r'(key="' + re.escape(key) + r'",[\s\S]*?commit=")([0-9a-f]{40})(")')
    updated, count = pattern.subn(lambda match: match.group(1) + commit + match.group(3), text, count=1)
    if count != 1:
        raise ValueError(f"Could not find the pin for {key} in {path.name}")
    if updated == text:
        return False
    path.write_text(updated, encoding="utf-8")
    return True
