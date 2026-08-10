"""Structural contracts consumed by the Script Runtime script lifecycle.

These protocols describe the small scene/actor surface needed by script
instances. They intentionally do not import or expose the Python entity model;
the embedded Script Runtime host is the only production implementation.
"""

from __future__ import annotations

from typing import Protocol, Sequence


class ActorScriptTarget(Protocol):
    name: str
    script_path: str


class SceneScriptTarget(Protocol):
    name: str
    route: str
    script_path: str
    _actors: Sequence[ActorScriptTarget]


__all__ = ["ActorScriptTarget", "SceneScriptTarget"]
