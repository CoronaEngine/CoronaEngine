//
// Created by Zero on 2023/9/11.
//

#include "base/integral/integrator.h"
#include "base/integral/radiance_cache.h"
#include "base/mgr/pipeline.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "ReSTIR/direct.h"
#include "ReSTIR/indirect.h"
#include <cstdlib>

namespace vision {
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

class RealTimeIntegrator : public IlluminationIntegrator,
                           public enable_shared_from_this<RealTimeIntegrator>,
                           public Observer {
private:
    SP<ReSTIRDI> direct_;
    SP<ReSTIRGI> indirect_;
    SP<ScreenBuffer> specular_buffer_{make_shared<ScreenBuffer>("RealTimeIntegrator::specular_buffer_")};
    Shader<void(uint, float, float)> combine_;
    Shader<void(uint, Buffer<SurfaceData>)> path_tracing_;
    SP<RadianceCache> cache_;

public:
    RealTimeIntegrator() = default;
    explicit RealTimeIntegrator(const IntegratorDesc &desc)
        : IlluminationIntegrator(desc), cache_(Node::create_shared<RadianceCache>(desc.cache_desc)) {
        max_depth_ = max_depth_.hv() - 1;
    }

    void initialize_(const vision::NodeDesc &node_desc) noexcept override {
        const Desc &desc = static_cast<const Desc &>(node_desc);
        direct_ = make_shared<ReSTIRDI>(shared_from_this(), desc["direct"]);
        indirect_ = make_shared<ReSTIRGI>(shared_from_this(), desc["indirect"]);
        cache_->set_integrator(shared_from_this());
    }

    VS_MAKE_GUI_STATUS_FUNC(IlluminationIntegrator, direct_, indirect_)

    void restore(vision::RuntimeObject *old_obj) noexcept override {
        IlluminationIntegrator::restore(old_obj);
        VS_HOTFIX_MOVE_ATTRS(direct_, indirect_, specular_buffer_,
                             combine_, path_tracing_, denoiser_)
        direct_->set_integrator(shared_from_this());
        indirect_->set_integrator(shared_from_this());
        cache_->set_integrator(shared_from_this());
    }

    void update_resolution(ocarina::uint2 res) noexcept override {
        direct_->update_resolution(res);
        indirect_->update_resolution(res);
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->update_resolution(res);
        }
    }

    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override {
        std::tuple tp = {addressof(direct_), addressof(indirect_), addressof(cache_)};
        HotfixSystem::replace_objects(constructor, tp);
    }

    VS_MAKE_PLUGIN_NAME_FUNC
    void prepare() noexcept override {
        IlluminationIntegrator::prepare();
        direct_->prepare();
        indirect_->prepare();
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->prepare();
        }
        cache_->prepare();
        Pipeline *rp = pipeline();

        frame_buffer().prepare_screen_buffer(specular_buffer_);
        frame_buffer().prepare_hit_bsdfs();
        frame_buffer().prepare_surfaces();
        frame_buffer().prepare_surface_exts();
        frame_buffer().prepare_visibility_buffer();
        frame_buffer().prepare_motion_vectors();
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        direct_->render_UI(widgets);
        indirect_->render_UI(widgets);
        cache_->render_UI(widgets);
    }

    void compile() noexcept override {
        direct_->compile();
        indirect_->compile();
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->compile();
        }
        TSensor &camera = scene().sensor();
        Kernel kernel = [&](Uint frame_index, Float di, Float ii) {
            camera->load_data();
            Float3 direct = direct_->radiance()->read(dispatch_id()).xyz() * di;
            Float3 indirect = indirect_->radiance()->read(dispatch_id()).xyz() * ii;
            Float3 L = direct + indirect;
            frame_buffer().add_sample(dispatch_idx().xy(), L, frame_index);
        };
        combine_ = device().compile(kernel, "combine");
    }

    RealTimeDenoiseInput denoise_input() const noexcept {
        RealTimeDenoiseInput ret;
        TSensor &camera = scene().sensor();
        ret.frame_index = frame_index_;
        ret.resolution = pipeline()->resolution();
        ret.visibility = frame_buffer().cur_visibility_buffer_view(frame_index_);
        ret.prev_visibility = frame_buffer().prev_visibility_buffer_view(frame_index_);
        ret.motion_vec = frame_buffer().motion_vectors();
        ret.direct = direct_->radiance()->view();
        ret.indirect = indirect_->radiance()->view();
        // Camera positions for depth calculation from visibility buffer
        float3 cam_pos = camera->position();
        float3 prev_cam_pos = camera->prev_host_position();
        ret.camera_pos = {cam_pos.x, cam_pos.y, cam_pos.z};
        ret.prev_camera_pos = {prev_cam_pos.x, prev_cam_pos.y, prev_cam_pos.z};
        // ReSTIR path: direct_ = direct lighting, indirect_ = indirect lighting (NOT diffuse/specular).
        ret.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
        return ret;
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

        submit(frame_buffer().compute_GBuffer(frame_index_), &cur_stage_profile_.gbuffer_ms);
        submit(direct_->dispatch(frame_index_), &cur_stage_profile_.path_tracing_ms);
        submit(indirect_->dispatch(frame_index_), &cur_stage_profile_.path_tracing_ms);
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            auto dn_input = denoise_input();
            submit(denoiser_->dispatch(dn_input), &cur_stage_profile_.spatial_angular_ms);
            submit(combine_(frame_index_, direct_->factor(),
                            indirect_->factor())
                       .dispatch(pipeline()->resolution()),
                   &cur_stage_profile_.combine_ms);
        } else {
            submit(combine_(frame_index_, direct_->factor(),
                            indirect_->factor())
                       .dispatch(pipeline()->resolution()),
                   &cur_stage_profile_.combine_ms);
        }
        submit(frame_buffer().post_path_tracing(frame_index_), &cur_stage_profile_.postprocess_ms);
        submit(frame_buffer().render_final(frame_index_), &cur_stage_profile_.render_final_ms);
        increase_frame_index();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, RealTimeIntegrator)
