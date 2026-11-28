#pragma once

#include <cstdint>
#include <array>

// Engine lighting controls for Vision's spherical atmosphere and delta sun.

// Forward declarations
namespace vision {
class Pipeline;
}  // namespace vision

namespace Corona {
struct EnvironmentDevice;
}  // namespace Corona

namespace Corona::Systems::Vision {

enum class VisionLightingMode { None, Atmosphere, Authored };

struct VisionLightingState {
    std::size_t authored_revision{};
    VisionLightingMode mode{VisionLightingMode::None};
    // Vision stores the outgoing direction, whereas Corona stores toward-sun.
    std::array<float, 3> sun_direction{};
    std::array<float, 3> sun_color{};
    float sun_intensity{0.f};
    float sky_intensity{0.f};
    bool operator==(const VisionLightingState&) const = default;
};

[[nodiscard]] VisionLightingState make_vision_lighting_state(const EnvironmentDevice& env) noexcept;

[[nodiscard]] bool try_update_vision_sun_direction(::vision::Pipeline& pipeline, VisionLightingState previous,
                                                   const VisionLightingState& current);

struct VisionLightSetupResult {
    VisionLightingMode mode{VisionLightingMode::None};
    float environment_scale{0.f};
    std::uint32_t environment_count{0};
    std::uint32_t directional_light_count{0};
    std::uint32_t area_light_count{0};
    std::uint32_t punctual_light_count{0};
};

// Authored environments take precedence over the procedural atmosphere.
[[nodiscard]] VisionLightingMode vision_lighting_mode(const EnvironmentDevice& env) noexcept;
[[nodiscard]] const char* vision_lighting_mode_name(VisionLightingMode mode) noexcept;

// Replaces non-area lights in an EngineBuilt scene, preserving geometry-owned
// area lights. A black environment is retained in None mode because Vision's
// current miss and light-sampling paths require a registered environment.
// Call on scene build or effective lighting-mode change, then prepare/upload the
// scene light data and invalidate accumulated frames before rendering again.
VisionLightSetupResult setup_vision_lights(::vision::Pipeline& pipeline, const EnvironmentDevice& env);

}  // namespace Corona::Systems::Vision
