from pathlib import Path


CORE_ROOT = Path(__file__).resolve().parents[1].parent / "CoronaCore" / "core"


def test_runtime_host_owns_response_helpers():
    canonical = CORE_ROOT.parents[1] / "runtime" / "response_utils.py"
    compatibility = CORE_ROOT.parent / "utils" / "response_utils.py"
    editor_source = (CORE_ROOT.parents[1] / "runtime" / "editor_host.py").read_text(encoding="utf-8")
    compatibility_source = compatibility.read_text(encoding="utf-8")

    assert canonical.is_file()
    assert compatibility.is_file()
    assert "from runtime.response_utils import" in editor_source
    assert "from runtime.response_utils import" in compatibility_source
    assert "CoronaCore.core.response_utils" not in compatibility_source


def test_runtime_owns_host_support_helpers_and_core_paths_are_compatibility_aliases():
    repo_root = CORE_ROOT.parents[1]
    host_source = (repo_root / "runtime" / "editor_host.py").read_text(encoding="utf-8")
    engine_alias = (CORE_ROOT / "corona_engine.py").read_text(encoding="utf-8")
    response_alias = (CORE_ROOT / "response_utils.py").read_text(encoding="utf-8")

    assert (repo_root / "runtime" / "native_engine.py").is_file()
    assert (repo_root / "runtime" / "response_utils.py").is_file()
    assert "CoronaCore.core.corona_engine" not in host_source
    assert "CoronaCore.core.response_utils" not in host_source
    assert "Compatibility" in engine_alias
    assert "from runtime.native_engine import" in engine_alias
    assert "Compatibility" in response_alias
    assert "from runtime.response_utils import" in response_alias


def test_runtime_owns_network_sync_policy_and_core_path_is_compatibility_alias():
    repo_root = CORE_ROOT.parents[1]
    runtime_source = (repo_root / "runtime" / "network_sync_policy.py").read_text(
        encoding="utf-8"
    )
    compatibility_source = (CORE_ROOT / "network_sync_policy.py").read_text(
        encoding="utf-8"
    )

    assert "def deferred_actor_broadcasts" in runtime_source
    assert "Compatibility" in compatibility_source
    assert "from runtime.network_sync_policy import" in compatibility_source


def test_runtime_owns_archive_parser_and_corona_core_path_is_compatibility_package():
    repo_root = CORE_ROOT.parents[1]
    runtime_parser = repo_root / "runtime" / "archive" / "parser.py"
    compatibility_parser = repo_root / "CoronaCore" / "archive" / "parser.py"
    compatibility_init = repo_root / "CoronaCore" / "archive" / "__init__.py"

    assert runtime_parser.is_file()
    assert "from runtime.archive.parser import" in compatibility_parser.read_text(
        encoding="utf-8"
    )
    assert "from runtime.archive import" in compatibility_init.read_text(encoding="utf-8")


def test_script_runtime_uses_canonical_project_helpers():
    source = (CORE_ROOT.parents[1] / "script_runtime" / "engine" / "corona_engine.py").read_text(
        encoding="utf-8"
    )

    assert "from runtime.scene_support import get_project_scenes" in source
    assert "CoronaCore.utils.proejct_utils" not in source


def test_runtime_owns_project_support_and_core_path_is_compatibility_alias():
    repo_root = CORE_ROOT.parents[1]
    runtime_source = (repo_root / "runtime" / "project_templates.py").read_text(
        encoding="utf-8"
    )
    compatibility_source = (CORE_ROOT / "project_utils.py").read_text(encoding="utf-8")

    assert "def create_project_from_template" in runtime_source
    assert "Compatibility" in compatibility_source
    assert "from runtime.project_templates import" in compatibility_source
    assert "from runtime.scene_support import" in compatibility_source


def test_runtime_owns_legacy_scene_implementation_and_core_legacy_path_is_compatibility():
    repo_root = CORE_ROOT.parents[1]
    runtime_entities = repo_root / "runtime" / "legacy" / "entities"
    runtime_components = repo_root / "runtime" / "legacy" / "components"
    runtime_managers = repo_root / "runtime" / "legacy" / "managers"
    compatibility_source = (
        repo_root / "CoronaCore" / "core" / "legacy" / "entities" / "actor.py"
    ).read_text(encoding="utf-8")

    assert (runtime_entities / "actor.py").is_file()
    assert (runtime_components / "geometry.py").is_file()
    assert (runtime_managers / "scene_manager.py").is_file()
    assert "Compatibility" in compatibility_source
    assert "from runtime.legacy.entities.actor import" in compatibility_source


def test_runtime_owns_legacy_engine_adapter_and_core_path_is_compatibility():
    repo_root = CORE_ROOT.parents[1]
    canonical = repo_root / "runtime" / "legacy_engine_adapter.py"
    compatibility = CORE_ROOT / "engine_runtime.py"

    assert canonical.is_file()
    assert "Compatibility" in compatibility.read_text(encoding="utf-8")
    assert "from runtime.legacy_engine_adapter import" in compatibility.read_text(
        encoding="utf-8"
    )
