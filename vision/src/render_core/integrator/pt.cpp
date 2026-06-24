//
// Created by Zero on 12/09/2022.
//

#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include "base/sensor/light_field_types.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "adaptive/inspector.h"
#include "render_core/denoiser/SSAT/ssat.h"
#include <cstdlib>

namespace vision {
using namespace ocarina;
namespace {
[[nodiscard]] bool denoiser_runtime_disabled() noexcept {
    const char *value = std::getenv("VISION_DISABLE_DENOISER");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

[[nodiscard]] bool stage_profile_runtime_enabled() noexcept {
    const char *value = std::getenv("VISION_STAGE_PROFILE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}
}
class PathTracingIntegrator : public IlluminationIntegrator, public Observer{
private:
    SP<ConvergenceInspector> inspector_;
    Shader<void(uint)> combine_;
    Shader<void()> merge_indirect_into_direct_;

    SP<ScreenBuffer> taa_history_;
    Shader<void(uint)> taa_shader_;

public:
    PathTracingIntegrator() = default;
    explicit PathTracingIntegrator(const IntegratorDesc &desc)
        : IlluminationIntegrator(desc),
          inspector_(make_shared<ConvergenceInspector>(desc.value("adaptive"))),
          taa_history_(make_shared<ScreenBuffer>("taa_history")) {}

    VS_MAKE_PLUGIN_NAME_FUNC
    void render_sub_UI(Widgets *widgets) noexcept override {
        inspector_->render_UI(widgets);
    }
    
    void update_resolution(ocarina::uint2 res) noexcept override {
        uint2 rt_res = frame_buffer().raytracing_resolution();
        OC_INFO_FORMAT("PathTracingIntegrator::update_resolution input=({}, {}), framebuffer=({}, {}), raytracing=({}, {})",
                       res.x, res.y,
                       frame_buffer().resolution().x, frame_buffer().resolution().y,
                       rt_res.x, rt_res.y);
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->update_resolution(rt_res);
        }
//        taa_history_->update_resolution(rt_res, device());
    }

    void prepare() noexcept override {
        IlluminationIntegrator::prepare();
//        inspector_->prepare();
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->prepare();
        }
        frame_buffer().prepare_visibility_buffer();
        frame_buffer().prepare_direct_lighting();
        frame_buffer().prepare_indirect_lighting();
        frame_buffer().prepare_motion_vectors();
//        frame_buffer().prepare_screen_buffer(taa_history_);
    }
    VS_HOTFIX_MAKE_RESTORE(IlluminationIntegrator, inspector_)
    OC_ENCODABLE_FUNC(IlluminationIntegrator, inspector_)
    VS_MAKE_GUI_STATUS_FUNC(IlluminationIntegrator, inspector_)
    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override {
        std::tuple tp = {addressof(inspector_)};
        HotfixSystem::replace_objects(constructor, tp);
    }

    void compile() noexcept override {
        ILightFieldFrameBuffer *lf_fb = dynamic_cast<ILightFieldFrameBuffer *>(&frame_buffer());
        bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        bool compatible_lightfield_denoiser = denoiser_ && denoiser_->supports_lightfield();
        bool should_compile_denoiser = denoiser_enabled && (!lf_fb || compatible_lightfield_denoiser);
        OC_INFO_FORMAT("PathTracingIntegrator::compile begin framebuffer=({}, {}), raytracing=({}, {}), denoiser_enabled={}, lightfield_fb={}, lightfield_compatible={}",
                       frame_buffer().resolution().x, frame_buffer().resolution().y,
                       frame_buffer().raytracing_resolution().x, frame_buffer().raytracing_resolution().y,
                       denoiser_enabled, lf_fb != nullptr, compatible_lightfield_denoiser);
        TSensor &camera = scene().sensor();
        TSampler &sampler = renderer().sampler();

        Kernel combine_kernel = [&](Uint frame_index) {
            camera->load_data();
            frame_buffer().load_data();
            load_data();
            RadType3Var direct = frame_buffer().direct_lighting().read(dispatch_id()).xyz();
            RadType3Var indirect = frame_buffer().indirect_lighting().read(dispatch_id()).xyz();
            Float3 L = make_float3(direct + indirect);
            frame_buffer().add_sample(dispatch_idx().xy(), L, frame_index);
        };
        combine_ = device().compile(combine_kernel, "combine");

        // Merge kernel: combines indirect into direct, zeros indirect
        Kernel merge_kernel = [&]() {
            frame_buffer().load_data();
            Uint idx = dispatch_id();
            auto d = frame_buffer().direct_lighting().read(idx);
            auto i = frame_buffer().indirect_lighting().read(idx);
            Float3 merged = d.xyz() + i.xyz();
            frame_buffer().direct_lighting().write(idx, make_RadType4(make_float4(merged, d.w)));
            frame_buffer().indirect_lighting().write(idx, make_RadType4(make_float4(0.f)));
        };
        merge_indirect_into_direct_ = device().compile(merge_kernel, "merge_indirect_direct");

//        Kernel taa_kernel = [&](Uint frame_index) {
//            camera->load_data();
//            frame_buffer().load_data();
//
//            Int2 screen_size = make_int2(dispatch_dim().xy());
//            Int2 cur_pixel = make_int2(dispatch_idx().xy());
//
//            Float2 motion_vec = frame_buffer().motion_vectors().read(dispatch_id());
//            Float2 prev_pixel_uv = make_float2(cur_pixel) + 0.5f - motion_vec;
//
//            Bool valid_history = prev_pixel_uv.x >= 0 && prev_pixel_uv.x < screen_size.x &&
//                                 prev_pixel_uv.y >= 0 && prev_pixel_uv.y < screen_size.y &&
//                                 frame_index > 0u;
//
//            Float4 cur_color = frame_buffer().rt_buffer().read(dispatch_id());
//
//            Float3 color_min = make_float3(1e10f);
//            Float3 color_max = make_float3(-1e10f);
//            Float3 color_avg = make_float3(0.f);
//            for (int dy = -1; dy <= 1; ++dy) {
//                for (int dx = -1; dx <= 1; ++dx) {
//                    Int2 neighbor_pixel = clamp(cur_pixel + make_int2(dx, dy),
//                                                make_int2(0), screen_size - 1);
//                    Float3 neighbor_color = frame_buffer().rt_buffer().read(
//                        dispatch_id(make_uint2(neighbor_pixel))).xyz();
//                    color_min = min(color_min, neighbor_color);
//                    color_max = max(color_max, neighbor_color);
//                    color_avg = color_avg + neighbor_color;
//                }
//            }
//            color_avg = color_avg / 9.f;
//
//            Float3 aabb_center = (color_min + color_max) * 0.5f;
//            Float3 aabb_extent = (color_max - color_min) * 0.5f + 0.001f;
//            color_min = aabb_center - aabb_extent;
//            color_max = aabb_center + aabb_extent;
//
//            Float3 history_color = cur_color.xyz();
//            $if(valid_history) {
//                Float2 sample_pos = prev_pixel_uv - 0.5f;
//                Float2 sample_floor = floor(sample_pos);
//                Float2 frac_uv = sample_pos - sample_floor;
//
//                Int2 p00 = make_int2(sample_floor);
//                Int2 p10 = p00 + make_int2(1, 0);
//                Int2 p01 = p00 + make_int2(0, 1);
//                Int2 p11 = p00 + make_int2(1, 1);
//
//                p00 = clamp(p00, make_int2(0), screen_size - 1);
//                p10 = clamp(p10, make_int2(0), screen_size - 1);
//                p01 = clamp(p01, make_int2(0), screen_size - 1);
//                p11 = clamp(p11, make_int2(0), screen_size - 1);
//
//                Float3 c00 = taa_history_->read(dispatch_id(make_uint2(p00))).xyz();
//                Float3 c10 = taa_history_->read(dispatch_id(make_uint2(p10))).xyz();
//                Float3 c01 = taa_history_->read(dispatch_id(make_uint2(p01))).xyz();
//                Float3 c11 = taa_history_->read(dispatch_id(make_uint2(p11))).xyz();
//
//                Float fx = frac_uv.x;
//                Float fy = frac_uv.y;
//                Float3 c0 = c00 * (1.f - fx) + c10 * fx;
//                Float3 c1 = c01 * (1.f - fx) + c11 * fx;
//                history_color = c0 * (1.f - fy) + c1 * fy;
//
//                history_color = clamp(history_color, color_min, color_max);
//            };
//
//            Float motion_blend_factor = saturate(length(motion_vec) / 16.f);
//            Float alpha = lerp(0.1f, 0.5f, motion_blend_factor);
//            Float3 result = (1.0f - alpha) * history_color + alpha * cur_color.xyz();
//
//            frame_buffer().rt_buffer().write(dispatch_id(), make_float4(result, 1.0f));
//            taa_history_->write(dispatch_id(), make_float4(result, 1.0f));
//        };
//        taa_shader_ = device().compile(taa_kernel, "TAA temporal accumulation");

        compile_path_tracing();
        OC_INFO_FORMAT("PathTracingIntegrator::compile after compile_path_tracing framebuffer=({}, {}), raytracing=({}, {})",
                       frame_buffer().resolution().x, frame_buffer().resolution().y,
                       frame_buffer().raytracing_resolution().x, frame_buffer().raytracing_resolution().y);
        if (denoiser_enabled && lf_fb && !compatible_lightfield_denoiser) {
            OC_WARNING_FORMAT("Bypassing incompatible denoiser '{}' for light-field framebuffer",
                              denoiser_->impl_type().data());
        }
        if (should_compile_denoiser) {
            denoiser_->compile();
        }
    }

    void render() const noexcept override {
        const Pipeline *rp = pipeline();
        Stream &stream = rp->stream();
        cur_stage_profile_ = {};
        cur_stage_profile_.enabled = stage_profile_runtime_enabled();
        auto submit = [&](auto &&command, double *stage_ms) {
            if (!cur_stage_profile_.enabled) {
                stream << command;
                return;
            }
            Clock clk;
            stream << command;
            stream << synchronize() << commit();
            if (stage_ms != nullptr) {
                *stage_ms += clk.elapse_ms();
            }
        };
        if (frame_index_ == 0) {
//            stream << inspector_->reset();
        }
        submit(frame_buffer().compute_GBuffer(frame_index_), &cur_stage_profile_.gbuffer_ms);

        PTParam param;
        param.frame_index = frame_index_;
        param.rays = frame_buffer().rays().descriptor();
        param.colors = frame_buffer().rt_buffer().descriptor();
        param.direct = frame_buffer().direct_lighting().descriptor();
        param.indirect = frame_buffer().indirect_lighting().descriptor();

        TSensor &camera = scene().sensor();
        float3 cam_pos = camera->position();
        float3 prev_cam_pos = camera->prev_host_position();

        // Check if we're using a light field frame buffer and SSAT denoiser
        ILightFieldFrameBuffer *lf_fb = dynamic_cast<ILightFieldFrameBuffer*>(&frame_buffer());
        bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        bool use_lightfield_denoise = lf_fb != nullptr && denoiser_ && denoiser_->supports_lightfield() && denoiser_enabled;
        bool bypass_incompatible_lightfield_denoiser = lf_fb != nullptr && denoiser_ && denoiser_enabled && !denoiser_->supports_lightfield();

        if (use_lightfield_denoise) {
            // Light field denoising path - get parameters from LightFieldFrameBuffer
            LightFieldDenoiseInput lf_input;
            lf_input.frame_index = frame_index_;
            lf_input.resolution = frame_buffer().raytracing_resolution();
            lf_input.visibility = frame_buffer().cur_visibility_buffer_view(frame_index_);
            lf_input.prev_visibility = frame_buffer().prev_visibility_buffer_view(frame_index_);
            lf_input.motion_vec = frame_buffer().motion_vectors();
            lf_input.indirect = frame_buffer().indirect_lighting();
            lf_input.direct = frame_buffer().direct_lighting();
            lf_input.camera_pos = {cam_pos.x, cam_pos.y, cam_pos.z};
            lf_input.prev_camera_pos = {prev_cam_pos.x, prev_cam_pos.y, prev_cam_pos.z};
            // PT path splits the signal into diffuse (direct buffer) / specular (indirect buffer).
            lf_input.channel_kind = RealTimeDenoiseInput::ChannelKind::DiffuseSpecular;
            
            // Get light field parameters from the actual LightFieldFrameBuffer
            lf_input.lenticular = lf_fb->lenticular_params();
            lf_input.geometry = lf_fb->geometry_params();
            lf_input.l2w = lf_fb->get_current_l2w();
            lf_input.prev_l2w = lf_fb->get_prev_l2w();
            if (frame_index_ == 0u) {
                OC_INFO_FORMAT("PT lightfield denoise input: resolution=({}, {}), lenticular=({}, {})",
                               lf_input.resolution.x, lf_input.resolution.y,
                               lf_input.lenticular.res_w, lf_input.lenticular.res_h);
            }

            auto *ssat_denoiser = dynamic_cast<ssat::SSAT *>(denoiser_.get());
            if (ssat_denoiser != nullptr) {
                submit(ssat_denoiser->prepass_sampling_mask(lf_input), &cur_stage_profile_.sampling_mask_ms);
                param.enable_sparse_sampling = ssat_denoiser->use_adaptive_sampling() ? 1u : 0u;
                if (param.enable_sparse_sampling != 0u) {
                    param.sampling_mask = ssat_denoiser->sampling_mask().descriptor();
                }
            }

            if (denoiser_enabled) {
                submit(path_tracing(param, frame_buffer().raytracing_resolution()), &cur_stage_profile_.path_tracing_ms);
                if (ssat_denoiser != nullptr) {
                    // Merge indirect into direct (always needed)
                    submit(merge_indirect_into_direct_().dispatch(frame_buffer().raytracing_resolution()), nullptr);

                    bool skip_phase2 = (std::getenv("SSAT_SKIP_PHASE2") != nullptr);
                    bool skip_phase3 = (std::getenv("SSAT_SKIP_PHASE3") != nullptr);

                    BufferView<RadType4> spatial_result;

                    if (!skip_phase2) {
                        auto spatial = ssat_denoiser->dispatch_spatial_filter(lf_input);
                        submit(spatial.commands, &cur_stage_profile_.spatial_angular_ms);
                        spatial_result = spatial.spatial_result;
                    } else {
                        ssat_denoiser->ensure_phase_buffers(lf_input.resolution);
                        auto temp_view = ssat_denoiser->spatial_angular_filter()->temp_buffer().view();
                        submit(temp_view.copy_from(lf_input.direct), nullptr);
                        spatial_result = temp_view;
                    }

                    if (!skip_phase3) {
                        submit(ssat_denoiser->dispatch_temporal_accumulation(lf_input, spatial_result),
                               &cur_stage_profile_.temporal_ms);
                    } else {
                        submit(lf_input.direct.copy_from(spatial_result), nullptr);
                    }
                } else {
                    submit(denoiser_->dispatch_lightfield(lf_input), &cur_stage_profile_.spatial_angular_ms);
                }
                submit(combine_(frame_index_).dispatch(frame_buffer().raytracing_resolution()),
                       &cur_stage_profile_.combine_ms);
            } else {
                param.enable_sparse_sampling = 0u;
                submit(path_tracing(param, frame_buffer().raytracing_resolution()), &cur_stage_profile_.path_tracing_ms);
                submit(combine_(frame_index_).dispatch(frame_buffer().raytracing_resolution()),
                       &cur_stage_profile_.combine_ms);
            }
        } else {
            // Standard denoising path
            RealTimeDenoiseInput dn_input;
            dn_input.frame_index = frame_index_;
            dn_input.resolution = frame_buffer().raytracing_resolution();
            dn_input.visibility = frame_buffer().cur_visibility_buffer_view(frame_index_);
            dn_input.prev_visibility = frame_buffer().prev_visibility_buffer_view(frame_index_);
            dn_input.motion_vec = frame_buffer().motion_vectors();
            dn_input.indirect = frame_buffer().indirect_lighting();
            dn_input.direct = frame_buffer().direct_lighting();
            dn_input.camera_pos = {cam_pos.x, cam_pos.y, cam_pos.z};
            dn_input.prev_camera_pos = {prev_cam_pos.x, prev_cam_pos.y, prev_cam_pos.z};
            // PT path splits the signal into diffuse (direct buffer) / specular (indirect buffer).
            dn_input.channel_kind = RealTimeDenoiseInput::ChannelKind::DiffuseSpecular;
            param.enable_sparse_sampling = 0u;

            if (denoiser_enabled && !bypass_incompatible_lightfield_denoiser) {
                submit(path_tracing(param, frame_buffer().raytracing_resolution()), &cur_stage_profile_.path_tracing_ms);
                submit(denoiser_->dispatch(dn_input), &cur_stage_profile_.spatial_angular_ms);
                submit(combine_(frame_index_).dispatch(frame_buffer().raytracing_resolution()),
                       &cur_stage_profile_.combine_ms);
            } else {
                submit(path_tracing(param, frame_buffer().raytracing_resolution()), &cur_stage_profile_.path_tracing_ms);
                submit(combine_(frame_index_).dispatch(frame_buffer().raytracing_resolution()),
                       &cur_stage_profile_.combine_ms);
            }
        }
        
        // Post-processing hook for custom frame buffers (e.g. light field interlacing)
        submit(frame_buffer().post_path_tracing(frame_index_), &cur_stage_profile_.postprocess_ms);
        // Final presentation stage (may differ across frame buffer implementations)
        submit(frame_buffer().render_final(frame_index_), &cur_stage_profile_.render_final_ms);

        increase_frame_index();
    }
};
}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, PathTracingIntegrator)
