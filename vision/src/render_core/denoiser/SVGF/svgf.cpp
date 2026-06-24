#include "svgf.h"
#include "svgf_config.h"
#include "variance_estimator.h"
#include "prefilter.h"
#include <cstdlib>

namespace vision::svgf {

using Cfg = SVGFConfig;
namespace {
[[nodiscard]] bool env_flag(const char *name) noexcept {
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}
}

void SVGF::prepare_buffers() {
    uint pixel_num = pipeline()->pixel_num();
    auto rt_res = pipeline()->frame_buffer()->raytracing_resolution();
    OC_INFO_FORMAT("SVGF::prepare_buffers pipeline_pixel_num={}, framebuffer=({}, {}), raytracing=({}, {})",
                   pixel_num,
                   pipeline()->frame_buffer()->resolution().x, pipeline()->frame_buffer()->resolution().y,
                   rt_res.x, rt_res.y);
    init_buffer_zero(device(), svgf_data, pixel_num, "SVGF::svgf_data");
    svgf_data.register_self(0, pixel_num);
    init_buffer_zero(device(), svgf_data2, pixel_num, "SVGF::svgf_data2");
    svgf_data2.register_self(0, pixel_num);
}

void SVGF::compute_GBuffer(const vision::RayState &rs, const vision::Interaction &it) noexcept {
}

void SVGF::initialize_(const vision::NodeDesc &node_desc) noexcept {
    atrous_ = make_shared<AtrousFilter>(this);
    modulator_ = make_shared<Modulator>(this);
    variance_estimator_ = make_shared<VarianceEstimator>(this);
    prefilter_ = make_shared<Prefilter>(this);
}

void SVGF::render_sub_UI(Widgets *widgets) noexcept {
    bool enabled = params_.switch_;
    if (widgets->check_box("turn on", &enabled)) {
        changed_ = true;
        set_enabled(enabled);
    }
    changed_ |= widgets->input_float_limit("sigma_rt", &params_.sigma_rt_,
                                           0.01, 1e10, 1, 3);
    changed_ |= widgets->input_float_limit("sigma_normal", &params_.sigma_normal_,
                                           0.01, 1e10, 1, 3);
    changed_ |= widgets->input_float_limit("sigma_depth", &params_.sigma_depth_,
                                           0.01, 10.0, 0.1, 0.5);
}

BufferView<SVGFDataDual> SVGF::svgf_buffer_cur(uint frame_index) const noexcept {
    return ((frame_index & 1u) == 0u) ? svgf_data.view() : svgf_data2.view();
}

BufferView<SVGFDataDual> SVGF::svgf_buffer_prev(uint frame_index) const noexcept {
    return ((frame_index & 1u) == 0u) ? svgf_data2.view() : svgf_data.view();
}

void SVGF::prepare() noexcept {
    prepare_buffers();
    frame_buffer().register_callback(shared_from_this());
    atrous_->prepare();
    modulator_->prepare();
    variance_estimator_->prepare();
    prefilter_->prepare();
}

void SVGF::compile() noexcept {
    atrous_->compile();
    modulator_->compile();
    variance_estimator_->compile();
    prefilter_->compile();
}

CommandBatch SVGF::dispatch(vision::RealTimeDenoiseInput &input) noexcept {
    CommandBatch ret;
    if (params_.switch_) {
        const bool radiance_domain = env_flag("VISION_SVGF_RADIANCE_DOMAIN");
        const bool skip_variance = env_flag("VISION_SVGF_SKIP_VARIANCE");
        const bool skip_prefilter = env_flag("VISION_SVGF_SKIP_PREFILTER") || !params_.spatial_filter_;
        const bool skip_atrous = env_flag("VISION_SVGF_SKIP_ATROUS") || !params_.spatial_filter_;

        if (!radiance_domain) {
            ret << modulator_->demodulate(input);
        }
        if (!skip_variance) {
            ret << variance_estimator_->dispatch_variance(input);
        }
        if (!skip_prefilter) {
            ret << prefilter_->dispatch(input);
        }
        
        if (!skip_atrous) {
            for (uint i = 0; i < Cfg::Atrous::kIterationCount; ++i) {
                ret << atrous_->dispatch_combined(input, Cfg::Atrous::kStepSizes[i], i);
            }
        }
        if (!radiance_domain) {
            ret << modulator_->modulate(input);
        }
    }
    return ret;
}

void SVGF::set_enabled(bool enabled) noexcept {
    params_.switch_ = enabled;
    if (enabled && frame_buffer().enable_accumulation()) {
        frame_buffer().set_enable_accumulation(false);
        frame_buffer().auto_manage_accumulation_buffer(false);
    }
}

bool SVGF::enabled() noexcept {
    return params_.switch_;
}

void SVGF::update_resolution(uint2 resolution) noexcept {
    uint pixel_num = resolution.x * resolution.y;
    OC_INFO_FORMAT("SVGF::update_resolution input=({}, {}), pixel_num={}, framebuffer=({}, {}), pipeline_pixel_num={}, raytracing=({}, {})",
                   resolution.x, resolution.y, pixel_num,
                   frame_buffer().resolution().x, frame_buffer().resolution().y,
                   pipeline()->pixel_num(),
                   frame_buffer().raytracing_resolution().x, frame_buffer().raytracing_resolution().y);
    init_buffer_zero(device(), svgf_data.super(), pixel_num, "SVGF::svgf_data");
    svgf_data.register_self(0, pixel_num);
    init_buffer_zero(device(), svgf_data2.super(), pixel_num, "SVGF::svgf_data2");
    svgf_data2.register_self(0, pixel_num);
    atrous_->update_resolution(resolution);
    variance_estimator_->update_resolution(resolution);
}

}// namespace vision::svgf

VS_MAKE_CLASS_CREATOR_HOTFIX_DIRECTORY(vision::svgf, SVGF, 1)
