"""Host-facing compatibility entrypoint for the embedded Python runtime."""

from runtime.bootstrap import editor, run

__all__ = ["editor", "run"]
