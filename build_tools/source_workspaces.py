"""Source dependency baselines. Change these commits to update a pinned version.

Build preparation aligns local HEAD to these pins, preserving nonconflicting
edits. A different live remote branch tip only produces a warning.
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
    commit="fc35ba8b79d354f446dd7a4e6c2b02e22cbf323e",
)

EDITOR = SourceWorkspace(
    key="editor",
    name="CoronaEditor",
    directory=Path("CoronaEditor"),
    url="https://github.com/CoronaEngine/CoronaEditor.git",
    ref="main",
    commit="6003c4bbbb48055830dba6263d66b4af6268b8ea",
)

WORKSPACES = (HORIZON, EDITOR)
