"""Line accounting for the C++ -> web interface layer.

The layer is measured as an explicit file list rather than a glob so that a new
file is a deliberate budget decision, not silent growth. ``CORE_BUDGET`` is a
ratchet: a slimming task lowers it first (RED) and deletes code second (GREEN).

Files excluded from the core are the CEF host and window/viewport plumbing
(``cef_client``, ``browser_manager``, ``browser_ui``, ``cef_viewport_gizmo``,
``scene_folder``, ``popup_overlay``, ``cef_osr_mode``, ``cef_paint_upload``,
``cef_shared_texture_probe``, ``cef_app``, ``cef_runtime``, ``cef_renderer_bridge``,
``scene_archive_rpc``). They are a separate project from the interface surface.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Measured 2026-11-09 on feat/systems-thread-dag @ 579b408c.
BASELINE_LINES = 17_477

# Conservative programme target: delete the duplicated implementations and the
# repeated validation preamble without touching the named V8 contracts.
CONSERVATIVE_TARGET = 15_747

# Programme target including the approved single-JSON-entry task.
TARGET_LINES = 13_807

# Current ratchet. Never above BASELINE_LINES, lowered by each slimming task.
CORE_BUDGET = 17_435

INTERFACE_FILES = [
    ("src/systems/ui/cef/actor_selection_routing.h", 56),
    ("src/systems/ui/cef/animation_config.h", 45),
    ("src/systems/ui/cef/archive_load_policy.h", 21),
    ("src/systems/ui/cef/camera_window_lifetime.h", 46),
    ("src/systems/ui/cef/cef_bridge_operations.h", 22),
    ("src/systems/ui/cef/cef_editor_native_api_handlers.cpp", 8495),
    ("src/systems/ui/cef/cef_editor_native_api_handlers.h", 17),
    ("src/systems/ui/cef/cef_editor_native_api_registry.cpp", 309),
    ("src/systems/ui/cef/cef_editor_native_api_registry.h", 80),
    ("src/systems/ui/cef/cef_files_operations.h", 22),
    ("src/systems/ui/cef/cef_network_operations.h", 40),
    ("src/systems/ui/cef/cef_paths.h", 41),
    ("src/systems/ui/cef/cef_project_launcher_operations.h", 54),
    ("src/systems/ui/cef/cef_project_settings_operations.h", 29),
    ("src/systems/ui/cef/cef_py_bind.cpp", 104),
    ("src/systems/ui/cef/cef_scene_operations.h", 428),
    ("src/systems/ui/cef/cef_script_input.cpp", 51),
    ("src/systems/ui/cef/cef_script_input.h", 17),
    ("src/systems/ui/cef/cef_v8_binding.cpp", 2975),
    ("src/systems/ui/cef/cef_v8_binding.h", 20),
    ("src/systems/ui/cef/cef_viewport_pick.cpp", 119),
    ("src/systems/ui/cef/cef_viewport_pick.h", 60),
    ("src/systems/ui/cef/cef_window_operations.cpp", 844),
    ("src/systems/ui/cef/cef_window_operations.h", 101),
    ("src/systems/ui/cef/ik_chain_config.h", 209),
    ("src/systems/ui/cef/model_node_config.h", 52),
    ("src/systems/ui/cef/model_node_page.h", 37),
    ("src/systems/ui/cef/native_api_timing.h", 72),
    ("src/systems/ui/cef/native_material_config.h", 181),
    ("src/systems/ui/cef/scene_tree_actor.h", 36),
    ("src/systems/ui/cef/scene_tree_actor_row.h", 32),
    ("src/systems/ui/cef/ui_resource_read.h", 32),
    ("src/systems/ui/cef/viewport_image_input.h", 42),
    ("src/systems/ui/editor_api/actor_properties_service.cpp", 70),
    ("src/systems/ui/editor_api/actor_properties_service.h", 30),
    ("src/systems/ui/editor_api/camera_properties_service.cpp", 87),
    ("src/systems/ui/editor_api/camera_properties_service.h", 42),
    ("src/systems/ui/editor_api/camera_render_settings_service.cpp", 38),
    ("src/systems/ui/editor_api/camera_render_settings_service.h", 22),
    ("src/systems/ui/editor_api/cef_editor_api.cpp", 1302),
    ("src/systems/ui/editor_api/cef_editor_api.h", 110),
    ("src/systems/ui/editor_api/editor_api_caller.h", 19),
    ("src/systems/ui/editor_api/editor_dispatch.h", 377),
    ("src/systems/ui/editor_api/project_script_service.cpp", 57),
    ("src/systems/ui/editor_api/project_script_service.h", 48),
    ("src/systems/ui/editor_api/resource_import_service.cpp", 55),
    ("src/systems/ui/editor_api/resource_import_service.h", 50),
    ("src/systems/ui/editor_api/scene_environment_service.cpp", 59),
    ("src/systems/ui/editor_api/scene_environment_service.h", 38),
    ("src/systems/ui/editor_api/scene_model_node_service.cpp", 63),
    ("src/systems/ui/editor_api/scene_model_node_service.h", 39),
    ("src/systems/ui/editor_api/scene_projection_service.cpp", 26),
    ("src/systems/ui/editor_api/scene_projection_service.h", 32),
    ("CoronaEditor/editor/Frontend/src/api/editorApi.js", 182),
]

# Per-file ceilings are the measured baseline. They start equal to the current
# size and only come down, which is what stops a task from "reducing" the total
# by moving lines into an untracked neighbour.
SIZE_CEILING = dict(INTERFACE_FILES)


def _line_count(path: Path) -> int:
    return len(path.read_text(encoding="utf-8-sig", errors="replace").splitlines())


def interface_core_lines() -> dict[str, int]:
    """Measured line count per tracked file, keyed by repo-relative POSIX path."""
    return {rel: _line_count(ROOT / rel) for rel, _ in INTERFACE_FILES}


def total_core_lines() -> int:
    return sum(interface_core_lines().values())


if __name__ == "__main__":
    lines = interface_core_lines()
    total = sum(lines.values())
    print(f"interface core: {total} lines across {len(lines)} files")
    print(f"  budget (ratchet):     {CORE_BUDGET}")
    print(f"  conservative target:  {CONSERVATIVE_TARGET}")
    print(f"  programme target:     {TARGET_LINES}")
    for rel, n in sorted(lines.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  {n:6d}  {rel}")
