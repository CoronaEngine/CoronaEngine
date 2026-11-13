#pragma once

#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace pt_denoiser_signal_probe {
using namespace vision;

inline void compile_raw_producer(Pipeline &pipeline, IlluminationIntegrator &integrator) {
    pipeline.activate_global_context();
    Global::SceneGpuContextScope context{pipeline.geometry().bindless_array(), pipeline.device()};
    integrator.compile_path_tracing();
}

inline void dispatch_raw_producer(Pipeline &pipeline, IlluminationIntegrator &integrator, uint frame) {
    auto &fb = *pipeline.frame_buffer();
    PTParam param;
    param.frame_index = frame;
    param.rays = fb.rays().descriptor();
    param.colors = fb.rt_buffer().descriptor();
    param.direct = fb.direct_lighting().descriptor();
    param.indirect = fb.indirect_lighting().descriptor();
    pipeline.stream() << fb.compute_GBuffer(frame) << integrator.path_tracing(param, fb.raytracing_resolution());
}

inline void check_frame(Pipeline &pipeline, IlluminationIntegrator &integrator, bool enabled) {
    auto &fb = *pipeline.frame_buffer();
    const float4 sentinel = make_float4(-1234.f, -2345.f, -3456.f, -4567.f);
    const std::array buffers{&fb.denoiser_diffuse(), &fb.denoiser_specular(), &fb.denoiser_bypass()};
    std::array<std::vector<float4>, 3> signals;
    for (size_t index = 0; index < buffers.size(); ++index) {
        signals[index].assign(buffers[index]->size(), sentinel);
        pipeline.stream() << buffers[index]->view().upload(signals[index].data());
    }
    pipeline.upload_data(); pipeline.display(1.0 / 60.0);
    const double denoise_ms = integrator.cur_denoise_time();
    if (enabled ? !(std::isfinite(denoise_ms) && denoise_ms > 0.0) : denoise_ms != 0.0)
        throw std::runtime_error("PT denoise time must follow OFF/ON producer transitions");
    std::vector<float4> pixels(pipeline.pixel_num());
    for (size_t index = 0; index < buffers.size(); ++index)
        pipeline.stream() << buffers[index]->view().download(signals[index].data());
    pipeline.stream() << fb.rt_buffer().device_buffer().download(pixels.data()) << synchronize() << commit();
    for (const auto &signal : signals) for (size_t index = 0; index < signal.size(); ++index)
        for (uint channel = 0; channel < 4u; ++channel) {
            const float value = signal[index][channel];
            if (!enabled && value != sentinel[channel])
                throw std::runtime_error("PT OFF must preserve every denoiser signal sentinel");
            if (enabled && (!std::isfinite(value) || (index < pipeline.pixel_num() && value == sentinel[channel])))
                throw std::runtime_error("PT ON must overwrite active denoiser signals with finite values");
        }
    bool nonzero = false;
    for (auto pixel : pixels) for (uint channel = 0; channel < 3u; ++channel) {
        if (!std::isfinite(pixel[channel])) throw std::runtime_error("PT denoise transitions must preserve finite colors");
        nonzero |= pixel[channel] > 0.f;
    }
    if (!nonzero) throw std::runtime_error("PT denoise transitions must preserve nonzero scene radiance");
}

} // namespace pt_denoiser_signal_probe
