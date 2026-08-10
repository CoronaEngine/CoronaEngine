"""Project archive migration facade.

Archive format parsing belongs to :mod:`runtime.archive`; this plugin owns
only the historical request facade and its load-policy/diagnostic mapping.
"""

from __future__ import annotations

from runtime.archive.errors import ArchiveParseError
from runtime.archive.parser import parse_archive
from runtime.plugin_base import PluginBase


@PluginBase.register_web("ProjectArchive")
class ProjectArchive(PluginBase):
    """Internal Script Service facade for the pure archive parser."""

    @staticmethod
    def parse(payload: dict | str) -> dict:
        request = payload if isinstance(payload, dict) else {"path": str(payload)}
        path = str(request.get("path") or "").strip()
        load_policy = str(request.get("load_policy") or "prompt").strip().lower()
        if load_policy not in {"prompt", "degraded"}:
            return {
                "ok": False,
                "status": "invalid_archive",
                "diagnostics": [
                    {
                        "severity": "error",
                        "recoverable": False,
                        "stage": "archive_parse",
                        "code": "INVALID_LOAD_POLICY",
                        "message": f"Unsupported load policy: {load_policy}",
                        "path": path,
                    }
                ],
            }
        try:
            snapshot = parse_archive(path)
        except ArchiveParseError as exc:
            return {
                "ok": False,
                "status": "invalid_archive",
                "diagnostics": [exc.to_diagnostic()],
            }

        diagnostics = list(snapshot.get("diagnostics") or [])
        if diagnostics and load_policy == "prompt":
            status = "decision_required"
        elif diagnostics:
            status = "ready_degraded"
        else:
            status = "ready"
        return {
            "ok": True,
            "status": status,
            "snapshot": snapshot,
            "diagnostics": diagnostics,
        }

__all__ = ["ProjectArchive"]
