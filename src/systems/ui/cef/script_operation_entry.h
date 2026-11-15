#pragma once

// The typed entry points behind `CoronaEngine.<group>.<name>`.
//
// Each entry packs the positional arguments the ScriptRuntime contract documents into the
// `NativeRequest` the existing `script_scene_*` handler already reads, and returns that
// handler's `NativeResult` unchanged. Two invariants:
//
//   - an entry never switches threads and never throws: it runs wherever the caller put it,
//     and the CEF owner thread must never see a C++ exception on its way to Python;
//   - a failure carries both the numeric class (`error_code`, 400/403/404/409/2) and the
//     scene layer's symbolic code in `data.code`, because the Python binding raises the
//     symbolic one as `EditorApiError.code`.
//
// Keeping the packing here (rather than inline in the nanobind binders) is what lets
// tests/systems/ui/test_script_runtime_api.cpp drive the same code path the interpreter
// does, without a Python interpreter or a CEF runtime.

#include "native_api_types.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

/// A script-visible failure, translated to `api.errors.EditorApiError` on the Python side.
struct EditorApiError : std::exception {
    EditorApiError(std::string error_code, std::string error_message)
        : code(std::move(error_code)), message(std::move(error_message)) {}

    const char* what() const noexcept override { return message.c_str(); }

    std::string code;
    std::string message;
};

NativeResult script_entry_read_environment(std::string_view scene, const nlohmann::json& scope);
NativeResult script_entry_read_active();
NativeResult script_entry_read_actor(const nlohmann::json& request);
NativeResult script_entry_read_transforms(const nlohmann::json& request);
NativeResult script_entry_read_archive_scene(const nlohmann::json& request);
NativeResult script_entry_read_scene(const nlohmann::json& request);
NativeResult script_entry_set_actor_transform(std::string_view scene, std::string_view actor,
                                              const nlohmann::json& patch);
NativeResult script_entry_set_environment(std::string_view scene, const nlohmann::json& patch);
NativeResult script_entry_update_actor(std::string_view scene, std::string_view actor,
                                       const nlohmann::json& patch);
NativeResult script_entry_set_actor_animation(std::string_view scene, std::string_view actor,
                                              std::string_view animation, double blend_seconds);
NativeResult script_entry_pause_actor_animation(std::string_view scene, std::string_view actor);
NativeResult script_entry_resume_actor_animation(std::string_view scene, std::string_view actor);
NativeResult script_entry_set_actor_physics(std::string_view scene, std::string_view actor,
                                            const nlohmann::json& patch);
NativeResult script_entry_set_camera_pose(std::string_view scene, std::string_view camera,
                                          const nlohmann::json& patch);
NativeResult script_entry_create_actor(std::string_view scene, std::string_view path,
                                       const nlohmann::json& options);
NativeResult script_entry_play_actor_audio(const nlohmann::json& request);
NativeResult script_entry_stop_actor_audio(const nlohmann::json& request);
NativeResult script_entry_delete_actor(std::string_view scene, std::string_view actor,
                                       const nlohmann::json& scope);

}  // namespace Corona::Systems::UI
