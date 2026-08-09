from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


COMPATIBILITY_DOCS = (
    "backend/COMPATIBILITY.md",
    "CoronaCore/COMPATIBILITY.md",
    "CoronaPlugin/COMPATIBILITY.md",
    "utils/COMPATIBILITY.md",
    "scripts/COMPATIBILITY.md",
    "plugins/MainView/COMPATIBILITY.md",
    "plugins/SceneDatas/COMPATIBILITY.md",
    "Frontend/src/compat/BOUNDARY.md",
)


def test_editor_has_a_global_compatibility_inventory():
    inventory = EDITOR_ROOT / "COMPATIBILITY.md"

    assert inventory.is_file()
    source = inventory.read_text(encoding="utf-8")
    for marker in (
        "canonical owner",
        "外部宿主",
        "删除条件",
        "禁止新增业务",
        "runtime.registry",
        "script_runtime",
        "editorApi",
    ):
        assert marker in source


def test_global_inventory_links_all_local_compatibility_boundaries():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    for relative_path in COMPATIBILITY_DOCS:
        assert relative_path in source


def test_global_inventory_records_high_risk_legacy_families():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    for marker in (
        "CoronaCore.core.entities",
        "backend.blockly",
        "CoronaPlugin",
        "utils.settings",
        "scripts.pack",
        "SceneDatas",
        "window.coronaBridge",
    ):
        assert marker in source


def test_compatibility_inventory_requires_external_confirmation_before_deletion():
    source = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")

    assert "仓内搜索不到调用方" in source
    assert "不能直接删除" in source
    assert "回归" in source


def test_project_copy_compatibility_docs_match_the_actual_wrapper_layout():
    inventory = (EDITOR_ROOT / "COMPATIBILITY.md").read_text(encoding="utf-8")
    plugins_boundary = (EDITOR_ROOT / "plugins" / "BOUNDARY.md").read_text(
        encoding="utf-8"
    )

    assert "plugins/ProjectLauncher/compat/legacy_project_copy.py" in inventory
    assert "旧入口：`project_copy.py`、`utils/project_copy.py`" in inventory
    assert "`runtime.legacy_project_copy`" in inventory
    assert "`utils/project_copy.py` |" not in inventory
    assert "转发到 `runtime.compat.legacy_project_copy`" in plugins_boundary


def test_small_legacy_directories_are_one_way_wrappers():
    wrappers = {
        "scripts/compat/legacy_pack.py": "from tools.pack import",
        "utils/compat/legacy_settings.py": "from config.settings import",
        "utils/compat/legacy_logging.py": "from runtime.logging import",
        "CoronaPlugin/compat/legacy_plugin_base.py": "from runtime.plugin_base import",
        "CoronaPlugin/compat/legacy_load_utils.py": "from runtime.plugin_loader import",
        "backend/project_settings/main.py": "from plugins.ProjectSettings.main import",
    }

    for relative_path, owner_import in wrappers.items():
        source = (EDITOR_ROOT / relative_path).read_text(encoding="utf-8")
        assert owner_import in source, relative_path
        assert "class " not in source, relative_path


def test_backend_docs_distinguish_editor_wrappers_from_project_legacy_output():
    backend_doc = (EDITOR_ROOT / "backend" / "COMPATIBILITY.md").read_text(
        encoding="utf-8"
    )
    blockly_init = (EDITOR_ROOT / "script_runtime" / "blockly" / "__init__.py").read_text(
        encoding="utf-8"
    )

    assert "项目根目录" in backend_doc
    assert "不是当前 `editor/backend` 下的文件" in backend_doc
    assert "Script Runtime Blockly/Scratch" in blockly_init
