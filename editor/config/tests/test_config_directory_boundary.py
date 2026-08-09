from pathlib import Path


EDITOR_ROOT = Path(__file__).resolve().parents[2]
CONFIG_ROOT = EDITOR_ROOT / "config"


def test_config_has_a_local_boundary_inventory():
    boundary = CONFIG_ROOT / "BOUNDARY.md"

    assert boundary.is_file()
    source = boundary.read_text(encoding="utf-8")
    for marker in (
        "路径 owner",
        "活动项目状态",
        "应用运行时配置",
        "project_state.py",
        "删除条件",
    ):
        assert marker in source
    for file_name in (
        "paths_config.py",
        "app_config.py",
        "runtime_config.py",
    ):
        assert file_name in source


def test_config_modules_have_distinct_canonical_owners():
    paths = (CONFIG_ROOT / "paths_config.py").read_text(encoding="utf-8")
    app = (CONFIG_ROOT / "app_config.py").read_text(encoding="utf-8")
    runtime = (CONFIG_ROOT / "runtime_config.py").read_text(encoding="utf-8")

    assert "class PathsConfig" in paths
    assert "def get_default_paths" in paths
    project_state = (CONFIG_ROOT / "project_state.py").read_text(encoding="utf-8")
    assert "class CoronaSettings" in project_state
    assert "settings_manager" in project_state
    assert "class AppConfig" in app
    assert "def get_app_config" in app
    assert "class RuntimeConfig" in runtime


def test_paths_config_owns_repository_asset_directory():
    from config.paths_config import get_repository_assets_dir

    assert get_repository_assets_dir() == EDITOR_ROOT.parent / "assets"


def test_project_state_is_the_canonical_owner():
    project_state = (CONFIG_ROOT / "project_state.py").read_text(encoding="utf-8")

    assert "class CoronaSettings" in project_state
    assert "settings_manager = CoronaSettings()" in project_state
    assert not (CONFIG_ROOT / "settings.py").is_file()


def test_removed_utils_settings_has_no_source_owner():
    assert not any(
        path.is_file() and "__pycache__" not in path.parts
        for path in (EDITOR_ROOT / "utils").rglob("*")
    )


def test_active_consumers_do_not_import_project_state_from_settings_facade():
    consumers = (
        EDITOR_ROOT / "runtime" / "editor_host.py",
        EDITOR_ROOT / "runtime" / "project_context.py",
        EDITOR_ROOT / "runtime" / "project_templates.py",
        EDITOR_ROOT / "script_runtime" / "blockly" / "main.py",
        EDITOR_ROOT / "plugins" / "MainView" / "main.py",
    )

    for path in consumers:
        source = path.read_text(encoding="utf-8")
        assert "from config.settings import" not in source


def test_project_state_does_not_own_paths_config_instance():
    project_state = (CONFIG_ROOT / "project_state.py").read_text(encoding="utf-8")
    assert "core_path = get_default_paths()" not in project_state


def test_project_state_default_config_path_is_independent_of_process_cwd(
    monkeypatch, tmp_path
):
    from config.project_state import CoronaSettings

    monkeypatch.chdir(tmp_path)

    settings = CoronaSettings()

    assert Path(settings.config_path) == EDITOR_ROOT.parent / "CoronaEditor.ini"
    assert not (tmp_path / "CoronaEditor.ini").exists()
