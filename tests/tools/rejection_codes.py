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
    "PROJECT_CONTEXT_CHANGED",  # the request named a scene/archive the editor no longer owns
    "READ_ONLY",                # the scene is read-only
    "READ_ONLY_PROJECT",        # the project is read-only
    "CONFLICT",                 # a concurrent edit won
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
    "SAVE_FAILED",            # the scene could not be written
    "PICK_FAILED",            # the viewport pick had no answer
    "PICK_TIMEOUT",
}

BRIDGE = {
    "EDITOR_API_TIMEOUT",   # the Python service did not answer in time
    "SERVICE_BUSY",         # the service is handling another request
    "SERVICE_UNAVAILABLE",  # the service could not start
    "SERVICE_FAILED",       # the service answered with a failure
}

ALL = INFRASTRUCTURE | WINDOW | SCENE | BRIDGE

# A code is written directly, as an argument to the failure helper, as the reject
# argument, or as one branch of a status-to-code mapping. All four spellings reach Web.
PATTERNS = (
    re.compile(r'error_code\s*=\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'native_failure\("([A-Z][A-Z_]+)"'),
    re.compile(r'reject\([^;]*?"([A-Z][A-Z_]+)"'),
    re.compile(r'RejectPromise\(\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'[?:]\s*"([A-Z][A-Z_]+)"'),
    re.compile(r'return\s+"([A-Z][A-Z_]+)"'),
)


def emitted_codes(root: Path = ROOT / "src/systems/ui") -> dict[str, set[str]]:
    """Every code a source file under the interface layer writes, per code."""
    codes: dict[str, set[str]] = {}
    for path in sorted(root.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        for pattern in PATTERNS:
            for match in pattern.finditer(text):
                codes.setdefault(match.group(1), set()).add(path.name)
    return codes
