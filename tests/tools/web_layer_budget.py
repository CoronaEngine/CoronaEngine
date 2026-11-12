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
# The handler split moved code into files that were not in this ledger before
# (native_editor_state.h, editor_paths.*, editor_service_operations.cpp), so the
# measured total rose by exactly those lines while the code itself shrank.
# The JSON manifest is gone: table, registry, caller masks, the two `_invoke_cpp_*`
# bindings and six handler modules. The interface core is now below the programme
# target this file recorded when the migration started.
# The window merge traded five public methods (createCameraView, detachPanel,
# redockPanel, toggleCameraMaximized/Borderless, cycleCameraWindowMode,
# toggleMainBorderless) for one translation table: `kind`, `detached` and `mode`
# are decoded once in the V8 reader, and the native host keeps one presentation
# setter instead of four toggles. The surface shrank; the reader that names it is
# explicit, so this ratchet moved up by that decoder's 39 lines only. The editor
# pin then moved to the facade commit that calls those entries, and the pinned
# editorApi.js is measured here, so its 43 bridge lines are in this total too.
# ImGui owns the dock, so the placement request went away as well: the window
# namespace lost setPanelPlacement, the detach state machine left BrowserTab, and
# the dead pre-ImGui layout solver and input router were deleted. The drag-region
# declaration followed: its SDL hit test had no installer, so pages declared
# title-bar rectangles that nothing read. Both removals bring the ratchet down.
# The close merge then traded two methods for one identity reader: three names
# collapsed into close(identity?), and two duplicate self-close paths into one.
# Folding resize/focus/scene-suspension into close() and setWindowPresentation()
# cost a few reader lines and removed three registrations and two dispatch
# branches, so this ratchet moved up by eleven lines net. Collapsing the four
# single-method namespaces and the four audio verbs paid that back: four readers
# and eight registrations became two readers and two registrations. Removing the
# `minimized` snapshot field also removed hide_tab/show_tab, which nothing called.
# Two uncalled helpers followed: browser_ui's clamp_float and the
# read_select_model_file reader no table entry used. Refusing a panel position cost
# three lines and removed a silently ignored option.
CORE_BUDGET = 14391
INTERFACE_FILES = [
    ("src/systems/ui/cef/actor_selection_routing.h", 56),
    ("src/systems/ui/cef/animation_config.h", 45),
    ("src/systems/ui/cef/archive_load_policy.h", 21),
    ("src/systems/ui/cef/camera_window_lifetime.h", 46),
    ("src/systems/ui/cef/cef_operations.h", 130),
    ("src/systems/ui/cef/editor_scene_service.cpp", 4388),
    ("src/systems/ui/cef/editor_paths.h", 22),
    ("src/systems/ui/cef/editor_paths.cpp", 31),
    ("src/systems/ui/cef/editor_service_operations.cpp", 88),
    ("src/systems/ui/cef/editor_network_state.h", 16),
    ("src/systems/ui/cef/editor_network_state.cpp", 219),
    ("src/systems/ui/cef/editor_actor_projection.h", 40),
    ("src/systems/ui/cef/editor_actor_projection.cpp", 294),
    ("src/systems/ui/cef/editor_scene_projection.h", 23),
    ("src/systems/ui/cef/editor_scene_projection.cpp", 165),
    ("src/systems/ui/cef/editor_camera_views.h", 29),
    ("src/systems/ui/cef/editor_camera_views.cpp", 109),
    ("src/systems/ui/cef/editor_scene_access.h", 64),
    ("src/systems/ui/cef/editor_scene_access.cpp", 118),
    ("src/systems/ui/cef/editor_actor_properties.h", 32),
    ("src/systems/ui/cef/editor_actor_properties.cpp", 151),
    ("src/systems/ui/cef/editor_request_values.h", 41),
    ("src/systems/ui/cef/editor_request_values.cpp", 149),
    ("src/systems/ui/cef/editor_platform_dialogs.h", 52),
    ("src/systems/ui/cef/editor_platform_dialogs.cpp", 229),
    ("src/systems/ui/cef/editor_archive_snapshot.h", 35),
    ("src/systems/ui/cef/editor_archive_snapshot.cpp", 361),
    ("src/systems/ui/cef/editor_text_utils.h", 35),
    ("src/systems/ui/cef/native_editor_state.h", 166),
    ("src/systems/ui/cef/editor_scene_service.h", 17),
    ("src/systems/ui/cef/native_api_types.cpp", 250),
    ("src/systems/ui/cef/native_api_types.h", 80),
    ("src/systems/ui/cef/cef_paths.h", 41),
    ("src/systems/ui/cef/cef_py_bind.cpp", 186),
    ("src/systems/ui/cef/script_runtime_api.h", 119),
    ("src/systems/ui/cef/cef_scene_operations.h", 428),
    ("src/systems/ui/cef/cef_script_input.cpp", 51),
    ("src/systems/ui/cef/cef_script_input.h", 17),
    ("src/systems/ui/cef/cef_v8_binding.cpp", 2771),
    ("src/systems/ui/cef/cef_process.h", 36),
    ("src/systems/ui/cef/cef_viewport_pick.cpp", 119),
    ("src/systems/ui/cef/cef_viewport_pick.h", 60),
    ("src/systems/ui/cef/cef_window_operations.cpp", 838),
    ("src/systems/ui/cef/cef_window_operations.h", 110),
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
    ("src/systems/ui/editor_api/editor_api_services.h", 288),
    ("src/systems/ui/editor_api/camera_properties_service.cpp", 87),
    ("src/systems/ui/editor_api/camera_render_settings_service.cpp", 38),
    ("src/systems/ui/editor_api/python_service_bridge.cpp", 91),
    ("src/systems/ui/editor_api/python_service_bridge.h", 29),
    ("src/systems/ui/editor_api/editor_dispatch.h", 377),
    ("src/systems/ui/editor_api/project_script_service.cpp", 57),
    ("src/systems/ui/editor_api/resource_import_service.cpp", 55),
    ("src/systems/ui/editor_api/scene_environment_service.cpp", 59),
    ("src/systems/ui/editor_api/scene_model_node_service.cpp", 63),
    ("src/systems/ui/editor_api/scene_projection_service.cpp", 26),
    ("CoronaEditor/editor/Frontend/src/api/editorApi.js", 181),
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
