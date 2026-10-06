#include "vision_render_mode_config.h"

#include <string>

namespace Corona::Systems::Vision {

std::string_view vision_render_mode_name(CameraVisionRenderMode mode) noexcept {
    switch (mode) {
        case CameraVisionRenderMode::ReSTIR:
            return "restir";
        case CameraVisionRenderMode::ProgressivePathTracing:
            return "progressive_path_tracing";
        case CameraVisionRenderMode::SVGF:
            return "svgf";
        case CameraVisionRenderMode::SSAT:
            return "ssat";
        case CameraVisionRenderMode::PathTracing:
        default:
            return "path_tracing";
    }
}

bool vision_render_mode_uses_denoise(CameraVisionRenderMode mode) noexcept {
    // Legacy combined modes remain readable; algorithms have a separate camera toggle.
    return mode == CameraVisionRenderMode::SVGF || mode == CameraVisionRenderMode::SSAT;
}

namespace {

::vision::DataWrap& ensure_vision_json_object(::vision::DataWrap& parent,
                                              std::string_view key) {
    const auto name = std::string(key);
    if (!parent.contains(name) || !parent[name].is_object()) {
        parent[name] = ::vision::DataWrap::object();
    }
    return parent[name];
}

template <typename T>
void set_default_vision_json_value(::vision::DataWrap& parent, std::string_view key,
                                   T value) {
    const auto name = std::string(key);
    if (!parent.contains(name)) {
        parent[name] = value;
    }
}

void apply_ssat_denoiser_defaults(::vision::DataWrap& denoiser_param) {
    set_default_vision_json_value(denoiser_param, "rho_base", 1.0f);
    set_default_vision_json_value(denoiser_param, "alpha_shear", 1.0f);
    set_default_vision_json_value(denoiser_param, "sigma_lum", 6.0f);
    set_default_vision_json_value(denoiser_param, "sigma_normal", 128.0f);
    set_default_vision_json_value(denoiser_param, "sigma_depth_angular", 100.0f);
    set_default_vision_json_value(denoiser_param, "sigma_x", 3.0f);
    set_default_vision_json_value(denoiser_param, "sigma_u", 0.5f);
    set_default_vision_json_value(denoiser_param, "beta", 1.0f);
    set_default_vision_json_value(denoiser_param, "angular_range", 1.0f);
    set_default_vision_json_value(denoiser_param, "spatial_radius", 1);
    set_default_vision_json_value(denoiser_param, "angular_samples", 7);
    set_default_vision_json_value(denoiser_param, "alpha_base", 0.1f);
    set_default_vision_json_value(denoiser_param, "angular_bandwidth", 0.1f);
    set_default_vision_json_value(denoiser_param, "enabled", true);
    set_default_vision_json_value(denoiser_param, "use_adaptive_sampling", true);
}

void apply_ssat_framebuffer_defaults(::vision::DataWrap& frame_buffer_param) {
    set_default_vision_json_value(frame_buffer_param, "accumulation", true);

    auto& lenticular = ensure_vision_json_object(frame_buffer_param, "lenticular");
    set_default_vision_json_value(lenticular, "pe", 19.1849f);
    set_default_vision_json_value(lenticular, "angle", 0.2333f);
    set_default_vision_json_value(lenticular, "offset", 10.0f);
    set_default_vision_json_value(lenticular, "num_views", 48);
    set_default_vision_json_value(lenticular, "res_w", 1280);
    set_default_vision_json_value(lenticular, "res_h", 720);

    auto& geometry = ensure_vision_json_object(frame_buffer_param, "geometry");
    set_default_vision_json_value(geometry, "d_f", 4.2f);
    set_default_vision_json_value(geometry, "fov_h_deg", 45.0f);
    set_default_vision_json_value(geometry, "aspect", 0.75f);
    set_default_vision_json_value(geometry, "array_angle_deg", 5.0f);
}

}  // namespace

void configure_vision_scene_for_mode(::vision::DataWrap& data,
                                     CameraVisionRenderMode mode) {
    auto& output = ensure_vision_json_object(data, "output");
    output["denoise"] = vision_render_mode_uses_denoise(mode);

    auto& render = ensure_vision_json_object(data, "render");
    auto& integrator = ensure_vision_json_object(render, "integrator");
    if (mode == CameraVisionRenderMode::ReSTIR) {
        integrator["type"] = "rt";
    } else if (mode == CameraVisionRenderMode::PathTracing ||
               mode == CameraVisionRenderMode::ProgressivePathTracing) {
        integrator["type"] = "pt";
    }
    auto& integrator_param = ensure_vision_json_object(integrator, "param");
    if (mode == CameraVisionRenderMode::ReSTIR) {
        // ReSTIR reads nested ParameterSets during its noexcept initialization.
        // Missing blocks must be objects, not JSON null, so leaf defaults work.
        for (const auto channel : {"direct", "indirect"}) {
            auto& parameters = ensure_vision_json_object(integrator_param, channel);
            ensure_vision_json_object(parameters, "spatial");
            ensure_vision_json_object(parameters, "temporal");
        }
    }
    auto& denoiser = ensure_vision_json_object(integrator_param, "denoiser");
    denoiser["type"] = mode == CameraVisionRenderMode::SSAT ? "SSAT" : "svgf";
    auto& denoiser_param = ensure_vision_json_object(denoiser, "param");
    denoiser_param["enabled"] = vision_render_mode_uses_denoise(mode);
    if (mode == CameraVisionRenderMode::SSAT) {
        apply_ssat_denoiser_defaults(denoiser_param);
    }

    auto& pipeline = ensure_vision_json_object(data, "pipeline");
    auto& pipeline_param = ensure_vision_json_object(pipeline, "param");
    auto& frame_buffer = ensure_vision_json_object(pipeline_param, "frame_buffer");
    if (mode == CameraVisionRenderMode::SSAT) {
        frame_buffer["type"] = "lightfield";
    } else {
        set_default_vision_json_value(frame_buffer, "type", "normal");
    }
    auto& frame_buffer_param = ensure_vision_json_object(frame_buffer, "param");
    if (mode == CameraVisionRenderMode::ProgressivePathTracing) {
        frame_buffer_param["accumulation"] = true;
    } else if (mode != CameraVisionRenderMode::SSAT) {
        frame_buffer_param["accumulation"] = false;
    }
    if (mode == CameraVisionRenderMode::SSAT) {
        apply_ssat_framebuffer_defaults(frame_buffer_param);
    }
}

}  // namespace Corona::Systems::Vision
