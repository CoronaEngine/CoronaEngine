"""The rejection vocabulary the interface layer can put in front of Web.

One place owns the list so the guard and the generated contract agree: every
code below is emitted by a source file under ``src/systems/ui`` and reaches a
Promise rejection (or a bridge reply) verbatim. Adding a code means adding it
here and explaining it in the contract document; the guard fails otherwise.

The groups answer "who answers with it":

- ``INFRASTRUCTURE`` — the V8 binding itself, before any native work runs.
- ``WINDOW`` — the native window host (a surface vanished or could not be made).
- ``SCENE`` — the scene/archive layer: the request named something the editor
  no longer owns, or a scene object refused the write.
- ``BRIDGE`` — the Web -> Python transport and the service behind it.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

INFRASTRUCTURE = {
    "PERMISSION_DENIED",  # the calling context may not perform this operation
    "CONTEXT_CHANGED",    # the page's V8 context was replaced mid-call
    "INVALID_ARGUMENT",   # the request reader refused the arguments
    "NATIVE_UNAVAILABLE", # the native owner thread did not take the work
    "NATIVE_FAILURE",     # the operation threw
}

WINDOW = {
    "WINDOW_CLOSED",  # the calling browser is no longer tracked
    "CREATE_FAILED",  # the native host could not create the surface
}

SCENE = {
    "NO_ACTIVE_PROJECT",        # the archive/file service was asked to work without an open project
    "PROJECT_CONTEXT_CHANGED",  # the request named a scene/archive the editor no longer owns
    "READ_ONLY",                # the scene is read-only
    "READ_ONLY_PROJECT",        # the project is read-only
    "CONFLICT",                 # a concurrent edit won
    "ACTOR_VERSION_CONFLICT",   # the Actor moved on; reload its state and retry once
    "LAST_CAMERA",              # a scene keeps at least one camera
    "CAMERA_NOT_DELETABLE",     # the camera cannot be removed
    "CAMERA_NOT_FOUND",
    "ACTOR_NOT_FOUND",
    "ACTOR_NOT_READY",
    "ACTOR_CREATE_FAILED",
    "ACTOR_AUDIO_UNAVAILABLE",
    "RESOURCE_NOT_FOUND",
    "MODEL_DECODE_FAILED",
    "UNSUPPORTED_RESOURCE_TYPE",
    "FILE_NOT_FOUND",
    "FILE_OPERATION_FAILED",  # the archive service could not run a file operation
    "INVALID_PROJECT_PATH",
    "PROJECT_OPEN_FAILED",
    "PROJECT_CREATION_FAILED",
    "DOCUMENT_CONFLICT",         # commit compare-and-swap lost; reload the document then retry
    "INVALID_PROJECT_DOCUMENT",  # the document route is outside the project or not JSON
    "RUN_ACTIVE",                # a script run owns the scene; stop and restore it first
    "SAVE_FAILED",            # the scene could not be written
    "CAPTURE_FAILED",         # the screenshot could not be taken
    "COMPONENT_NOT_FOUND",    # the imported model has no such component
    "INVALID_BOUNDS",         # the requested rectangle is degenerate
    "NODE_EDIT_REJECTED",     # the geometry owner refused the node write
    "NODE_NOT_FOUND",         # the model node index is unknown
}

BRIDGE = {
    "EDITOR_API_TIMEOUT",   # the Python service did not answer in time
    "SERVICE_BUSY",         # the service is handling another request
    "SERVICE_UNAVAILABLE",  # the service could not start
    "SERVICE_FAILED",       # the service answered with a failure
}

ALL = INFRASTRUCTURE | WINDOW | SCENE | BRIDGE

# The two tiers a Web caller should treat differently.
#
# STABLE codes name a decision the caller can act on (retry, reload the page, show a
# message, stop writing). Their pairing with a scenario is part of the contract. Every
# other code is a detail: it still rejects, but it goes to the log, not to a branch -
# details may be renamed, merged, or moved under another code between releases.
#
# Adding to STABLE is a contract change: say in docs/interface-decisions.md what
# decision the caller is supposed to make.
STABLE = frozenset({
    # infrastructure
    "CONTEXT_CHANGED",        # retry after the page's context is replaced
    "PERMISSION_DENIED",      # this caller may not do it; disable the control
    "INVALID_ARGUMENT",       # a caller bug; fix the request, do not retry
    "NATIVE_UNAVAILABLE",     # the owner thread did not take it; retry later
    # window
    "WINDOW_CLOSED",          # the surface is gone; stop addressing it
    "CREATE_FAILED",          # the native host could not make it; report
    # scene / archive
    "PROJECT_CONTEXT_CHANGED",  # the scene/archive changed under the caller
    "READ_ONLY",
    "READ_ONLY_PROJECT",
    "CONFLICT",               # another writer won; reload then retry
    "ACTOR_VERSION_CONFLICT",  # the Actor moved on; reload then retry once
    "CAMERA_NOT_FOUND",
    "ACTOR_NOT_FOUND",
    "RESOURCE_NOT_FOUND",
    "FILE_NOT_FOUND",
    "DOCUMENT_CONFLICT",      # the document CAS lost; reload the document then retry
    # bridge
    "EDITOR_API_TIMEOUT",
    "SERVICE_BUSY",
    "SERVICE_UNAVAILABLE",
    "SERVICE_FAILED",
})

DETAIL = ALL - STABLE


# A code is written directly, as an argument to the failure helper, as the reject
# argument, or as one branch of a status-to-code mapping. All four spellings reach Web.
PATTERNS = (
    re.compile(r'error_code\s*=\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'native_failure\("([A-Z][A-Z_]+)"'),
    re.compile(r'fail\("([A-Z][A-Z_]+)"'),
    re.compile(r'reject\([^;]*?"([A-Z][A-Z_]+)"'),
    re.compile(r'RejectPromise\(\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'[?:]\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'return\s+"([A-Z][A-Z_]+)"'),
)


def python_refusal_codes(root: Path = ROOT / "embedded_python") -> set[str]:
    """Codes the Python bridge services refuse with (they reach Web as rejections)."""
    codes: set[str] = set()
    for path in root.rglob("*.py"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in re.finditer(r"BridgeError\(\s*[\"']([A-Z][A-Z0-9_]+)[\"']", text):
            codes.add(match.group(1))
        if path.name == "project_context.py":
            for match in re.finditer(r"\"code\":\s*\"([A-Z][A-Z0-9_]+)\"", text):
                codes.add(match.group(1))
        if path.name == "documents.py":
            for match in re.finditer(r"\"error\":\s*\"([A-Z][A-Z0-9_]+)\"", text):
                codes.add(match.group(1))
        if path.name == "project_files.py":
            for match in re.finditer(r"\(\s*\"[^\"]+\",\s*\"([A-Z][A-Z0-9_]+)\"\s*\)", text):
                codes.add(match.group(1))
    return codes


def emitted_codes(root: Path = ROOT / "src/systems/ui") -> dict[str, set[str]]:
    """Every code a source file under the interface layer writes, per code."""
    codes: dict[str, set[str]] = {}
    for path in sorted(root.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for pattern in PATTERNS:
            for match in pattern.finditer(text):
                codes.setdefault(match.group(1), set()).add(path.name)
    return codes
