//
// Created by Zero on 2023/9/11.
//

#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/evaluation_debug.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "restir/di/di_settings.h"
#include "restir/di/di_radiance.h"
#include "restir/gi/gi_receiver.h"
#include "restir/gi/restir_gi.h"
#include "restir/gi/restir_gi_lights.h"
#include "restir/gi/restir_gi_denoiser_diagnostic.h"
#include "restir/gi/restir_gi_denoiser_prepare.h"
#include "restir/gi/restir_gi_shadow_diagnostic.h"
#include "restir/gi/restir_gi_primary_light_diagnostic.h"
#include "restir/denoiser_signals.h"
#include "restir/di/reblur_settings.h"
#include "restir/di/restir_di.h"
#include "restir/gi/reference/restir_gi_experiment.h"
#include <cstdlib>
#include <fstream>
#include <map>

namespace vision {
struct NativeReblurComposeParam {
    BufferDesc<float4> di_diffuse, di_specular, di_bypass, gi_bypass, output;
    BufferDesc<float4> gi_diffuse0, gi_specular0, gi_diffuse1, gi_specular1, gi_diffuse2, gi_specular2;
    float direct_factor{1.f}, indirect_factor{1.f};
};
struct ReSTIRDILobeStageParam {
    BufferDesc<float4> diffuse, specular, bypass;
    float factor{1.f};
};
}
OC_PARAM_STRUCT(vision, NativeReblurComposeParam, di_diffuse, di_specular, di_bypass,
                gi_bypass, output, gi_diffuse0, gi_specular0, gi_diffuse1, gi_specular1,
                gi_diffuse2, gi_specular2, direct_factor, indirect_factor) {};
OC_PARAM_STRUCT(vision, ReSTIRDILobeStageParam, diffuse, specular, bypass, factor) {};

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

[[nodiscard]] bool raw_lobes_requested(bool precise_lobes) noexcept {
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

    SP<DISettings> di_settings_;
    SP<DIRadiance> di_radiance_;
    SP<ReSTIRGI> indirect_;
    DataWrap scene_restir_settings_;
    Shader<void(uint, float, float, uint)> combine_;
    Shader<void(float, float)> prepare_denoiser_;
    Shader<void(ReSTIRDILobeStageParam)> stage_restir_di_lobes_;
    Shader<void()> compose_denoiser_;
    mutable float2 denoiser_factors_{make_float2(-1.f)};
    mutable std::unique_ptr<ReSTIRDI> restir_di_;
    mutable std::unique_ptr<GIReceiver> gi_receiver_;
    mutable std::string restir_di_error_;
    mutable bool restir_di_needs_compile_{true};
    mutable bool restir_di_was_active_{};
    mutable std::unique_ptr<RestirGIExperiment> reference_gi_;
    mutable bool reference_gi_needs_compile_{true};
    mutable bool reference_gi_was_active_{};
    mutable bool reference_gi_rejected_{};
    mutable HotfixSlot<SP<Denoiser>> native_di_denoiser_{};
    mutable uint2 native_denoiser_size_{};
    Shader<void(NativeReblurComposeParam)> native_reblur_compose_;

    [[nodiscard]] const char *restir_di_mode() const noexcept {
        const char *mode = std::getenv("VISION_RESTIR_DI_MODE");
        if (mode && *mode) return mode;
        return di_settings_->pairwise() ? "pairwise" : "basic";
    }
    [[nodiscard]] bool native_reblur_requested() const noexcept {
        return indirect_->open() && di_settings_->open() &&
            !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled() &&
            denoiser_->impl_type() == "ReBLUR";
    }

    void prepare_native_di_denoiser(const string &settings) const noexcept {
        // Runtime compilation needs the scene's material LUT bindings.
        Global::SceneGpuContextScope scene_gpu_context{
            scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
        if (!native_di_denoiser_) {
            DenoiserDesc desc{"ReBLUR"};
            desc.set_value("enabled", true);
            native_di_denoiser_ = Node::create_shared<Denoiser>(desc);
            native_di_denoiser_->set_enabled(true);
            native_di_denoiser_->prepare();
            native_di_denoiser_->compile();
            native_denoiser_size_ = pipeline()->resolution();
            OC_INFO("ReSTIR GI + ReSTIR DI + REBLUR active: private_planes=3, physical_DI_contexts=1, GI_contexts=3, shared_transport=false");
        } else if (any(native_denoiser_size_ != pipeline()->resolution())) {
            native_di_denoiser_->update_resolution(pipeline()->resolution());
            native_denoiser_size_ = pipeline()->resolution();
        }
        native_di_denoiser_->set_enabled(true);
        native_di_denoiser_->set_reblur_settings(settings);
    }

public:
    [[nodiscard]] bool allows_path_tracing_fallback() const noexcept override {
        return di_settings_->open() && indirect_->open();
    }

    [[nodiscard]] const char *render_incompatibility() const noexcept override {
        if (indirect_->open()) {
            if (const char *reason = indirect_->backend_incompatibility()) return reason;
        }
        if (!di_settings_->open()) return nullptr;
        if (di_settings_->max_recursion() != 1u) return "ReSTIR DI requires direct.max_recursion = 1";
        if (spectrum()->impl_type() != "srgb" || frame_buffer().impl_type() != "normal")
            return "ReSTIR DI requires an RGB spectrum and the normal framebuffer";
        if (jitter_primary_samples()) return "ReSTIR DI requires centered primary samples";
        if (!scene().sensor()->supports_screen_position_reconstruction())
            return "ReSTIR DI requires a perspective camera with zero aperture";
        if (pipeline()->geometry().process_mediums()) return "ReSTIR DI does not support participating media";
        if (indirect_->open() && suffix_depth() == 0u) return "ReSTIR GI requires max_depth above 1";
        if (indirect_->open() && !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled() &&
            !native_reblur_requested()) return "Combined ReSTIR DI and GI require ReBLUR with ReSTIR GI, or denoising disabled";
        if (!restir_di_needs_compile_ && !restir_di_error_.empty()) return restir_di_error_.c_str();
        return nullptr;
    }

    [[nodiscard]] DataWrap reblur_context_settings() const override {
        if (!native_reblur_requested()) return DataWrap::object();
        const auto parent = DataWrap::parse(denoiser_->reblur_settings_json());
        const auto target = physical_di_reblur_settings(parent);
        const auto physical = native_di_denoiser_
            ? DataWrap::parse(native_di_denoiser_->reblur_settings_json()) : DataWrap::object();
        return {{"policy", "physical-di-detail-preserving-v1"},
                {"physical_di", {{"count", 1u}, {"ready", physical == target}, {"settings", physical}}},
                {"gi", {{"count", 3u}, {"settings", parent}}}};
    }

    RealTimeIntegrator() = default;
    explicit RealTimeIntegrator(const IntegratorDesc &desc)
        : IlluminationIntegrator(desc) {
        OC_ERROR_IF(max_depth_.hv() == 0u, "ReSTIR max_depth must be at least 1.");
        max_depth_ = max_depth_.hv() - 1u;
    }

    void initialize_(const vision::NodeDesc &node_desc) noexcept override {
        const Desc &desc = static_cast<const Desc &>(node_desc);
        di_settings_ = make_shared<DISettings>(shared_from_this(), desc["direct"]);
        di_radiance_ = make_shared<DIRadiance>();
        indirect_ = make_shared<ReSTIRGI>(shared_from_this(), desc["indirect"]);
        // The ReSTIR GI candidate path evaluates the full BSDF inline.
        separate_ = false;
        OC_INFO("ReSTIR GI candidate material layout: inline full BSDF (separate=false)");
        scene_restir_settings_ = settings();
    }

    VS_MAKE_GUI_STATUS_FUNC(IlluminationIntegrator, di_settings_, indirect_)

    void restore(vision::RuntimeObject *old_obj) noexcept override {
        const auto *previous = hotfix::hotfix_detail::restore_source(*this, old_obj);
        if (!previous) return;
        const RuntimeObject *previous_gi = previous->indirect_.get();
        OC_ERROR_IF(previous_gi && string_view{previous_gi->class_name()} != hotfix::type_string<ReSTIRGI>(),
                    "GI runtime object layout changed; restart the engine before reloading the integrator.");
        IlluminationIntegrator::restore(old_obj);
        VS_HOTFIX_MOVE_ATTRS(di_settings_, di_radiance_, indirect_, scene_restir_settings_,
                             combine_, prepare_denoiser_, stage_restir_di_lobes_, compose_denoiser_, denoiser_factors_, denoiser_,
                             native_di_denoiser_, native_denoiser_size_, native_reblur_compose_)
        di_settings_->set_integrator(shared_from_this());
        indirect_->set_integrator(shared_from_this());
    }

    void update_resolution(ocarina::uint2 res) noexcept override {
        gi_receiver_.reset();
        restir_di_needs_compile_ = true;
        reference_gi_needs_compile_ = true;
        di_radiance_->update_resolution(res);
        indirect_->update_resolution(res);
        if (!denoiser_runtime_disabled() && denoiser_ &&
            (denoiser_->enabled() || denoiser_->has_prepared_resources())) {
            denoiser_->update_resolution(res);
        }
    }

    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override {
        std::tuple tp = {addressof(indirect_)};
        HotfixSystem::replace_objects(constructor, tp);
        restir_di_needs_compile_ = true;
        gi_receiver_.reset();
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
        return frame_buffer().enable_accumulation() || (denoiser_enabled && (!di_settings_ || !di_settings_->open()));
    }
    void prepare() noexcept override {
        switch_profile::Scope profile{"integrator.prepare", "buffers"};
        IlluminationIntegrator::prepare();
        const bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        const bool precise_lobes = denoiser_enabled && denoiser_->requires_lobe_signals() && precise_lobes_requested();
        const bool compact = !denoiser_enabled || precise_lobes;
        di_radiance_->prepare(compact);
        indirect_->set_native_reblur_enabled(native_reblur_requested());
        indirect_->prepare(compact);
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            denoiser_->prepare();
        }
        frame_buffer().prepare_visibility_buffer();
        frame_buffer().prepare_motion_vectors();
        if (denoiser_ && denoiser_->requires_lobe_signals()) frame_buffer().prepare_denoiser_signals();
    }

    [[nodiscard]] bool stable_planes_enabled() const noexcept override {
        return di_settings_->stable_planes_enabled();
    }

    void set_stable_planes_enabled(bool enabled) noexcept override {
        if (stable_planes_enabled() == enabled) { return; }
        di_settings_->set_stable_planes_enabled(enabled);
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
        di_settings_->render_UI(widgets);
        indirect_->render_UI(widgets);
    }

    [[nodiscard]] DataWrap settings() const override {
        auto result = illumination_settings();
        result["max_depth"] = max_depth_.hv() + 1u;
        result.update(DataWrap{{"direct", di_settings_->settings()}, {"indirect", indirect_->settings()}});
        return result;
    }

    bool set_settings(const DataWrap &overrides) noexcept override {
        auto candidate = scene_restir_settings_;
        if (!merge_restir_settings(candidate, overrides) || candidate == settings()) return false;
        // A camera's view and scene GPU context must be active before applying.
        // Finish readers before replacing a shader captured by an earlier frame.
        pipeline()->stream() << synchronize() << commit();
        const bool was_direct_enabled = di_settings_->open();
        const bool was_indirect_enabled = indirect_->open();
        di_settings_->set_settings(candidate["direct"]);
        indirect_->set_settings(candidate["indirect"]);
        restir_di_needs_compile_ = true;
        reference_gi_needs_compile_ = true;
        auto transport = candidate;
        transport["max_depth"] = candidate["max_depth"].get<uint>() - 1u;
        apply_illumination_settings(transport);
        if (was_indirect_enabled != indirect_->open()) {
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            indirect_->compile();
        }
        if (was_direct_enabled != di_settings_->open() || was_indirect_enabled != indirect_->open()) gi_receiver_.reset();
        return true;
    }

    void compile() noexcept override {
        switch_profile::Scope profile{"integrator.compile", "compile"};
        const char *native_lambert = std::getenv("VISION_RESTIR_GI_NATIVE_ALIGNED_LAMBERT");
        if (native_lambert && native_lambert[0] != '\0' && native_lambert[0] != '0') {
            OC_ASSERT(!separate());
            OC_INFO_FORMAT("Native ReSTIR GI material contract: separate={}, suffix_depth={}", separate(), suffix_depth());
        }
        restir_di_needs_compile_ = true;
        reference_gi_needs_compile_ = true;
        gi_receiver_.reset();
        indirect_->compile(true);
        if (!denoiser_runtime_disabled() && denoiser_ &&
            (denoiser_->enabled() || denoiser_->has_prepared_resources())) {
            denoiser_->compile();
        }
        compile_composition();
    }

    void compile_composition() noexcept {
        TSensor &camera = scene().sensor();
        Kernel kernel = [&](Uint frame_index, Float di, Float ii, Uint resolved_lobes) {
            camera->load_data();
            Float3 L;
            $if(resolved_lobes != 0u) {
                L = frame_buffer().rt_buffer().read(dispatch_id()).xyz();
            } $else {
                Float3 direct = di_radiance_->read_radiance(dispatch_id()).xyz();
                Float3 indirect = indirect_->read_radiance(dispatch_id()).xyz();
                L = direct * di + indirect * ii;
            };
            frame_buffer().add_sample(dispatch_idx().xy(), L, frame_index);
        };
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            Kernel pack_lobes = [&](Float di, Float gi) {
                Uint i = dispatch_id();
                Float4 sd = frame_buffer().denoiser_di_specular().read(i);
                Float4 sg = frame_buffer().denoiser_gi_specular().read(i);
                Float3 direct = di_radiance_->read_radiance(i).xyz();
                Float3 bypass = frame_buffer().denoiser_di_bypass().read(i).xyz();
                Float4 old_d = make_float4(0.f), old_s = make_float4(0.f), old_b = make_float4(0.f);
                auto merged = denoiser_signals::merge_restir_lobes(direct,
                    indirect_->read_radiance(i).xyz(), sd, sg, bypass, di, gi, old_d, old_s, old_b);
                frame_buffer().denoiser_diffuse().write(i, merged.diffuse);
                frame_buffer().denoiser_specular().write(i, merged.specular);
                frame_buffer().denoiser_bypass().write(i, merged.bypass);
            };
            prepare_denoiser_ = device().compile(pack_lobes, "Denoiser-MergeReSTIRLobes");
            Kernel stage_di_lobes = [&](Var<ReSTIRDILobeStageParam> p) {
                Uint i = dispatch_id();
                Float4 diffuse = p.diffuse.read(i);
                Float4 specular = p.specular.read(i);
                frame_buffer().denoiser_diffuse().write(i, make_float4(diffuse.xyz() * p.factor, diffuse.w));
                frame_buffer().denoiser_specular().write(i, make_float4(specular.xyz() * p.factor, specular.w));
                frame_buffer().denoiser_bypass().write(i, p.bypass.read(i) * p.factor);
            };
            stage_restir_di_lobes_ = device().compile(stage_di_lobes, "REBLUR-StageReSTIRDI");
            Kernel compose_lobes = [&] {
                Uint i = dispatch_id();
                Float3 total = frame_buffer().denoiser_diffuse().read(i).xyz() +
                               frame_buffer().denoiser_specular().read(i).xyz() +
                               frame_buffer().denoiser_bypass().read(i).xyz();
                frame_buffer().rt_buffer().write(i, make_float4(total, 1.f));
            };
            compose_denoiser_ = device().compile(compose_lobes, "Denoiser-ComposeReSTIR");
        }
        if (denoiser_ && denoiser_->impl_type() == "ReBLUR") {
            Kernel compose_native = [](Var<NativeReblurComposeParam> p) {
                Uint i = dispatch_id();
                Float3 direct = p.di_diffuse.read(i).xyz() + p.di_specular.read(i).xyz() + p.di_bypass.read(i).xyz();
                Float3 indirect = p.gi_bypass.read(i).xyz();
                indirect += p.gi_diffuse0.read(i).xyz(); indirect += p.gi_specular0.read(i).xyz();
                indirect += p.gi_diffuse1.read(i).xyz(); indirect += p.gi_specular1.read(i).xyz();
                indirect += p.gi_diffuse2.read(i).xyz(); indirect += p.gi_specular2.read(i).xyz();
                p.output.write(i, make_float4(direct * p.direct_factor + indirect * p.indirect_factor, 1.f));
            };
            native_reblur_compose_ = device().compile(compose_native, "REBLUR-ComposePhysicalDI-PrivateGI");
        }
        {
            switch_profile::Scope combine_profile{"combine.compile", "compile"};
            combine_ = device().compile(kernel, "combine");
        }
    }

    RealTimeDenoiseInput denoise_input() const noexcept {
        RealTimeDenoiseInput ret;
        TSensor &camera = scene().sensor();
        ret.frame_index = frame_index_;
        ret.resolution = pipeline()->resolution();
        ret.visibility = frame_buffer().cur_visibility_buffer_view(frame_index_);
        ret.prev_visibility = frame_buffer().prev_visibility_buffer_view(frame_index_);
        ret.motion_vec = frame_buffer().motion_vectors();
        if (!di_radiance_->compact_radiance()) ret.direct = di_radiance_->radiance()->view();
        if (!indirect_->compact_radiance()) ret.indirect = indirect_->radiance()->view();
        // Camera positions for depth calculation from visibility buffer
        float3 cam_pos = camera->position();
        float3 prev_cam_pos = camera->prev_host_position();
        ret.camera_pos = {cam_pos.x, cam_pos.y, cam_pos.z};
        ret.prev_camera_pos = {prev_cam_pos.x, prev_cam_pos.y, prev_cam_pos.z};
        // This denoiser input uses direct and indirect lighting channels.
        ret.channel_kind = RealTimeDenoiseInput::ChannelKind::DirectIndirect;
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            ret.has_lobe_signals = true;
            auto &fb = frame_buffer();
            auto d = fb.denoiser_layer_view(fb.denoiser_diffuse(), 0u);
            auto s = fb.denoiser_layer_view(fb.denoiser_specular(), 0u);
            ret.lobe_signals = {d, s, d, s};
        }
        return ret;
    }

    void render() const noexcept override {
        if (const char *reason = render_incompatibility()) {
            OC_WARNING_FORMAT("ReSTIR unavailable: {}", reason);
            return;
        }
        const Pipeline *rp = pipeline();
        Stream &stream = rp->stream();
        reset_denoise_timing();
        const bool denoiser_enabled = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled();
        const bool extra_aa = !denoiser_ || denoiser_coverage_aa_enabled(impl_type(), denoiser_->impl_type());
        const bool precise_lobes = denoiser_enabled && denoiser_->requires_lobe_signals() && precise_lobes_requested();
        const bool native_denoising = native_reblur_requested();
        if (indirect_->native_reblur_enabled() != native_denoising) {
            stream << synchronize() << commit();
            Global::SceneGpuContextScope context{scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            indirect_->set_native_reblur_enabled(native_denoising);
            restir_di_needs_compile_ = true;
        }
        if (!native_denoising && native_di_denoiser_) native_di_denoiser_->set_enabled(false);
        const bool compact = !denoiser_enabled || precise_lobes;
        if (di_radiance_->compact_radiance() != compact || indirect_->compact_radiance() != compact) {
            // Finish the old representation before changing bindings captured by
            // the DI/GI producers and their raw/denoised composition kernels.
            stream << synchronize() << commit();
            di_radiance_->set_compact_radiance(compact);
            indirect_->set_compact_radiance(compact);
            restir_di_needs_compile_ = true;
            reference_gi_needs_compile_ = true;
            pipeline()->upload_bindless_array();
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            indirect_->compile();
            const_cast<RealTimeIntegrator *>(this)->compile_composition();
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
        const char *di_mode = restir_di_mode();
        const bool gi_enabled = indirect_->open();
        const char *reference_gi_mode = std::getenv("VISION_RESTIR_GI_MODE");
        const char *multibounce_value = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        const bool multibounce_reference = multibounce_value && multibounce_value[0] == '1';
        const bool reference_depth_supported = suffix_depth() == 1u ||
            (multibounce_reference && suffix_depth() == 15u && min_depth_.hv() == 5u && rr_threshold_.hv() == 1.f);
        const bool reference_gi_requested = reference_gi_mode && *reference_gi_mode && gi_enabled &&
            di_settings_->open() && reference_depth_supported && di_settings_->max_recursion() == 1u &&
            !denoiser_enabled && !jitter_primary_samples() &&
            spectrum()->impl_type() == "srgb" && frame_buffer().impl_type() == "normal" &&
            scene().sensor()->supports_screen_position_reconstruction() &&
            !pipeline()->geometry().process_mediums();
        const bool publish_gi_proposal = native_denoising && restir_gi::primary_light_diagnostic_enabled();
        const bool di_lobe_signals = native_denoising ||
            (!gi_enabled && denoiser_ && denoiser_->requires_lobe_signals());
        const bool di_requested = di_settings_->open();
        const bool prepare_di = di_requested && (restir_di_needs_compile_ ||
            (restir_di_ && !restir_di_->matches(rp->resolution(), di_mode, gi_enabled, publish_gi_proposal, di_lobe_signals)));
        if (prepare_di) {
            stream << synchronize() << commit();
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            restir_di_needs_compile_ = false;
            restir_di_.reset();
            restir_di_error_.clear();
            try {
                if (publish_gi_proposal && frame_buffer().hit_bsdfs_size() == 0u) {
                    frame_buffer().prepare_hit_bsdfs();
                    pipeline()->upload_bindless_array();
                }
                restir_di_ = std::make_unique<ReSTIRDI>(*di_settings_, *di_radiance_, di_mode,
                                                        gi_enabled, publish_gi_proposal, di_lobe_signals);
            } catch (const std::exception &error) {
                restir_di_error_ = error.what();
                OC_WARNING_FORMAT("ReSTIR DI unavailable: {}", error.what());
                return;
            }
        }
        const bool use_di = di_requested && restir_di_;
        const bool receiver_needed = gi_enabled && !use_di;
        const bool prepare_receiver = receiver_needed && !gi_receiver_;
        if (prepare_receiver) {
            stream << synchronize() << commit();
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            gi_receiver_ = std::make_unique<GIReceiver>();
            gi_receiver_->compile();
        }
        const auto primary_flags = use_di ? restir_di_->primary_flags() :
            receiver_needed ? gi_receiver_->primary_flags() : BufferView<uint>{};
        if (prepare_receiver || prepare_di || use_di != restir_di_was_active_ || frame_index_ == 0u) {
            OC_INFO_FORMAT("ReSTIR surface storage: full_bytes=0, primary_flag_bytes={}", primary_flags.size_in_byte());
            indirect_->invalidate_history();
        }
        restir_di_was_active_ = use_di;
        const bool reference_gi_rejected = reference_gi_mode && *reference_gi_mode && !reference_gi_requested;
        if (reference_gi_rejected && !reference_gi_rejected_) {
            OC_WARNING("Reference GI inactive: requires DI+GI, max_depth=2 (or explicit multibounce benchmark5/16/1), max_recursion=1, zero-aperture perspective camera, RGB normal framebuffer, "
                       "no denoiser, primary jitter or media; using ReSTIR GI.");
        }
        reference_gi_rejected_ = reference_gi_rejected;
        const bool prepare_reference_gi = reference_gi_requested && (reference_gi_needs_compile_ ||
            (reference_gi_ && !reference_gi_->matches(rp->resolution(), reference_gi_mode)));
        if (prepare_reference_gi) {
            stream << synchronize() << commit();
            Global::SceneGpuContextScope scene_gpu_context{
                scene().geometry().bindless_array(), scene().geometry().gpu_resource()->device()};
            reference_gi_needs_compile_ = false;
            reference_gi_.reset();
            try {
                reference_gi_ = std::make_unique<RestirGIExperiment>(*indirect_, reference_gi_mode);
            } catch (const std::exception &error) {
                OC_WARNING_FORMAT("Reference GI unavailable, using ReSTIR GI: {}", error.what());
            }
        }
        const bool use_reference_gi = reference_gi_requested && reference_gi_;
        if (prepare_reference_gi || use_reference_gi != reference_gi_was_active_) indirect_->invalidate_history();
        reference_gi_was_active_ = use_reference_gi;
        if (use_di) {
            submit(restir_di_->dispatch(frame_index_), &cur_stage_profile_.restir_di_ms);
        } else {
            if (di_radiance_->compact_radiance()) {
                submit(rp->reset_buffer(di_radiance_->raw_radiance().view(), make_uint2(0u), "DI radiance clear"),
                       &cur_stage_profile_.restir_di_ms);
            } else {
                submit(rp->reset_buffer(di_radiance_->radiance()->view(), make_float4(0.f), "DI radiance clear"),
                       &cur_stage_profile_.restir_di_ms);
            }
            if (denoiser_ && denoiser_->requires_lobe_signals()) {
                submit(rp->reset_buffer(frame_buffer().denoiser_di_specular().view(), make_float4(0.f), "DI specular clear"),
                       &cur_stage_profile_.restir_di_ms);
                submit(rp->reset_buffer(frame_buffer().denoiser_di_bypass().view(), make_float4(0.f), "DI bypass clear"),
                       &cur_stage_profile_.restir_di_ms);
            }
        }
        if (!gi_enabled) {
            indirect_->invalidate_history();
            if (indirect_->compact_radiance()) {
                submit(rp->reset_buffer(indirect_->raw_radiance().view(), make_uint2(0u), "ReSTIR indirect clear"),
                       &cur_stage_profile_.restir_gi_ms);
            } else {
                submit(rp->reset_buffer(indirect_->radiance()->view(), make_float4(0.f), "ReSTIR indirect clear"),
                       &cur_stage_profile_.restir_gi_ms);
            }
            if (!precise_lobes && denoiser_ && denoiser_->requires_lobe_signals()) {
                submit(rp->reset_buffer(frame_buffer().denoiser_gi_specular().view(), make_float4(0.f),
                                       "ReSTIR indirect specular clear"), &cur_stage_profile_.restir_gi_ms);
            }
        } else {
            if (receiver_needed) {
                submit(gi_receiver_->dispatch(frame_index_), &cur_stage_profile_.restir_gi_ms);
            }
            submit(use_reference_gi ? reference_gi_->dispatch(frame_index_) :
                   indirect_->dispatch(frame_index_, precise_lobes, primary_flags,
                                       di_settings_->factor(), indirect_->factor(), di_settings_->open()),
                   &cur_stage_profile_.restir_gi_ms);
        }
        cur_stage_profile_.path_tracing_ms = cur_stage_profile_.restir_di_ms + cur_stage_profile_.restir_gi_ms;
        auto debug_readback = [&](const char *stage) {
            const auto directory = evaluation_debug::directory(frame_index_);
            if (directory.empty()) return;
            fs::create_directories(directory);
            if (native_denoising && use_di && !string_view(stage).starts_with("composite_")) {
                restir_gi::NativeReblurDiagnosticViews views;
                views.di_diffuse = restir_di_->denoiser_diffuse();
                views.di_specular = restir_di_->denoiser_specular();
                views.di_bypass = restir_di_->denoiser_bypass();
                views.gi_stable = indirect_->native_reblur_bypass();
                if (string_view(stage) == "producer" && std::getenv("VISION_GI_GUIDE_REUSE_FULL_READBACK")) {
                    auto prefix_view = indirect_->native_reblur_prefix().view_as<ReSTIRGIPrefixPlane>();
                    auto header_view = indirect_->native_reblur_headers();
                    vector<ReSTIRGIPrefixPlane> prefix(prefix_view.size());
                    vector<uint4> headers(header_view.size());
                    stream << prefix_view.download(prefix.data()) << header_view.download(headers.data())
                           << synchronize() << commit();
                    auto write = [&](const char *name, const auto &values) {
                        std::ofstream file(directory / name, std::ios::binary);
                        file.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(values[0]));
                        if (!file) throw std::runtime_error("GI prefix diagnostic write failed");
                    };
                    write("pt_prefix_records.bin", prefix); write("pt_prefix_headers.bin", headers);
                }
                // Readback-only materialization; normal rendering has no Guide array.
                auto guides = device().create_buffer<reblur::Guide>(size_t(pipeline()->pixel_num()) * StablePlaneCount,
                    "ReSTIR GI diagnostic-only decoded guides");
                auto prefixes = indirect_->native_reblur_prefix().view_as<ReSTIRGIPrefixPlane>();
                auto headers = indirect_->native_reblur_headers();
                auto motion_y = indirect_->native_reblur_motion_y();
                Kernel decode = [&](BufferVar<ReSTIRGIPrefixPlane> prefix_records, BufferVar<uint4> prefix_headers,
                                    BufferVar<float> prefix_motion_y, BufferVar<reblur::Guide> output, Uint2 resolution) {
                    Uint2 pixel = dispatch_idx().xy();
                    $if(any(pixel >= resolution)) { $return(); };
                    for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                        auto guide = restir_gi::load_pt_reblur_guide(prefix_records, prefix_headers, prefix_motion_y, pixel, resolution, Uint{layer},
                            [&](const Uint &instance) { return pipeline()->geometry().data()->instances().read(instance).mat_id; });
                        output.write(layer * resolution.x * resolution.y + pixel.y * resolution.x + pixel.x, guide);
                    }
                };
                auto decode_shader = device().compile(decode, "ReSTIR GI diagnostic-only Guide decode");
                stream << decode_shader(prefixes, headers, motion_y, guides.view(), rp->resolution()).dispatch(rp->resolution());
                for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                    views.gi_diffuse[layer] = indirect_->native_reblur_diffuse(layer);
                    views.gi_specular[layer] = indirect_->native_reblur_specular(layer);
                    views.gi_guides[layer] = guides.view(size_t(layer) * pipeline()->pixel_num(), pipeline()->pixel_num()).view_as<std::byte>();
                }
                restir_gi::download_native_reblur_diagnostics(device(), stream, views, rp->resolution(), directory, stage);
                if (string_view(stage) == "raw") {
                    restir_gi::download_shadow_diagnostics(device(), stream, frame_buffer(), frame_index_,
                                                         rp->resolution(), *di_radiance_, directory);
                    restir_gi::download_environment_diagnostics(device(), stream, frame_buffer(), frame_index_,
                                                              rp->resolution(), directory);
                    restir_gi::download_primary_light_diagnostics(device(), stream, frame_buffer(), frame_index_,
                                                                rp->resolution(), *di_radiance_, directory);
                }
            }
            if (string_view(stage) == "raw" && gi_enabled && !use_reference_gi) {
                indirect_->restir_gi_lights().download_validation(directory);
            }
            if(use_reference_gi&&string_view(stage)=="raw") reference_gi_->download_guides(frame_index_,directory);
            const char *native_diagnostic = std::getenv("VISION_RESTIR_GI_NATIVE_MB_DIAGNOSTIC");
            if (!use_reference_gi && string_view(stage) == "raw" && native_diagnostic && native_diagnostic[0] == '1') {
                auto positions = device().create_buffer<float4>(pipeline()->pixel_num());
                auto normals = device().create_buffer<float4>(pipeline()->pixel_num());
                auto &camera = scene().sensor();
                Kernel guides = [&](Uint frame) {
                    camera->load_data();
                    Uint i = dispatch_id();
                    Float4 p = make_float4(0.f), n = make_float4(0.f);
                    auto hit = frame_buffer().cur_visibility_buffer_var(frame).read(i);
                    $if(hit->is_hit()) {
                        auto ray = frame_buffer().rays().read(i)->to_ray_state().ray;
                        auto it = pipeline()->geometry().compute_surface_interaction(hit, ray);
                        Float3 normal = normalize(it.ng);
                        normal = ocarina::select(dot(normal, ray.dir_max.xyz()) > 0.f, -normal, normal);
                        p = make_float4(it.pos, camera->linear_depth(it.pos));
                        n = make_float4(normal, 1.f);
                    };
                    positions.write(i, p); normals.write(i, n);
                };
                guides.function()->set_block_dim(8u, 8u);
                auto shader = device().compile(guides, "Native GI benchmark physical guides");
                vector<float4> host_positions(pipeline()->pixel_num()), host_normals(pipeline()->pixel_num());
                stream << shader(frame_index_).dispatch(rp->resolution())
                       << positions.download(host_positions.data()) << normals.download(host_normals.data()) << synchronize() << commit();
                auto write_guide = [&](const char *name, const vector<float4> &values) {
                    std::ofstream file(fs::path(directory) / name, std::ios::binary);
                    file.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(float4));
                    if (!file) throw std::runtime_error("Native GI physical guide export failed");
                };
                write_guide("reference_position_depth.f32", host_positions);
                write_guide("reference_world_normal.f32", host_normals);
            }
            vector<float4> direct, indirect;
            if (precise_lobes && string_view(stage).starts_with("composite_")) {
                direct.resize(pipeline()->pixel_num());
                indirect.assign(direct.size(), make_float4(0.f));
                stream << frame_buffer().rt_buffer().view().download(direct.data()) << synchronize() << commit();
            } else {
                direct = di_radiance_->download_radiance();
                indirect = indirect_->download_radiance();
            }
            auto write = [&](const char *channel, const auto &pixels) {
                std::ofstream out(fs::path(directory) / (std::string(stage) + channel + ".f32"), std::ios::binary);
                out.write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * sizeof(float4));
                if (!out) throw std::runtime_error("ReSTIR diagnostic write failed");
            };
            write("_direct", direct); write("_indirect", indirect);
        };
        if (gi_enabled && !use_reference_gi && std::getenv("VISION_EVAL_GI_STORAGE_DEBUG") != nullptr) {
            const auto directory = evaluation_debug::directory(frame_index_);
            if (!directory.empty()) {
                debug_readback("producer");
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
        debug_readback("raw");
        const bool denoiser = !denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled() && denoiser_->requires_lobe_signals();
        if (native_denoising && use_di) {
            const auto physical_di_settings = physical_di_reblur_settings(
                DataWrap::parse(denoiser_->reblur_settings_json())).dump();
            if (!native_di_denoiser_ || any(native_denoiser_size_ != pipeline()->resolution()) ||
                native_di_denoiser_->reblur_settings_json() != physical_di_settings)
                stream << synchronize() << commit();
            prepare_native_di_denoiser(physical_di_settings);
            begin_denoise_timing(stream);
            auto physical = denoise_input();
            physical.use_stable_planes = false;
            physical.layer_count = 1u; physical.layer_index = 0u;
            physical.allow_fused_composition = false; physical.compose_only = false;
            auto dd = restir_di_->denoiser_diffuse(), ds = restir_di_->denoiser_specular();
            physical.lobe_signals = {dd, ds, dd, ds};
            submit(native_di_denoiser_->dispatch(physical), &cur_stage_profile_.spatial_angular_ms);
            if (!evaluation_debug::directory(frame_index_).empty())
                restir_gi::copy_physical_di_snapshots(stream, frame_index_);
            for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
                auto gi = denoise_input();
                gi.use_stable_planes = true; gi.layer_count = StablePlaneCount; gi.layer_index = layer;
                gi.allow_fused_composition = false; gi.compose_only = false;
                auto d = indirect_->native_reblur_diffuse(layer), sp = indirect_->native_reblur_specular(layer);
                gi.lobe_signals = {d, sp, d, sp};
                gi.prepared_reblur_prefix = indirect_->native_reblur_prefix();
                gi.prepared_reblur_headers = indirect_->native_reblur_headers();
                gi.prepared_reblur_motion_y = indirect_->native_reblur_motion_y();
                submit(denoiser_->dispatch(gi), &cur_stage_profile_.spatial_angular_ms);
            }
            NativeReblurComposeParam p;
            p.di_diffuse = dd.descriptor(); p.di_specular = ds.descriptor();
            p.di_bypass = restir_di_->denoiser_bypass().descriptor();
            p.gi_bypass = indirect_->native_reblur_bypass().descriptor();
            p.output = frame_buffer().rt_buffer().view().descriptor();
            p.direct_factor = di_settings_->factor(); p.indirect_factor = indirect_->factor();
            p.gi_diffuse0 = indirect_->native_reblur_diffuse(0u).descriptor();
            p.gi_specular0 = indirect_->native_reblur_specular(0u).descriptor();
            p.gi_diffuse1 = indirect_->native_reblur_diffuse(1u).descriptor();
            p.gi_specular1 = indirect_->native_reblur_specular(1u).descriptor();
            p.gi_diffuse2 = indirect_->native_reblur_diffuse(2u).descriptor();
            p.gi_specular2 = indirect_->native_reblur_specular(2u).descriptor();
            submit(native_reblur_compose_(p).dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
            end_denoise_timing(stream);
            debug_readback("filtered");
            debug_readback("composite_native");
        } else {
        // Keep the explicit raw lobe contract observable with denoising off,
        // just as PT does. This path exists only when Denoiser was selected.
        if (denoiser_ && denoiser_->requires_lobe_signals()) {
            const float2 factors = make_float2(di_settings_->factor(), indirect_->factor());
            if (denoiser && any(factors != denoiser_factors_)) {
                denoiser_->set_enabled(false); denoiser_->set_enabled(true); denoiser_factors_ = factors;
            }
            if (use_di && !gi_enabled && di_lobe_signals) {
                ReSTIRDILobeStageParam signals;
                signals.diffuse = restir_di_->denoiser_diffuse().descriptor();
                signals.specular = restir_di_->denoiser_specular().descriptor();
                signals.bypass = restir_di_->denoiser_bypass().descriptor();
                signals.factor = factors.x;
                submit(stage_restir_di_lobes_(signals).dispatch(rp->resolution()), &cur_stage_profile_.combine_ms);
            } else if (!precise_lobes || !gi_enabled)
                submit(prepare_denoiser_(factors.x, factors.y).dispatch(pipeline()->resolution()),
                       &cur_stage_profile_.combine_ms);
        }
        if (!denoiser_runtime_disabled() && denoiser_ && denoiser_->enabled()) {
            begin_denoise_timing(stream);
            auto dn_input = denoise_input();
            const char *batch_compose = std::getenv("REBLUR_BATCH_COMPOSE");
            dn_input.compose_only = denoiser_->impl_type() == "ReBLUR" &&
                (!batch_compose || batch_compose[0] != '0') && !raw_lobes_requested(precise_lobes);
            submit(denoiser_->dispatch(dn_input), &cur_stage_profile_.spatial_angular_ms);
            debug_readback("filtered");
            if (denoiser) {
                if (!dn_input.composed_lobe_output)
                    submit(compose_denoiser_().dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
                debug_readback("composite_pre");
                if (extra_aa) {
                    auto coverage = denoise_input();
                    coverage.composed_coverage = true;
                    coverage.total_radiance_coverage = true;
                    coverage.direct = frame_buffer().rt_buffer().view();
                    coverage.indirect = coverage.direct; // Total coverage omits this channel.
                    submit(denoiser_->dispatch(coverage), &cur_stage_profile_.spatial_angular_ms);
                }
                debug_readback("composite_post");
            }
            end_denoise_timing(stream);
        }
        }
        // Raw ownership is unchanged. Filtered output resolves that same sum
        // before channel factors are applied exactly once in presentation.
        submit(combine_(frame_index_, denoiser ? 1.f : di_settings_->factor(), denoiser ? 1.f : indirect_->factor(), uint(denoiser))
                   .dispatch(pipeline()->resolution()), &cur_stage_profile_.combine_ms);
        submit(frame_buffer().post_path_tracing(frame_index_), &cur_stage_profile_.postprocess_ms);
        submit(frame_buffer().render_final(frame_index_), &cur_stage_profile_.render_final_ms);
        increase_frame_index();
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, RealTimeIntegrator)
