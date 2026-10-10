"""Panel/editor code must be reachable only through lazy imports.

Every editor window is a separate document that boots the whole frontend. If panel
components (and Blockly) are imported eagerly, *every* window -- including each
detached `?standalone=1` panel -- downloads all ten panels plus Blockly before it can
render anything. These tests pin the lazy-loading invariant at the source level, the
same way the other `test_frontend_*_boundary.py` tests pin ownership boundaries.
"""

import re
from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"

STATIC_IMPORT_RE = re.compile(r"^\s*import\s+[\w{},\s*]+\s+from\s+['\"]([^'\"]+)['\"]")
SIDE_EFFECT_IMPORT_RE = re.compile(r"^\s*import\s+['\"]([^'\"]+)['\"]")
DYNAMIC_IMPORT_RE = re.compile(r"import\(\s*['\"]([^'\"]+\.vue)['\"]\s*\)")

# Statically reachable from `main.js`, i.e. evaluated in EVERY editor window -- including
# each detached `?standalone=1` panel window.
ENTRY_GRAPH_FILES = (
    "main.js",
    "App.vue",
    "router/index.js",
    "views/panelRegistry.js",
)

EXPECTED_PANELS = {
    "SceneBar.vue",
    "Object.vue",
    "Pet.vue",
    "LogView.vue",
    "FileManager.vue",
    "ProjectSettings.vue",
    "NodeGraphPanel.vue",
    "CabbageChatPanel.vue",
    "EditorSettings.vue",
    "LightFieldCalibrationPanel.vue",
}


def test_panel_registry_does_not_eagerly_import_panel_components():
    registry = (SRC_ROOT / "views" / "panelRegistry.js").read_text(encoding="utf-8")

    offenders = [
        (line_number, match.group(1))
        for line_number, line in enumerate(registry.splitlines(), start=1)
        if (match := STATIC_IMPORT_RE.match(line))
        and match.group(1).startswith(("@/views/", "@/components/", "../views/", "../components/"))
    ]

    assert offenders == [], (
        "panel components must be loaded lazily, but panelRegistry.js imports them "
        f"eagerly at {offenders}"
    )


def test_every_panel_is_reachable_through_a_dynamic_import():
    registry = (SRC_ROOT / "views" / "panelRegistry.js").read_text(encoding="utf-8")

    lazy_paths = {Path(path).name for path in DYNAMIC_IMPORT_RE.findall(registry)}

    assert EXPECTED_PANELS <= lazy_paths, (
        "every panel must stay reachable through a dynamic import; missing "
        f"{sorted(EXPECTED_PANELS - lazy_paths)}"
    )
    assert "defineAsyncComponent" in registry, (
        "the registry must hand out components (defineAsyncComponent), not bare loader "
        "functions: DockPanel declares its `component` prop as type Object"
    )


def test_entry_graph_does_not_eagerly_load_blockly():
    """Blockly is the largest dependency in the bundle and no editor window needs it at boot.

    Measured: with an eager import in the entry graph, `dist/assets/index-*.js` is ~1030 kB
    and contains `FieldDropdown` / `WorkspaceSvg`. Every Blockly workspace loads the runtime
    it needs itself (`BlocklyToolboxPalette.vue`, `MiniBlocklyWorkspace.vue`,
    `BlocklyWorkspace.vue`), so the entry graph must stay free of it.
    """

    offenders = []
    for relative in ENTRY_GRAPH_FILES:
        source = (SRC_ROOT / relative).read_text(encoding="utf-8")
        for line_number, line in enumerate(source.splitlines(), start=1):
            for pattern in (STATIC_IMPORT_RE, SIDE_EFFECT_IMPORT_RE):
                match = pattern.match(line)
                if match and "blockly" in match.group(1):
                    offenders.append(f"{relative}:{line_number} -> {match.group(1)}")
                    break

    assert offenders == [], (
        "these files are statically reachable from main.js, so importing Blockly there "
        "makes EVERY editor window (including each detached panel) download it before it "
        f"can render: {offenders}"
    )


def test_every_blockly_workspace_loads_the_builtin_blocks_it_needs():
    """`blockly/blocks` registers the built-in block definitions the toolboxes reference.

    It must be loaded by each module that injects a workspace rather than globally at boot,
    otherwise removing the entry-graph import would silently empty the built-in block
    categories at runtime.
    """

    workspaces = [
        path
        for path in (SRC_ROOT / "blockly" / "components").rglob("*.vue")
        if "BlocklyLib.inject(" in path.read_text(encoding="utf-8")
        or "Blockly.inject(" in path.read_text(encoding="utf-8")
    ]
    assert workspaces, "expected to find the Blockly workspace components"

    missing = [
        str(path.relative_to(SRC_ROOT))
        for path in workspaces
        if "blockly/blocks'" not in path.read_text(encoding="utf-8")
        and 'blockly/blocks"' not in path.read_text(encoding="utf-8")
    ]

    assert missing == [], (
        "every module that injects a Blockly workspace must load `blockly/blocks` itself; "
        f"these rely on a global side effect instead: {missing}"
    )
