"""Store opaque project documents and the exact script bytes paired with each document."""
import hashlib
import json
import re
from threading import RLock

from runtime.archive.portable import atomic_replace
from runtime.project_context import require_project_session


class ProjectFiles:
    _lock = RLock()

    @classmethod
    def _access(cls, request, *, save):
        with cls._lock:
            root, error = require_project_session(request)
            if error:
                return error
            try:
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
                    code = request["code"].encode("utf-8")
                    digest = hashlib.sha256(code).hexdigest()
                    source = folder / f"{digest}.py"
                    if source.is_symlink():
                        raise ValueError("Invalid project script path")
                    record = {"document": request["document"], "revision": digest,
                              "code_path": source.relative_to(root).as_posix(),
                              "document_path": index.relative_to(root).as_posix()}
                    encoded = json.dumps(record, ensure_ascii=False, allow_nan=False, indent=2).encode("utf-8")
                    folder.mkdir(parents=True, exist_ok=True)
                    if not source.exists() or source.read_bytes() != code:
                        atomic_replace(source, code)
                    # Publish the document last. A failed save leaves the prior revision readable.
                    if not index.exists() or index.read_bytes() != encoded:
                        atomic_replace(index, encoded)
                    # Only the published revision belongs to this document. Interrupted
                    # saves may have left an unpublished, content-addressed file here.
                    for old in folder.glob("*.py"):
                        if old != source and re.fullmatch(r"[0-9a-f]{64}", old.stem):
                            try:
                                old.unlink(missing_ok=True)
                            except OSError:
                                pass  # An in-flight reader may still hold it on Windows.
                    return {"status": "saved", **record}
                if not index.exists():
                    return {"status": "missing"}
                return {"status": "loaded", **json.loads(index.read_bytes())}
            except (KeyError, OSError, ValueError, TypeError, AttributeError) as error:
                return {"status": "error", "message": str(error)}

    @classmethod
    def save(cls, request):
        return cls._access(request, save=True)

    @classmethod
    def load(cls, request):
        return cls._access(request, save=False)
