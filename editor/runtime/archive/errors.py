from __future__ import annotations

from typing import Any


class ArchiveParseError(ValueError):
    def __init__(
        self,
        code: str,
        message: str,
        *,
        path: str = "",
        recoverable: bool = False,
        details: dict[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.path = path
        self.recoverable = recoverable
        self.details = details or {}

    def to_diagnostic(self) -> dict[str, Any]:
        return {
            "severity": "error",
            "recoverable": self.recoverable,
            "stage": "archive_parse",
            "code": self.code,
            "message": str(self),
            "path": self.path,
            **self.details,
        }
