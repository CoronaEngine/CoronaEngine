"""Versioned JSON documents in the active project."""
import hashlib
import json
from pathlib import PureWindowsPath
from threading import RLock
from uuid import uuid4

from runtime.archive.portable import atomic_replace
from runtime.project_context import require_project_session

_lock = RLock()
_LIMIT = 8 * 1024 * 1024


def project_document(request, *, commit):
    with _lock:
        root, error = require_project_session(request)
        if error:
            return {"success": False, "error": error["code"]}
        relative = request["document"]
        route = PureWindowsPath(relative)
        path = (root / relative).resolve()
        if (route.anchor or ".." in route.parts or path.suffix != ".json"
                or not path.is_relative_to(root) or path == root):
            return {"success": False, "error": "INVALID_PROJECT_DOCUMENT"}
        if path == root / "assets.manifest.json" or path.is_relative_to(root / "Scripts" / "files"):
            return {"success": False, "error": "INVALID_PROJECT_DOCUMENT"}
        if path.exists() and path.stat().st_size > _LIMIT:
            raise ValueError("Project document exceeds size limit")
        previous = path.read_bytes().decode("utf-8") if path.exists() else ""
        # The structured bridge carries a digest; legacy non-Web callers retain
        # their raw-text version contract. Serialization stays in this owner.
        revision_only = request.get("revision_only") is True
        token = "revision" if revision_only else "version"
        revision = hashlib.sha256(previous.encode("utf-8")).hexdigest() if revision_only else previous
        corrupt = False
        try:
            parsed = json.loads(previous) if previous else None
        except ValueError:
            parsed, corrupt = None, True
        if not commit:
            return {"success": True, token: revision, "content": parsed, "corrupt": corrupt}
        from script_runtime.file_runner import FileScripts
        if FileScripts.has_restore_snapshot():
            return {"success": False, "error": "RUN_ACTIVE"}
        if request.get(token) != revision:
            return {"success": False, "error": "DOCUMENT_CONFLICT"}
        content = json.dumps(request["content"], ensure_ascii=False, indent=2, allow_nan=False)
        encoded = content.encode("utf-8")
        if len(encoded) > _LIMIT:
            raise ValueError("Project document exceeds size limit")
        path.parent.mkdir(parents=True, exist_ok=True)
        if corrupt:
            atomic_replace(path.with_name(f"{path.name}.{uuid4().hex}.corrupt.json"), previous.encode("utf-8"))
        atomic_replace(path, encoded)
        return {"success": True, token: hashlib.sha256(encoded).hexdigest() if revision_only else content}


def read_document(request):
    return project_document(request, commit=False)


def commit_document(request):
    return project_document(request, commit=True)
