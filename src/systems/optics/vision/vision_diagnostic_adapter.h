#pragma once

#include <cstdint>
#include <memory>

namespace vision { class Pipeline; }
namespace Corona { enum class CameraOutputMode : std::uint8_t; }

namespace Corona::Systems::Vision {

// Keep one adapter per camera view, alongside its output bridge. The compiled
// kernel references that view's sensor and must not outlive the pipeline.
class VisionDiagnosticAdapter {
public:
    VisionDiagnosticAdapter();
    ~VisionDiagnosticAdapter();
    VisionDiagnosticAdapter(const VisionDiagnosticAdapter&) = delete;
    VisionDiagnosticAdapter& operator=(const VisionDiagnosticAdapter&) = delete;

    [[nodiscard]] static bool supports(CameraOutputMode mode) noexcept;
    // Writes the framebuffer's display source directly with a deterministic
    // center primary ray per pixel. No materials, lights, denoising, accumulation
    // or tonemapping. Caller must bypass the Corona resolve tonemapper as well.
    [[nodiscard]] bool render(::vision::Pipeline& pipeline, CameraOutputMode mode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Corona::Systems::Vision
