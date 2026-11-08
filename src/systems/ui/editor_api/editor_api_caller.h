#pragma once

#include <cstdint>

namespace Corona::Systems::UI {

// Who issued an editor API request. CEF-free on purpose: the native registry,
// the dispatch DTOs and every domain service must compile without CEF headers.
// Web callers use Cef; Python services and file scripts use PythonScript;
// ScriptRuntime covers the in-engine script runtime; Native covers internal
// C++ orchestration (network apply, animation ticks, ImGui).
enum class EditorApiCaller : std::uint32_t {
    Cef = 1u << 0u,
    PythonScript = 1u << 1u,
    ScriptRuntime = 1u << 2u,
    Native = 1u << 3u,
};

}  // namespace Corona::Systems::UI
