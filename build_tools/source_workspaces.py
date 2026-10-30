"""Source dependency baselines. Change these commits to update a pinned version.

Existing developer clones are never aligned implicitly. Run ``dev.py <key>-sync``
after changing a baseline to explicitly adopt it in a local workspace.
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
    commit="750da53e6cae2f59d2f27aa6ada5369bf526e5b7",
)

EDITOR = SourceWorkspace(
    key="editor",
    name="CoronaEditor",
    directory=Path("CoronaEditor"),
    url="https://github.com/CoronaEngine/CoronaEditor.git",
    ref="main",
    commit="3e0edf418d1963e990589435892526daaf3952e1",
)

WORKSPACES = (HORIZON, EDITOR)
