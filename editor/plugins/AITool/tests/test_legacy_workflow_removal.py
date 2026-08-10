from pathlib import Path


AITool_ROOT = Path(__file__).resolve().parents[1]
CAI_EXTENSIONS = AITool_ROOT / "cai_extensions"


def test_legacy_workflow_packages_are_removed():
    removed = (
        CAI_EXTENSIONS / "agent",
        CAI_EXTENSIONS / "flows" / "integrated_multi_scene_workflow",
        CAI_EXTENSIONS / "flows" / "model_retrieval_workflow",
        CAI_EXTENSIONS / "flows" / "scene_composition_workflow",
        CAI_EXTENSIONS / "flows" / "scene_composition_workflow_v2",
        CAI_EXTENSIONS / "flows" / "multi_scene_parallel_workflow",
        CAI_EXTENSIONS / "flows" / "full_pipeline_workflow",
        CAI_EXTENSIONS / "flows" / "terrain_generation_workflow",
    )

    assert all(not any(path.rglob("*.py")) for path in removed)


def test_workflow_registration_has_no_legacy_module_list():
    source = (CAI_EXTENSIONS / "register.py").read_text(encoding="utf-8")

    assert "legacy_flow_modules" not in source
    assert "flow_modules" not in source


def test_legacy_generation_adapters_are_removed():
    services = AITool_ROOT / "services"
    removed = (
        services / "generation_scheduler.py",
        services / "generation_composer_adapter.py",
        services / "generation_provider_adapter.py",
    )
    assert all(not path.exists() for path in removed)


def test_current_ai_entrypoints_do_not_import_legacy_scene_agents():
    production_roots = (
        AITool_ROOT / "main.py",
        AITool_ROOT / "services",
        AITool_ROOT / "cai_extensions" / "mcp",
    )
    forbidden = (
        "cai_extensions.agent",
        "scene_composition_workflow",
        "model_retrieval_workflow",
        "full_pipeline_workflow",
        "multi_scene_parallel_workflow",
        "integrated_multi_scene_workflow",
        "terrain_generation_workflow",
    )

    texts = []
    for root in production_roots:
        paths = (root,) if root.is_file() else (
            path for path in root.rglob("*.py") if "\\tests\\" not in str(path)
        )
        texts.extend(path.read_text(encoding="utf-8") for path in paths)

    assert not any(token in text for text in texts for token in forbidden)
