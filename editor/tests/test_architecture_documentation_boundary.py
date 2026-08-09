from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[1]


def test_architecture_documentation_names_current_canonical_directory_boundaries():
    source = (EDITOR_ROOT / "ARCHITECTURE.md").read_text(encoding="utf-8")

    for marker in (
        "editor/Frontend/src/compat",
        "editor/plugins/AITool/configuration",
        "script_runtime/compat/legacy_scene_adapter.py",
        "runtime/legacy_script_scene_adapter.py",
        "plugins/AITool/compat/legacy_aitool_scene_adapter.py",
        "runtime/legacy_aitool_scene_adapter.py",
        "script_runtime/blockly",
        "compatibility / generated",
        "`editor/main.py` | 嵌入式 Python runtime 启动入口",
    ):
        assert marker in source, marker


def test_architecture_documentation_keeps_index_as_a_loader_not_a_panel_owner():
    source = (EDITOR_ROOT / "ARCHITECTURE.md").read_text(encoding="utf-8")

    assert "legacyCameraLockPanel.js" in source
    assert "index.html" in source
    assert "panel 实现不得重新内嵌" in source


def test_architecture_documentation_distinguishes_compatibility_and_generated_backend_paths():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")

    assert "`editor/backend`" in source
    assert "`runtime/generated`" in source
    assert "`backend/script`" in source
    assert "`backend/runScript.py`" in source
    assert "不是 Python 业务模块" in source


def test_aitool_documentation_does_not_promote_corona_core_as_public_adapter():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")

    assert "通过 `api.editor_api` 和受限 value-object adapter" in source
    assert "通过 `CoronaCore`、native value adapter" not in source


def test_editor_documentation_classifies_ignored_runtime_data_directories():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")

    for marker in ("editor/media", "editor/models", "运行时数据目录", "不是源码 owner"):
        assert marker in source, marker


def test_legacy_project_copy_data_is_ignored_as_runtime_data():
    ignore_source = (EDITOR_ROOT / ".gitignore").read_text(encoding="utf-8")

    assert "\ndata/\n" in f"\n{ignore_source}"


def test_scene_datas_compatibility_shell_has_a_registered_lifecycle_owner():
    source = (EDITOR_ROOT / "API_OWNERSHIP.md").read_text(encoding="utf-8")

    for marker in (
        "plugins/SceneDatas/compat/legacy_scene_datas_plugin.py",
        "runtime/registry.py",
        "Object panel ID",
        "does not call SceneDatas API",
        "native scene/project lifecycle",
        "SceneDatas native lifecycle",
    ):
        assert marker in source, marker


def test_aitool_service_tests_live_under_their_test_owner_directory():
    services_root = EDITOR_ROOT / "plugins" / "AITool" / "services"

    assert not list(services_root.glob("test_*.py"))
    assert (services_root / "tests").is_dir()


def test_aitool_service_test_support_lives_under_the_test_owner():
    services_root = EDITOR_ROOT / "plugins" / "AITool" / "services"
    support_root = services_root / "tests" / "support"

    assert not (services_root / "_test_import_guard.py").exists()
    assert not (services_root / "test_support").exists()
    assert (support_root / "_test_import_guard.py").is_file()
    assert (support_root / "engine_test_double.py").is_file()


def test_aitool_internal_directory_owners_are_documented():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")

    for marker in (
        "plugins/AITool/cai_extensions",
        "plugins/AITool/services",
        "plugins/AITool/services/tests",
        "plugins/AITool/tests",
        "plugins/AITool/configuration",
        "plugins/AITool/utils",
    ):
        assert marker in source, marker


def test_aitool_subdirectory_owners_are_documented():
    source = (EDITOR_ROOT / "README.md").read_text(encoding="utf-8")

    for marker in (
        "plugins/AITool/services/agent_runtime",
        "plugins/AITool/services/agent_collaboration",
        "plugins/AITool/cai_extensions/agent",
        "plugins/AITool/cai_extensions/flows",
        "plugins/AITool/cai_extensions/mcp",
        "plugins/AITool/cai_extensions/scene_placement",
    ):
        assert marker in source, marker


def test_aitool_verification_runner_lives_with_plugin_tests():
    services_root = EDITOR_ROOT / "plugins" / "AITool" / "services"
    plugin_tests_root = EDITOR_ROOT / "plugins" / "AITool" / "tests"

    assert not (services_root / "verify_ultimate_plan.py").exists()
    assert (plugin_tests_root / "verify_ultimate_plan.py").is_file()


def test_aitool_utils_contains_no_test_modules():
    aitool_root = EDITOR_ROOT / "plugins" / "AITool"

    assert not list((aitool_root / "utils").glob("test_*.py"))
    assert (aitool_root / "tests" / "test_local_ai_setting.py").is_file()


def test_script_runtime_blockly_contains_no_test_modules():
    script_runtime_root = EDITOR_ROOT / "script_runtime"

    assert not list((script_runtime_root / "blockly").glob("test_*.py"))
    assert (script_runtime_root / "tests").is_dir()
