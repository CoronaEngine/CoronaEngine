//
// Created by Zero on 2023/9/11.
//

#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/evaluation_debug.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "ReSTIR/direct.h"
#include "ReSTIR/indirect.h"
#include "ReSTIR/stable_planes.h"
#include "ReSTIR/denoiser_signals.h"
#include "falcor_di_experiment.h"
#include <cstdlib>
#include <fstream>
#include <map>

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

[[nodiscard]] bool precise_lobes_requested() noexcept {
    // Diagnostic oracle: compare producer staging with the original FP32
    // radiance buffers and separate lobe-merge dispatch on the same scenes.
    const char *value = std::getenv("VISION_RESTIR_PRECISE_LOBES");
    return value == nullptr || value[0] != '0';
}

[[nodiscard]] bool raw_stable_outputs_required(bool precise_lobes) noexcept {
    // Evaluation requests retain raw channels for every frame, including setup.
    return !precise_lobes || std::getenv("VISION_EVAL_DEBUG_DIR") ||
        std::getenv("VISION_EVAL_DEBUG_FRAME") || std::getenv("VISION_EVAL_DEBUG_FRAMES") ||
        std::getenv("VISION_EVAL_GI_STORAGE_DEBUG");
}
}

class RealTimeIntegrator : public IlluminationIntegrator,
                           public enable_shared_from_this<RealTimeIntegrator>,
                           public Observer {
private:
    // Validate the complete candidate before applying any live state. Common
    // transport fields use the same validation and float32 normalization as PT.
    [[nodiscard]] static bool merge_restir_settings(DataWrap &settings, const DataWrap &overrides) {
        if (!overrides.is_object()) return false;
        static const std::map<std::string, std::pair<double, double>> ranges{
            {"M_light", {0, 100}}, {"M_bsdf", {0, 100}}, {"max_age", {0, 100}},
            {"max_recursion", {1, 100}}, {"sample_num", {1, 32}}, {"diffuse_factor", {0, 1}},
            {"history_limit", {0, 50}}, {"theta", {0, 90}}, {"depth", {0, 1}}, {"radius", {0, 50}}};
        for (const auto &[key, value] : overrides.items()) {
            if (!settings.contains(key)) return false;
            auto &target = settings[key];
            if (target.is_object()) {
                if (!merge_restir_settings(target, value)) return false;
            } else if (target.is_boolean()) {
                if (!value.is_boolean()) return false;
                target = value;
            } else {
                if (!value.is_number()) return false;
                const bool integer = target.is_number_integer() || target.is_number_unsigned();
                if (integer && !value.is_number_integer() && !value.is_number_unsigned()) return false;
                // ReSTIR owns a primary receiver outside the suffix Li loop.
                if (key == "max_depth" && value.get<double>() < 1.0) return false;
                if (merge_illumination_setting(settings, key, value)) continue;
                const auto range = ranges.find(key);
                if (range == ranges.end()) return false;
                const double number = value.get<double>();
                if (!std::isfinite(number) || number < range->second.first || number > range->second.second)
                    return false;
                if (integer) target = value.get<uint>();
                else target = value.get<float>();
            }
        }
        return true;
    }

    SP<ReSTIRDI> direct_;
    SP<ReSTIRGI> indirect_;
    DataWrap scene_restir_settings_;
    SP<StablePlanes> stable_planes_;
    mutable bool stable_resources_dirty_{};
    Shader<void(uint, float, float, uint, uint)> combine_;
    Shader<void()> merge_dominant_;
    Shader<void()> compose_layers_;
    Shader<void(uint, float, float)> prepare_denoiser_;
    Shader<void(uint, float)> compose_denoiser_;
    mutable float2 denoiser_factors_{make_float2(-1.f)};
    Shader<void(uint, Buffer<SurfaceData>)> path_tracing_;
    mutable std::unique_ptr<FalcorDIExperiment> falcor_di_;

public:
    RealTimeIntegrator() = default;
    explicit RealTimeIntegrator(const IntegratorDesc &desc)
        : IlluminationIntegrator(desc) {
        OC_ERROR_IF(max_depth_.hv() == 0u, "ReSTIR max_depth must be at least 1.");
        max_depth_ = max_depth_.hv() - 1u;
    }

    void initialize_(const vision::NodeDesc &node_desc) noexcept override {
        const Desc &desc = static_cast<const Desc &>(node_desc);
        direct_ = make_shared<ReSTIRDI>(shared_from_this(), desc["direct"]);
        indirect_ = make_shared<ReSTIRGI>(shared_from_this(), desc["indirect"]);
        stable_planes_ = make_shared<StablePlanes>(shared_from_this(), direct_->max_recursion());
        scene_restir_settings_ = settings();
    }

    VS_MAKE_GUI_STATUS_FUNC(IlluminationIntegrator, direct_, indirect_)

    void restore(vision::RuntimeObject *old_obj) noexcept override {
        IlluminationIntegrator::restore(old_obj);
        VS_HOTFIX_MOVE_ATTRS(direct_, indirect_, scene_restir_settings_, stable_planes_, stable_resources_dirty_,
                             combine_, merge_dominant_, compose_layers_, prepare_denoiser_, compose_denoiser_, denoiser_factors_, path_tracing_, denoiser_)
        direct_->set_integrator(shared_from_this());
        indirect_->set_integrator(shared_from_this());
        stable_planes_->set_integrator(shared_from_this());
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
        std::tuple tp = {addressof(direct_), addressof(indirect_), addressof(stable_planes_)};
        HotfixSystem::replace_objects(constructor, tp);
    }

    VS_MAKE_PLUGIN_NAME_FUNC
    [[nodiscard]] bool jitter_primary_samples() const noexcept override {
        const bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        if (denoiser_ && denoiser_->impl_type() == "ReBLUR") {
            return frame_buffer().enable_accumulation() ||
                (denoiser_enabled && denoiser_coverage_aa_enabled(impl_type(), denoiser_->impl_type()));
        }
        if (const char *value = std::getenv("VISION_RESTIR_PRIMARY_JITTER"); value && value[0] != '\0')
            return value[0] != '0';
        return frame_buffer().enable_accumulation() || denoiser_enabled;
    }
    void prepare() noexcept override {
        switch_profile::Scope profile{"integrator.prepare", "buffers"};
        IlluminationIntegrator::prepare();
        const bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        const bool extra_aa = !denoiser_ || denoiser_coverage_aa_enabled(impl_type(), denoiser_->impl_type());
        const bool retain_history = denoiser_enabled && extra_aa;
        const bool precise_lobes = denoiser_enabled && denoiser_->requires_lobe_signals() && precise_lobes_requested();
        const bool compact = !denoiser_enabled || precise_lobes;
        direct_->prepare(compact);
        indirect_->prepare(compact);
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->prepare();
        }
        frame_buffer().prepare_hit_bsdfs();
        frame_buffer().prepare_surfaces();
        frame_buffer().prepare_surface_exts();
        frame_buffer().prepare_surface_ext_history();
        frame_buffer().prepare_visibility_buffer();
        frame_buffer().prepare_motion_vectors();
        stable_planes_->prepare(retain_history, denoiser_ && denoiser_->uses_compact_coverage_history(),
                                raw_stable_outputs_required(precise_lobes));
        if (denoiser_ && denoiser_->requires_lobe_signals()) frame_buffer().prepare_denoiser_signals();
        stable_resources_dirty_ = false;
    }

    [[nodiscard]] bool stable_planes_enabled() const noexcept override {
        return direct_->stable_planes_enabled();
    }

    void set_stable_planes_enabled(bool enabled) noexcept override {
        if (stable_planes_enabled() == enabled) { return; }
        direct_->set_stable_planes_enabled(enabled);
        stable_resources_dirty_ = true;
        // This setter may target an inactive camera. Defer GPU resources until
        // render activates this integrator's own framebuffer/bindless context.
        Integrator::invalidation();
    }

    bool render_UI(Widgets *widgets) noexcept override {
        bool open = widgets->use_folding_header(
            ocarina::format("{} integrator", impl_type().data()),
            [&] {
                changed_ |= widgets->button_click("recompile", [&] { compile(); });
                uint total_depth = max_depth_.hv() + 1u;
                if (widgets->input_uint_limit("max depth", &total_depth, 1, 128, 1, 1)) {
                    max_depth_ = total_depth - 1u;
                    changed_ = true;
                }
                changed_ |= widgets->input_uint_limit("min depth", &min_depth_.hv(), 0, 20, 1, 1);
                changed_ |= widgets->drag_float("rr threshold", &rr_threshold_.hv(), 0.01, 0, 1);
                render_sub_UI(widgets);
            });
        denoiser_->render_UI(widgets);
        return open;
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        bool stable_planes = stable_planes_enabled();
        if (widgets->check_box("Stable Plane", &stable_planes)) {
            set_stable_planes_enabled(stable_planes);
        }
        direct_->render_UI(widgets);
        indirect_->render_UI(widgets);
    }

    [[nodiscard]] DataWrap settings() const override {
        auto result = illumination_settings();
        result["max_depth"] = max_depth_.hv() + 1u;
        result.update(DataWrap{{"direct", direct_->settings()}, {"indirect", indirect_->settings()}});
        return result;
    }

    bool set_settings(const DataWrap &overrides) noexcept override {
        auto candidate = scene_restir_settings_;
        if (!merge_restir_settings(candidate, overrides) || candidate == settings()) return false;
        const uint previous_recursion = direct_->max_recursion();
        // A camera's view and scene GPU context must be active before applying.
        // Finish readers before replacing a shader captured by an earlier frame.
        pipeline()->stream() << synchronize() << commit();
        const bool recompile_direct = direct_->set_settings(candidate["direct"]);
        const bool recompile_indirect = indirect_->set_settings(candidate["indirect"]);
        auto transport = candidate;
        transport["max_depth"] = candidate["max_depth"].get<uint>() - 1u;
        apply_illumination_settings(transport);
        if (recompile_direct || recompile_indirect) {
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            if (recompile_direct) direct_->compile();
            if (recompile_indirect) indirect_->compile();
            if (previous_recursion != direct_->max_recursion()) {
                stable_planes_->set_max_recursion(direct_->max_recursion());
                stable_planes_->compile();
            }
        }
        return true;
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
        compile_composition();
    }

    void compile_composition() noexcept {
        TSensor &camera = scene().sensor();
        Kernel kernel = [&](Uint frame_index, Float di, Float ii, Uint layered, Uint resolved_lobes) {
            camera->load_data();
            Float3 L;
            $if(resolved_lobes != 0u) {
                L = frame_buffer().rt_buffer().read(dispatch_id()).xyz();
            } $else {
                Float3 direct = direct_->read_radiance(dispatch_id()).xyz();
                Float3 indirect = indirect_->read_radiance(dispatch_id()).xyz();
                L = direct * di + indirect * ii;
                $if(layered != 0u) {
                    L = frame_buffer().stable_radiance().read(dispatch_id()).xyz() * di;
                    for (uint layer = 0; layer < StablePlaneCount; ++layer) {
                        Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                        Float3 ld = frame_buffer().stable_direct().read(address).xyz();
                        Float3 li = frame_buffer().stable_indirect().read(address).xyz();
                        L += ld * di + li * ii;
                    }
                };
            };
            frame_buffer().add_sample(dispatch_idx().xy(), L, frame_index);
        };
        Kernel merge = [&] {
            Uint dominant = frame_buffer().stable_dominant().read(dispatch_id());
            $if(dominant < StablePlaneCount) {
                Uint address = dominant * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                Float3 direct = frame_buffer().stable_direct().read(address).xyz() + direct_->read_radiance(dispatch_id()).xyz();
                Float3 indirect = frame_buffer().stable_indirect().read(address).xyz() + indirect_->read_radiance(dispatch_id()).xyz();
                frame_buffer().stable_direct().write(address, make_float4(direct, 1.f));
                frame_buffer().stable_indirect().write(address, make_float4(indirect, 1.f));
            };
        };
        merge_dominant_ = device().compile(merge, "StablePlanes-MergeDominant");
        Kernel compose = [&] {
            Float3 direct = frame_buffer().stable_radiance().read(dispatch_id()).xyz();
            Float3 indirect = make_float3(0.f);
            for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                direct += frame_buffer().stable_direct().read(address).xyz();
                indirect += frame_buffer().stable_indirect().read(address).xyz();
            }
            // Estimator buffers are no longer needed after the dominant merge.
            // Reuse them for the unscaled complete DirectIndirect pixel signal.
            direct_->write_radiance(dispatch_id(), make_float4(direct, 1.f));
            indirect_->write_radiance(dispatch_id(), make_float4(indirect, 1.f));
        };
        compose_layers_ = device().compile(compose, "StablePlanes-ComposeCoverage");
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            Kernel pack_lobes = [&](Uint layered, Float di, Float gi) {
                Uint i = dispatch_id(), layer = 0u;
                $if(layered != 0u) {
                    layer = frame_buffer().stable_dominant().read(i);
                    $if(layer >= StablePlaneCount) { $return(); };
                };
                Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + i;
                Float4 sd = frame_buffer().denoiser_di_specular().read(i);
                Float4 sg = frame_buffer().denoiser_gi_specular().read(i);
                Float3 bypass = frame_buffer().denoiser_di_bypass().read(i).xyz();
                Float4 old_d = make_float4(0.f), old_s = make_float4(0.f), old_b = make_float4(0.f);
                $if(layered != 0u) {
                    old_d = frame_buffer().denoiser_diffuse().read(address);
                    old_s = frame_buffer().denoiser_specular().read(address);
                    old_b = frame_buffer().denoiser_bypass().read(address);
                };
                auto merged = denoiser_signals::merge_restir_lobes(direct_->read_radiance(i).xyz(),
                    indirect_->read_radiance(i).xyz(), sd, sg, bypass, di, gi, old_d, old_s, old_b);
                frame_buffer().denoiser_diffuse().write(address, merged.diffuse);
                frame_buffer().denoiser_specular().write(address, merged.specular);
                frame_buffer().denoiser_bypass().write(address, merged.bypass);
            };
            prepare_denoiser_ = device().compile(pack_lobes, "Denoiser-MergeReSTIRLobes");
            Kernel compose_lobes = [&](Uint layered, Float di) {
                Uint i = dispatch_id(); Float3 total = make_float3(0.f);
                $if(layered != 0u) { total = frame_buffer().stable_radiance().read(i).xyz() * di; };
                $for(layer, select(layered != 0u, StablePlaneCount, 1u)) {
                    Uint j = layer * (dispatch_dim().x * dispatch_dim().y) + i;
                    total += frame_buffer().denoiser_diffuse().read(j).xyz() + frame_buffer().denoiser_specular().read(j).xyz() +
                             frame_buffer().denoiser_bypass().read(j).xyz();
                };
                frame_buffer().rt_buffer().write(i, make_float4(total, 1.f));
            };
            compose_denoiser_ = device().compile(compose_lobes, "Denoiser-ComposeReSTIR");
        }
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
        if (!direct_->compact_radiance()) ret.direct = direct_->radiance()->view();
        if (!indirect_->compact_radiance()) ret.indirect = indirect_->radiance()->view();
        if (stable_planes_enabled()) {
            ret.use_stable_planes = true;
            ret.layer_count = StablePlaneCount;
            ret.layer_index = layer;
            ret.stable_planes = frame_buffer().cur_stable_planes_view(frame_index_, layer);
            if (frame_buffer().stable_plane_history_retained() && !frame_buffer().compact_coverage_history())
                ret.prev_stable_planes = frame_buffer().prev_stable_planes_view(frame_index_, layer);
            if (frame_buffer().stable_raw_outputs_retained()) {
                ret.direct = frame_buffer().stable_direct_view(layer);
                ret.indirect = frame_buffer().stable_indirect_view(layer);
            }
        }
        // Camera positions for depth calculation from visibility buffer
        float3 cam_pos = camera->position();
        float3 prev_cam_pos = camera->prev_host_position();
        ret.camera_pos = {cam_pos.x, cam_pos.y, cam_pos.z};
        ret.prev_camera_pos = {prev_cam_pos.x, prev_cam_pos.y, prev_cam_pos.z};
        // ReSTIR path: direct_ = direct lighting, indirect_ = indirect lighting (NOT diffuse/specular).
        ret.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            ret.has_lobe_signals = true;
            auto &fb = frame_buffer();
            auto d = fb.denoiser_layer_view(fb.denoiser_diffuse(), layer);
            auto s = fb.denoiser_layer_view(fb.denoiser_specular(), layer);
            ret.lobe_signals = {d, s, d, s};
        }
        return ret;
    }

    void render() const noexcept override {
        const Pipeline *rp = pipeline();
        Stream &stream = rp->stream();
        reset_denoise_timing();
        const bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        const bool extra_aa = !denoiser_ || denoiser_coverage_aa_enabled(impl_type(), denoiser_->impl_type());
        const bool retain_history = denoiser_enabled && extra_aa;
        const bool precise_lobes = denoiser_enabled && denoiser_->requires_lobe_signals() && precise_lobes_requested();
        const bool raw_stable_outputs = raw_stable_outputs_required(precise_lobes);
        const bool compact = !denoiser_enabled || precise_lobes;
        if (direct_->compact_radiance() != compact || indirect_->compact_radiance() != compact) {
            // Finish the old representation before changing bindings captured by
            // the DI/GI producers and their raw/denoised composition kernels.
            stream << synchronize() << commit();
            direct_->set_compact_radiance(compact);
            indirect_->set_compact_radiance(compact);
            pipeline()->upload_bindless_array();
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            direct_->compile();
            indirect_->compile();
            const_cast<RealTimeIntegrator *>(this)->compile_composition();
            // Output has no temporal state. Keep the frame sequence and prior
            // raw stable planes intact for newly enabled denoiser history.
        }
        const bool compact_history = denoiser_ && denoiser_->uses_compact_coverage_history();
        const bool raw_outputs_changed = frame_buffer().stable_raw_outputs_retained() != raw_stable_outputs;
        if (stable_resources_dirty_ || frame_buffer().stable_plane_history_retained() != retain_history ||
            frame_buffer().compact_coverage_history() != compact_history ||
            raw_outputs_changed) {
            // Resize history before build snapshots the complete previous raw
            // planes. A history-only switch must preserve those current records.
            stream << synchronize() << commit();
            frame_buffer().manage_stable_plane_buffers(stable_planes_enabled(), retain_history, compact_history, raw_stable_outputs);
            pipeline()->upload_bindless_array();
            if (stable_resources_dirty_) {
                if (denoiser_) { denoiser_->set_stable_planes_enabled(stable_planes_enabled()); }
                stable_resources_dirty_ = false;
                pipeline()->invalidate();
            }
        }
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
        // GBuffer does not write extensions. Save the previous receiver before
        // either stable-plane build or DI overwrites the full current record.
        submit(frame_buffer().save_surface_exts_history(frame_index_), &cur_stage_profile_.gbuffer_ms);
        if (stable_planes_enabled()) {
            const bool copy_history = retain_history && denoiser_->needs_stable_plane_history(frame_index_);
            submit(stable_planes_->build(frame_index_, jitter_primary_samples(), copy_history, raw_stable_outputs), &cur_stage_profile_.stable_build_ms);
            submit(stable_planes_->fill(frame_index_, direct_->factor(), indirect_->factor(), raw_stable_outputs), &cur_stage_profile_.stable_fill_ms);
        }
        const char *falcor_mode = std::getenv("VISION_FALCOR_DI_MODE");
        const bool falcor_requested = falcor_mode && *falcor_mode && direct_->open() && !indirect_->open() &&
            !denoiser_enabled && !stable_planes_enabled() && !jitter_primary_samples() &&
            suffix_depth() == 0u && direct_->max_recursion() == 1u && spectrum()->impl_type() == "srgb" &&
            !pipeline()->geometry().process_mediums();
        if (falcor_requested) {
            if (!falcor_di_ || !falcor_di_->matches(rp->resolution())) {
                stream << synchronize() << commit();
                Global::SceneGpuContextScope scene_gpu_context{
                    scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
                try {
                    falcor_di_ = std::make_unique<FalcorDIExperiment>(*direct_, falcor_mode);
                } catch (const std::exception &error) {
                    OC_ERROR_FORMAT("Falcor DI experiment: {}", error.what());
                }
            }
            submit(falcor_di_->dispatch(frame_index_), &cur_stage_profile_.restir_di_ms);
            if (indirect_->compact_radiance()) {
                submit(rp->reset_buffer(indirect_->raw_radiance().view(), make_uint2(0u), "ReSTIR indirect clear"),
                       &cur_stage_profile_.restir_gi_ms);
            } else {
                submit(rp->reset_buffer(indirect_->radiance()->view(), make_float4(0.f), "ReSTIR indirect clear"),
                       &cur_stage_profile_.restir_gi_ms);
            }
        } else {
            submit(direct_->dispatch(frame_index_, precise_lobes), &cur_stage_profile_.restir_di_ms);
            submit(indirect_->dispatch(frame_index_, precise_lobes, stable_planes_enabled(),
                                      direct_->factor(), indirect_->factor(), direct_->open()), &cur_stage_profile_.restir_gi_ms);
        }
        cur_stage_profile_.path_tracing_ms = cur_stage_profile_.restir_di_ms + cur_stage_profile_.restir_gi_ms;
        auto debug_readback = [&](const char *stage, bool layers = true) {
            const auto directory = evaluation_debug::directory(frame_index_);
            if (directory.empty()) return;
            fs::create_directories(directory);
            vector<float4> direct, indirect;
            if (precise_lobes && string_view(stage).starts_with("composite_")) {
                direct.resize(pipeline()->pixel_num());
                indirect.assign(direct.size(), make_float4(0.f));
                stream << frame_buffer().rt_buffer().view().download(direct.data()) << synchronize() << commit();
            } else {
                direct = direct_->download_radiance();
                indirect = indirect_->download_radiance();
            }
            auto write = [&](const char *channel, const auto &pixels) {
                std::ofstream out(fs::path(directory) / (std::string(stage) + channel + ".f32"), std::ios::binary);
                out.write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * sizeof(float4));
                if (!out) throw std::runtime_error("ReSTIR diagnostic write failed");
            };
            write("_direct", direct); write("_indirect", indirect);
            if (layers && stable_planes_enabled()) {
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
        if (std::getenv("VISION_EVAL_GI_STORAGE_DEBUG") != nullptr) {
            const auto directory = evaluation_debug::directory(frame_index_);
            if (!directory.empty()) {
                debug_readback("producer", false);
                vector<GISample> samples;
                vector<GIReservoir> reservoirs;
                indirect_->download_storage(samples, reservoirs);
                auto write_storage = [&](const char *name, const auto &values) {
                    std::ofstream out(directory / name, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(values[0]));
                    if (!out) throw std::runtime_error("GI storage diagnostic write failed");
                };
                write_storage("gi_samples.bin", samples);
                write_storage("gi_reservoirs.bin", reservoirs);
            }
        }
        if (stable_planes_enabled() && raw_stable_outputs) {
            submit(merge_dominant_().dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
        }
        // Opt-in raw layer diagnostics belong here, after merging ownership and
        // before any per-layer denoising. No permanent copy buffers are needed.
        debug_readback("raw");
        const bool denoiser = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled() && denoiser_->requires_lobe_signals();
        // Keep the explicit raw lobe contract observable with denoising off,
        // just as PT does. This path exists only when Denoiser was selected.
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            const float2 factors = make_float2(direct_->factor(), indirect_->factor());
            if (denoiser && any(factors != denoiser_factors_)) {
                denoiser_->set_enabled(false); denoiser_->set_enabled(true); denoiser_factors_ = factors;
            }
            if (!precise_lobes)
                submit(prepare_denoiser_(uint(stable_planes_enabled()), factors.x, factors.y).dispatch(pipeline()->resolution()),
                       &cur_stage_profile_.combine_ms);
        }
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            begin_denoise_timing(stream);
            const uint count = stable_planes_enabled() ? StablePlaneCount : 1u;
            bool composed_lobes = false;
            for (uint layer = 0u; layer < count; ++layer) {
                auto dn_input = denoise_input(layer);
                dn_input.stable_radiance_factor = direct_->factor();
                const char *batch_compose = std::getenv("REBLUR_BATCH_COMPOSE");
                dn_input.compose_only = denoiser_->impl_type() == "ReBLUR" &&
                    (!batch_compose || batch_compose[0] != '0') && !raw_stable_outputs_required(precise_lobes);
                submit(denoiser_->dispatch(dn_input), &cur_stage_profile_.spatial_angular_ms);
                composed_lobes |= dn_input.composed_lobe_output;
            }
            debug_readback("filtered");
            if (stable_planes_enabled() || denoiser) {
                if (denoiser && !composed_lobes) submit(compose_denoiser_(uint(stable_planes_enabled()), direct_->factor()).dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
                else if (!denoiser) submit(compose_layers_().dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
                debug_readback("composite_pre", false);
                if (extra_aa) {
                    auto coverage = denoise_input();
                    coverage.layer_count = 1u;
                    coverage.composed_coverage = true;
                    coverage.total_radiance_coverage = denoiser;
                    if (denoiser) {
                        coverage.direct = frame_buffer().rt_buffer().view();
                        coverage.indirect = coverage.direct; // Total coverage omits this channel.
                    } else {
                        coverage.direct = direct_->radiance()->view();
                        coverage.indirect = indirect_->radiance()->view();
                    }
                    if (stable_planes_enabled()) {
                        coverage.coverage_planes = frame_buffer().cur_stable_planes_view(frame_index_);
                        if (frame_buffer().compact_coverage_history())
                            coverage.prev_coverage_history = frame_buffer().coverage_history().view();
                        else
                            coverage.prev_coverage_planes = frame_buffer().prev_stable_planes_view(frame_index_);
                    }
                    submit(denoiser_->dispatch(coverage), &cur_stage_profile_.spatial_angular_ms);
                }
                debug_readback("composite_post", false);
            }
            end_denoise_timing(stream);
        }
        const bool resolved = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        // Raw ownership is unchanged. Filtered output resolves that same sum
        // before channel factors are applied exactly once in presentation.
        submit(combine_(frame_index_, denoiser ? 1.f : direct_->factor(), denoiser ? 1.f : indirect_->factor(), uint(stable_planes_enabled() && !resolved), uint(denoiser))
                   .dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
        submit(frame_buffer().post_path_tracing(frame_index_), &cur_stage_profile_.postprocess_ms);
        submit(frame_buffer().render_final(frame_index_), &cur_stage_profile_.render_final_ms);
        increase_frame_index();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, RealTimeIntegrator)
