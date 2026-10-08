//
// Created by Zero on 2023/9/11.
//

#include "base/integral/integrator.h"
#include "base/integral/radiance_cache.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/evaluation_debug.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "ReSTIR/direct.h"
#include "ReSTIR/indirect.h"
#include "ReSTIR/stable_planes.h"
#include <cstdlib>
#include <fstream>

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
    SP<StablePlanes> stable_planes_;
    SP<ScreenBuffer> specular_buffer_{make_shared<ScreenBuffer>("RealTimeIntegrator::specular_buffer_")};
    Shader<void(uint, float, float, uint)> combine_;
    Shader<void()> merge_dominant_;
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
        stable_planes_ = make_shared<StablePlanes>(shared_from_this(), direct_->max_recursion());
        cache_->set_integrator(shared_from_this());
    }

    VS_MAKE_GUI_STATUS_FUNC(IlluminationIntegrator, direct_, indirect_)

    void restore(vision::RuntimeObject *old_obj) noexcept override {
        IlluminationIntegrator::restore(old_obj);
        VS_HOTFIX_MOVE_ATTRS(direct_, indirect_, stable_planes_, specular_buffer_,
                             combine_, merge_dominant_, path_tracing_, denoiser_)
        direct_->set_integrator(shared_from_this());
        indirect_->set_integrator(shared_from_this());
        stable_planes_->set_integrator(shared_from_this());
        cache_->set_integrator(shared_from_this());
    }

    void update_resolution(ocarina::uint2 res) noexcept override {
        direct_->update_resolution(res);
        indirect_->update_resolution(res);
        if (!denoiser_runtime_disabled() && denoiser_ &&
            (denoiser_->enabled() || denoiser_->has_prepared_resources())) {
            denoiser_->update_resolution(res);
        }
    }

    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override {
        std::tuple tp = {addressof(direct_), addressof(indirect_), addressof(stable_planes_), addressof(cache_)};
        HotfixSystem::replace_objects(constructor, tp);
    }

    VS_MAKE_PLUGIN_NAME_FUNC
    [[nodiscard]] bool jitter_primary_samples() const noexcept override {
        // A raw frame is displayed directly, so shared film jitter otherwise
        // moves every silhouette even with a stationary camera. Keep stochastic
        // lighting/lens sampling and retain film jitter for reconstructed output.
        return frame_buffer().enable_accumulation() ||
               (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled());
    }
    void prepare() noexcept override {
        switch_profile::Scope profile{"integrator.prepare", "buffers"};
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
        stable_planes_->prepare();
    }

    [[nodiscard]] bool stable_planes_enabled() const noexcept override {
        return direct_->stable_planes_enabled();
    }

    void set_stable_planes_enabled(bool enabled) noexcept override {
        if (stable_planes_enabled() == enabled) { return; }
        direct_->set_stable_planes_enabled(enabled);
        // All producers and consumers switch together. Restart reservoirs,
        // SVGF and frame accumulation so primary and virtual guides never mix.
        pipeline()->invalidate();
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        bool stable_planes = stable_planes_enabled();
        if (widgets->check_box("Stable Plane", &stable_planes)) {
            set_stable_planes_enabled(stable_planes);
        }
        direct_->render_UI(widgets);
        indirect_->render_UI(widgets);
        cache_->render_UI(widgets);
    }

    void compile() noexcept override {
        switch_profile::Scope profile{"integrator.compile", "compile"};
        direct_->compile();
        indirect_->compile();
        stable_planes_->set_max_recursion(direct_->max_recursion());
        stable_planes_->compile();
        if (!denoiser_runtime_disabled() && denoiser_ &&
            (denoiser_->enabled() || denoiser_->has_prepared_resources())) {
            denoiser_->compile();
        }
        TSensor &camera = scene().sensor();
        Kernel kernel = [&](Uint frame_index, Float di, Float ii, Uint layered) {
            camera->load_data();
            Float3 direct = direct_->radiance()->read(dispatch_id()).xyz();
            Float3 indirect = indirect_->radiance()->read(dispatch_id()).xyz();
            Float3 L = direct * di + indirect * ii;
            $if(layered != 0u) {
                L = frame_buffer().stable_radiance().read(dispatch_id()).xyz() * di;
                for (uint layer = 0; layer < StablePlaneCount; ++layer) {
                    Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                    Float3 ld = frame_buffer().stable_direct().read(address).xyz();
                    Float3 li = frame_buffer().stable_indirect().read(address).xyz();
                    L += ld * di + li * ii;
                }
            };
            frame_buffer().add_sample(dispatch_idx().xy(), L, frame_index);
        };
        Kernel merge = [&] {
            Uint dominant = frame_buffer().stable_dominant().read(dispatch_id());
            $if(dominant < StablePlaneCount) {
                Uint address = dominant * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                Float3 direct = frame_buffer().stable_direct().read(address).xyz() + direct_->radiance()->read(dispatch_id()).xyz();
                Float3 indirect = frame_buffer().stable_indirect().read(address).xyz() + indirect_->radiance()->read(dispatch_id()).xyz();
                frame_buffer().stable_direct().write(address, make_float4(direct, 1.f));
                frame_buffer().stable_indirect().write(address, make_float4(indirect, 1.f));
            };
        };
        merge_dominant_ = device().compile(merge, "StablePlanes-MergeDominant");
        {
            switch_profile::Scope combine_profile{"combine.compile", "compile"};
            combine_ = device().compile(kernel, "combine");
        }
    }

    RealTimeDenoiseInput denoise_input(uint layer = 0u) const noexcept {
        RealTimeDenoiseInput ret;
        TSensor &camera = scene().sensor();
        ret.frame_index = frame_index_;
        ret.resolution = pipeline()->resolution();
        ret.visibility = frame_buffer().cur_visibility_buffer_view(frame_index_);
        ret.prev_visibility = frame_buffer().prev_visibility_buffer_view(frame_index_);
        ret.motion_vec = frame_buffer().motion_vectors();
        ret.use_stable_planes = direct_->uses_stable_planes();
        ret.stable_surfaces = frame_buffer().cur_surfaces_view(frame_index_);
        ret.prev_stable_surfaces = frame_buffer().prev_surfaces_view(frame_index_);
        ret.direct = direct_->radiance()->view();
        ret.indirect = indirect_->radiance()->view();
        if (stable_planes_enabled()) {
            ret.use_stable_planes = true;
            ret.layer_count = StablePlaneCount;
            ret.layer_index = layer;
            ret.stable_planes = frame_buffer().cur_stable_planes_view(frame_index_, layer);
            ret.prev_stable_planes = frame_buffer().prev_stable_planes_view(frame_index_, layer);
            ret.direct = frame_buffer().stable_direct_view(layer);
            ret.indirect = frame_buffer().stable_indirect_view(layer);
        }
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
        if (stable_planes_enabled()) {
            submit(stable_planes_->build(frame_index_, jitter_primary_samples()), &cur_stage_profile_.stable_build_ms);
            submit(stable_planes_->fill(frame_index_), &cur_stage_profile_.stable_fill_ms);
        }
        submit(direct_->dispatch(frame_index_), &cur_stage_profile_.restir_di_ms);
        submit(indirect_->dispatch(frame_index_), &cur_stage_profile_.restir_gi_ms);
        cur_stage_profile_.path_tracing_ms = cur_stage_profile_.restir_di_ms + cur_stage_profile_.restir_gi_ms;
        auto debug_readback = [&](const char *stage) {
            const auto directory = evaluation_debug::directory(frame_index_);
            if (directory.empty()) return;
            fs::create_directories(directory);
            vector<float4> direct(rp->pixel_num()), indirect(rp->pixel_num());
            stream << direct_->radiance()->view().download(direct.data())
                << indirect_->radiance()->view().download(indirect.data()) << synchronize() << commit();
            auto write = [&](const char *channel, const auto &pixels) {
                std::ofstream out(fs::path(directory) / (std::string(stage) + channel + ".f32"), std::ios::binary);
                out.write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * sizeof(float4));
                if (!out) throw std::runtime_error("ReSTIR diagnostic write failed");
            };
            write("_direct", direct); write("_indirect", indirect);
            if (stable_planes_enabled()) {
                for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                    stream << frame_buffer().stable_direct_view(layer).download(direct.data())
                           << frame_buffer().stable_indirect_view(layer).download(indirect.data())
                           << synchronize() << commit();
                    const auto prefix = "_layer_" + std::to_string(layer);
                    write((prefix + "_direct").c_str(), direct);
                    write((prefix + "_indirect").c_str(), indirect);
                }
            }
        };
        if (stable_planes_enabled()) {
            submit(merge_dominant_().dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
        }
        // Opt-in raw layer diagnostics belong here, after merging ownership and
        // before any per-layer denoising. No permanent copy buffers are needed.
        debug_readback("raw");
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            const uint count = stable_planes_enabled() ? StablePlaneCount : 1u;
            for (uint layer = 0u; layer < count; ++layer) {
                auto dn_input = denoise_input(layer);
                submit(denoiser_->dispatch(dn_input), &cur_stage_profile_.spatial_angular_ms);
            }
            debug_readback("filtered");
        }
        // Raw and filtered paths use exactly the same single layer sum.
        submit(combine_(frame_index_, direct_->factor(), indirect_->factor(), uint(stable_planes_enabled()))
                   .dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
        submit(frame_buffer().post_path_tracing(frame_index_), &cur_stage_profile_.postprocess_ms);
        submit(frame_buffer().render_final(frame_index_), &cur_stage_profile_.render_final_ms);
        increase_frame_index();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, RealTimeIntegrator)
