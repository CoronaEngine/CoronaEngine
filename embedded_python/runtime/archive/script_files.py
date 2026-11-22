"""Store opaque project documents.

A document owns no script: the generated-code channel is gone, so a save only
publishes ``document.json`` and the record carries no script pointer.
"""
import hashlib
import json
from threading import RLock

from runtime.archive.arguments import require_keys
from runtime.archive.portable import atomic_replace
from runtime.bridge_errors import BridgeError
from runtime.project_context import require_project_session


class ProjectFiles:
    _lock = RLock()

    @classmethod
    def _access(cls, request, *, save):
        with cls._lock:
            root, error = require_project_session(request)
            if error:
                raise BridgeError(error.get("code") or "SERVICE_FAILED",
                                  error.get("message") or "Project session refused")
            try:
                require_keys(request, required=('document',) if save else (),
                             optional=('archive_session', 'project_path', 'identity', 'key'))
                if 'identity' not in request and 'key' not in request:
                    raise ValueError('Missing document identity or key')
                if "identity" in request:
                    identity = request["identity"]
                    if (not isinstance(identity, list) or len(identity) != 4
                            or not all(isinstance(part, str) and "\0" not in part for part in identity)
                            or not identity[0] or identity[1] not in ("actor", "project")
                            or (identity[1] == "actor" and not all(identity[2:]))
                            or (identity[1] == "project" and any(identity[2:]))):
                        raise ValueError("Invalid document identity")
                    # Keep existing content-addressed folders readable. This is
                    # storage-key encoding, performed only by the file owner.
                    key = json.dumps(identity, ensure_ascii=False, separators=(",", ":"))
                else:
                    # The independent Python storage API retains opaque keys.
                    key = request["key"]
                if not isinstance(key, str) or not 0 < len(key) <= 2048:
                    raise ValueError("Invalid document key")
                folder = root / "Scripts" / "files" / hashlib.sha256(key.encode("utf-8")).hexdigest()
                if folder.resolve() != folder:
                    raise ValueError("Invalid project document directory")
                index = folder / "document.json"
                if index.is_symlink():
                    raise ValueError("Invalid project document path")
                if save:
                    # `revision` is a content digest of the stored document: it changes
                    # whenever the document changes, using the same JSON serialization
                    # rule as project_archive. This service has no compare-and-swap.
                    document_bytes = json.dumps(request["document"], ensure_ascii=False,
                                                allow_nan=False, indent=2).encode("utf-8")
                    record = {"document": request["document"],
                              "revision": hashlib.sha256(document_bytes).hexdigest(),
                              "document_path": index.relative_to(root).as_posix()}
                    encoded = json.dumps(record, ensure_ascii=False, allow_nan=False, indent=2).encode("utf-8")
                    folder.mkdir(parents=True, exist_ok=True)
                    # Publish the document last. A failed save leaves the prior revision readable.
                    if not index.exists() or index.read_bytes() != encoded:
                        atomic_replace(index, encoded)
                    # The document owns no script. Delete whatever an earlier revision
                    # stored here, including the legacy content-addressed file.
                    for stale in folder.glob("*.py"):
                        try:
                            stale.unlink(missing_ok=True)
                        except OSError:
                            pass  # An in-flight reader may still hold it on Windows.
                    return {"status": "saved", **record}
                if not index.exists():
                    return {"status": "missing"}
                return {"status": "loaded", **json.loads(index.read_bytes())}
            except (KeyError, OSError, ValueError, TypeError, AttributeError) as error:
                code = "FILE_OPERATION_FAILED" if isinstance(error, OSError) else "INVALID_ARGUMENT"
                raise BridgeError(code, str(error)) from error

    @classmethod
    def save(cls, request):
        return cls._access(request, save=True)

    @classmethod
    def load(cls, request):
        return cls._access(request, save=False)
