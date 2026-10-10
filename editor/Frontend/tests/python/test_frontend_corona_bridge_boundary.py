"""`window.coronaBridge` is a low-latency side channel, not a second editor API.

`src/BOUNDARY.md` states that the `cameraMove` / pick / gizmo / viewport / input fast paths
"不构成第二套公共编辑器 API", and that they may only be used from `utils/viewport*.js`,
viewport controllers, or explicitly registered input components. Nothing enforced that.

This is a **ratchet**, not an approval. The allowlist below records known deviations that
still exist in the view layer; the assertion is a subset check, so converging a file is
always allowed and removing it from the list is never required. Adding a *new* consumer of
the fast-path bridge outside the sanctioned adapters fails the test and forces a deliberate
decision instead of silent drift.

Strings and comments mentioning `coronaBridge` (translation catalogues, error messages) are
stripped first: `src/i18n/domTranslator.js` contains entries like
`'coronaBridge.actorTransform 不可用'`, which is documentation, not a call site.
"""

import re
from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"

# Sanctioned owners of the fast-path bridge.
SANCTIONED = {
    "api/editorApi.js",  # the transport owner wraps coronaBridge.dockCommand
    "utils/viewportUiMode.js",  # an explicitly registered low-latency input adapter
}

# Known deviations in the view/presentation layer, pending convergence into the adapters
# above. Recorded so the ratchet starts from the measured status quo rather than from zero.
KNOWN_DEVIATIONS = {
    "App.vue",
    "blockly/components/BlocklyWorkspace.vue",
    "services/projectLauncherService.js",
    "views/layout/MainPage.vue",
    "views/layout/StoryWorld.vue",
    "views/sidebar/EditorSettings.vue",
    "views/tools/CameraView.vue",
}

STRING_LITERAL_RE = re.compile(
    r"'(?:\\.|[^'\\])*'|\"(?:\\.|[^\"\\])*\"|`(?:\\.|[^`\\])*`",
    re.DOTALL,
)


def _source_without_strings(path):
    return STRING_LITERAL_RE.sub("''", path.read_text(encoding="utf-8"))


def _bridge_consumers():
    consumers = set()
    for path in SRC_ROOT.rglob("*"):
        if not path.is_file() or path.suffix not in {".js", ".mjs", ".vue"}:
            continue
        if "coronaBridge" in _source_without_strings(path):
            consumers.add(path.relative_to(SRC_ROOT).as_posix())
    return consumers


def test_corona_bridge_consumers_stay_within_the_agreed_boundary():
    consumers = _bridge_consumers()
    allowed = SANCTIONED | KNOWN_DEVIATIONS
    unexpected = sorted(consumers - allowed)

    assert unexpected == [], (
        "these files newly consume the window.coronaBridge fast path. It is a low-latency "
        "side channel, not a public editor API: route the call through src/api/editorApi.js "
        "or an explicit utils/viewport*.js adapter, or add the file to KNOWN_DEVIATIONS with "
        f"a reason: {unexpected}"
    )


def test_the_allowlist_does_not_rot():
    """Every declared deviation must still be real, so the list cannot accumulate fiction."""

    consumers = _bridge_consumers()
    stale = sorted((SANCTIONED | KNOWN_DEVIATIONS) - consumers)

    assert stale == [], (
        "these files no longer consume window.coronaBridge and must be removed from the "
        f"sanctioned/known lists so the boundary stays honest: {stale}"
    )
