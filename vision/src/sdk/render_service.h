//
// Created by Zero on 2026/4/30.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "vision.h"

namespace vision {
namespace sdk {

enum class SceneSourceKind : uint32_t {
    CoronaEngineSnapshot = 2,
};

enum class OutputMode : uint32_t {
    CpuImage = 0,
    ExternalTexture = 1,
    ExternalBuffer = 2,
    File = 3,
};

struct SceneSource {
    SceneSourceKind kind{SceneSourceKind::CoronaEngineSnapshot};
    std::string debug_name{};
    const void *payload{};
    size_t payload_size{};
    uint32_t schema_version{1u};
};

struct OutputBufferView {
    void *data{};
    uint64_t size_bytes{};
    uint64_t row_pitch_bytes{};
};

struct ExternalImageHandle {
    uint64_t handle{};
    uint64_t size_bytes{};
};

struct RenderOutput {
    OutputMode mode{OutputMode::CpuImage};
    VisionImageFormat format{VisionImageFormat_RGBA16Float};
    VisionExtent2D extent{};
    OutputBufferView cpu_buffer{};
    ExternalImageHandle external{};
    std::string file_path{};
};

struct SessionConfig {
    VisionExtent2D initial_extent{};
    uint32_t session_flags{VisionSessionFlag_EnableShaderCache};
    std::string runtime_dir{};
    std::string plugin_dir{};
    std::string shader_cache_dir{};
    std::string debug_name{};
};

struct RenderRequest {
    SceneSource scene{};
    RenderOutput output{};
    std::string camera_name{};
    VisionAOV aov{VisionAOV_FinalColor};
    uint32_t spp{};
    uint32_t max_depth{};
    uint32_t render_flags{VisionRenderFlag_BlockUntilFinished | VisionRenderFlag_ReturnStats};
    uint32_t time_budget_ms{};
};

struct RenderResult {
    VisionStatusCode status{VisionStatusCode_InternalError};
    VisionRenderStats stats{};
    VisionExtent2D extent{};
    VisionImageFormat format{VisionImageFormat_Unknown};
    uint64_t produced_bytes{};
    std::string message{};
};

class RenderSession {
public:
    virtual ~RenderSession() noexcept = default;

    [[nodiscard]] virtual VisionExtent2D extent() const noexcept = 0;

    virtual VisionStatusCode update_scene(const SceneSource &scene) noexcept = 0;
    [[nodiscard]] virtual RenderResult render(const RenderRequest &request) noexcept = 0;
    virtual VisionStatusCode resize(VisionExtent2D extent) noexcept = 0;
    virtual VisionStatusCode reset_accumulation() noexcept = 0;
    [[nodiscard]] virtual std::string last_error() const = 0;
};

/// Source-level integration entry point.
///
/// Intended usage in CoronaEngine:
/// 1. Construct one session during OpticsSystem initialization.
/// 2. Translate SharedDataHub scene state into SceneSource.
/// 3. Call render() each frame and write directly into an engine-owned output target.
[[nodiscard]] std::unique_ptr<RenderSession> create_render_session(const SessionConfig &config);

}// namespace sdk
}// namespace vision
