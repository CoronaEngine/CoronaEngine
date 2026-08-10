from pathlib import Path


FRONTEND_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = FRONTEND_ROOT / "src"
BLOCKLY_ROOT = SRC_ROOT / "blockly"


EXPECTED_DIRECTORIES = (
    "blocks",
    "components",
    "composables",
    "configs",
    "generators",
    "i18n",
    "node-editor",
    "store",
    "utils",
)


def _blockly_sources():
    return (
        path
        for path in BLOCKLY_ROOT.rglob("*")
        if path.is_file() and path.suffix in {".js", ".vue"}
    )


def test_blockly_has_a_local_boundary_inventory():
    boundary = BLOCKLY_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "积木定义",
        "Python 生成器",
        "工作区 UI",
        "节点图",
        "Script Runtime",
        "删除条件",
    ):
        assert marker in source
    for directory in EXPECTED_DIRECTORIES:
        assert directory in source


def test_blockly_does_not_bypass_frontend_compatibility_boundaries():
    offenders = []
    for path in _blockly_sources():
        source = path.read_text(encoding="utf-8")
        if "window.cefQuery" in source or "utils/bridge" in source:
            offenders.append(path)
    assert offenders == []


def test_blockly_generators_target_the_restricted_script_runtime_namespace():
    generator_sources = [
        path.read_text(encoding="utf-8")
        for path in (BLOCKLY_ROOT / "generators").glob("*.js")
    ]
    source = "\n".join(generator_sources)

    assert "CoronaEngine." in source
    assert "editorApi" not in source
    assert "window.cefQuery" not in source


def test_blockly_node_graph_adapter_is_not_a_transport_owner():
    source = (BLOCKLY_ROOT / "node-editor" / "aiNodeGraphService.js").read_text(
        encoding="utf-8"
    )

    assert "validateGeneratedNodeGraphResult" in source
    assert "BroadcastChannel" in source
    assert "window.cefQuery" not in source
    assert "editorApi" not in source
