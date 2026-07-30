//
// Created by Zero on 2025/01/26.
// SSAT (Sparse Spatial-Angular-Temporal) Light Field Denoiser
//
// A geometry-driven denoiser for automultiscopic light field displays based on
// 5D phase-space sparse signal reconstruction with three algebraic operators:
// - Ray Transfer Operator (Φ): Optical abstraction
// - Manifold Transport Operator (Ψ): Geometric connectivity
// - Sparse Gathering Operator (Υ): Radiance aggregation
//

#pragma once

#include "math/basic_types.h"
#include "dsl/dsl.h"
#include "base/denoiser.h"
#include "base/sensor/frame_buffer.h"
#include "base/sensor/light_field_types.h"
#include "adaptive_sampler.h"
#include "spatial_angular_filter.h"
#include "temporal_accumulator.h"
#include "view_reconstructor.h"
#include "utils.h"
#include "ssat_config.h"

namespace vision::ssat {
using namespace ocarina;

// ============================================================================
// SSAT Denoiser Main Class
// ============================================================================

/// SSAT: Sparse Spatial-Angular-Temporal Light Field Denoiser
/// 
/// Implements a three-phase denoising pipeline:
/// 1. Disparity-Guided Adaptive Sampling (Phase 1)
/// 2. Unified Spatio-Angular Integration (Phase 2)
/// 3. 5D Temporal Accumulation (Phase 3)
class SSAT : public Denoiser, public GBufferCallback, public enable_shared_from_this<SSAT> {
private:
    // ========================================================================
    // Sub-components
    // ========================================================================
    
    HotfixSlot<SP<AdaptiveSampler>> adaptive_sampler_;
    HotfixSlot<SP<SpatialAngularFilter>> spatial_angular_filter_;
    HotfixSlot<SP<TemporalAccumulator>> temporal_accumulator_;
    HotfixSlot<SP<SsatViewReconstructor>> view_reconstructor_;
    
    // ========================================================================
    // Parameters
    // ========================================================================
    
    struct Params {
        // Phase 1: Adaptive Sampling
        float rho_base{1.0f};           // Quality config: ρ_base=1 (all pixels sampled)
        float alpha_shear{1.0f};        // Shear sensitivity coefficient α
        
        // Phase 2: Spatio-Angular Filter
        float sigma_lum{3.0f};          // Luminance sigma for edge-stopping
        float sigma_normal{128.0f};     // Normal edge-stopping exponent
        float sigma_depth_angular{1.0f};// Depth-angular sigma for w_geo
        float sigma_x{2.0f};            // Spatial bandwidth for Gaussian kernel
        float sigma_u{0.3f};            // Angular bandwidth for Gaussian kernel
        float beta{1.0f};               // Projection scale for angular transport
        float angular_range{0.15f};     // Normalized angular aperture span
        int spatial_radius{2};          // Spatial integration radius (pixels)
        int angular_samples{3};         // Angular samples for multi-view gathering
        
        // Phase 3: Temporal Accumulation
        float alpha_base{0.1f};         // Base temporal blending factor
        float angular_bandwidth{0.1f};  // ω_angular for strict history rejection
        
        // Global
        bool enabled{true};             // Enable/disable SSAT
        bool use_adaptive_sampling{true}; // Must be true so sampling_mask is computed
        
        Params() = default;
        explicit Params(const DenoiserDesc &desc)
            : rho_base(desc["rho_base"].as_float(1.0f)),
              alpha_shear(desc["alpha_shear"].as_float(1.0f)),
              sigma_lum(desc["sigma_lum"].as_float(6.0f)),
              sigma_normal(desc["sigma_normal"].as_float(128.0f)),
              sigma_depth_angular(desc["sigma_depth_angular"].as_float(100.0f)),
              sigma_x(desc["sigma_x"].as_float(3.0f)),
              sigma_u(desc["sigma_u"].as_float(0.5f)),
              beta(desc["beta"].as_float(1.0f)),
              angular_range(desc["angular_range"].as_float(1.0f)),
              spatial_radius(desc["spatial_radius"].as_int(1)),
              angular_samples(desc["angular_samples"].as_int(7)),
              alpha_base(desc["alpha_base"].as_float(0.1f)),
              angular_bandwidth(desc["angular_bandwidth"].as_float(0.1f)),
              enabled(desc["enabled"].as_bool(true)),
              use_adaptive_sampling(desc["use_adaptive_sampling"].as_bool(true)) {}

        void apply_env_overrides() noexcept {
            auto read_env_float = [](const char *name, float &val) {
                if (const char *s = std::getenv(name)) val = std::stof(s);
            };
            auto read_env_int = [](const char *name, int &val) {
                if (const char *s = std::getenv(name)) val = std::stoi(s);
            };
            read_env_float("SSAT_RHO_BASE", rho_base);
            read_env_float("SSAT_SIGMA_LUM", sigma_lum);
            read_env_float("SSAT_SIGMA_NORMAL", sigma_normal);
            read_env_float("SSAT_SIGMA_Z", sigma_depth_angular);
            read_env_float("SSAT_SIGMA_X", sigma_x);
            read_env_float("SSAT_SIGMA_U", sigma_u);
            read_env_float("SSAT_BETA", beta);
            read_env_float("SSAT_ANGULAR_RANGE", angular_range);
            read_env_float("SSAT_ALPHA_BASE", alpha_base);
            read_env_float("SSAT_ANGULAR_BW", angular_bandwidth);
            read_env_int("SSAT_SPATIAL_RADIUS", spatial_radius);
            read_env_int("SSAT_ANGULAR_SAMPLES", angular_samples);
        }
    };
    Params params_;
    
    // Cached resolution info
    uint total_subpixels_{0};
    uint2 subpixel_resolution_{};
    uint last_sampling_mask_frame_{InvalidUI32};

    void sync_subpixel_resolution(uint2 raytracing_resolution) noexcept {
        subpixel_resolution_ = raytracing_resolution;
        total_subpixels_ = subpixel_resolution_.x * subpixel_resolution_.y;
    }

    void ensure_runtime_buffers(uint total_subpixels) noexcept {
        adaptive_sampler_->ensure_buffers(total_subpixels);
        spatial_angular_filter_->ensure_buffers(total_subpixels);
        temporal_accumulator_->ensure_buffers(total_subpixels);
    }

public:
    struct SpatialFilterDispatch {
        CommandBatch commands;
        BufferView<RadType4> spatial_result;
    };

    void ensure_phase_buffers(uint2 resolution) noexcept {
        uint total = resolution.x * resolution.y;
        ensure_runtime_buffers(total);
    }

    // ========================================================================
    // Construction
    // ========================================================================
    
    SSAT() = default;
    explicit SSAT(const DenoiserDesc &desc)
        : Denoiser(desc),
          params_(desc) {
        params_.apply_env_overrides();
    }
    
    VS_HOTFIX_MAKE_RESTORE(Denoiser, adaptive_sampler_, spatial_angular_filter_,
                           temporal_accumulator_, view_reconstructor_, params_,
                           total_subpixels_, subpixel_resolution_,
                           last_sampling_mask_frame_)
    VS_MAKE_PLUGIN_NAME_FUNC
    
    // ========================================================================
    // Initialization
    // ========================================================================
    
    void initialize_(const NodeDesc &node_desc) noexcept override {
        adaptive_sampler_ = make_shared<AdaptiveSampler>(this);
        spatial_angular_filter_ = make_shared<SpatialAngularFilter>(this);
        temporal_accumulator_ = make_shared<TemporalAccumulator>(this);
        view_reconstructor_ = make_shared<SsatViewReconstructor>(this);
    }
    
    // ========================================================================
    // GBufferCallback Implementation
    // ========================================================================
    
    void compute_GBuffer(const RayState &rs, const Interaction &it) noexcept override {
        // SSAT doesn't need to inject into GBuffer computation
        // All data is accessed via visibility buffer
    }
    
    // ========================================================================
    // Denoiser Interface
    // ========================================================================
    
    void prepare() noexcept override {
        // SSAT works in the framebuffer raytracing domain.
        // For light-field rendering this is already the expanded subpixel resolution.
        sync_subpixel_resolution(frame_buffer().raytracing_resolution());
        
        // Register GBuffer callback
        frame_buffer().register_callback(shared_from_this());
    }
    
    void compile() noexcept override {
        adaptive_sampler_->compile();
        spatial_angular_filter_->compile();
        temporal_accumulator_->compile();
        view_reconstructor_->compile();
    }
    
    void update_resolution(uint2 resolution) noexcept override {
        // `resolution` is passed in raytracing coordinates by the integrator.
        // Light-field paths already provide subpixel resolution here.
        sync_subpixel_resolution(resolution);
    }
    
    [[nodiscard]] bool enabled() noexcept override {
        return params_.enabled;
    }

    void set_enabled(bool enabled) noexcept override {
        params_.enabled = enabled;
    }
    
    /// SSAT supports light field denoising
    [[nodiscard]] bool supports_lightfield() const noexcept override { return true; }
    
    /// Standard denoiser dispatch (for non-light-field use)
    [[nodiscard]] CommandBatch dispatch(RealTimeDenoiseInput &input) noexcept override {
        // SSAT is designed for light field displays
        // For standard rendering, just pass through or use basic filtering
        return {};
    }
    
    /// Dispatch SSAT denoising for light field rendering
    /// @param input Light field specific denoising input
    /// @return Command list for the three-phase pipeline
    [[nodiscard]] SpatialFilterDispatch dispatch_spatial_filter(vision::LightFieldDenoiseInput &input) noexcept {
        SpatialFilterDispatch ret{};
        if (!params_.enabled) {
            return ret;
        }
        uint2 new_subpixel_res = input.resolution;
        uint runtime_total_subpixels = new_subpixel_res.x * new_subpixel_res.y;
        if (new_subpixel_res.x != subpixel_resolution_.x ||
            new_subpixel_res.y != subpixel_resolution_.y) {
            subpixel_resolution_ = new_subpixel_res;
            total_subpixels_ = runtime_total_subpixels;
        }
        ensure_runtime_buffers(runtime_total_subpixels);

        const LenticularParams &lent = input.lenticular;
        const LightFieldGeometry &geom = input.geometry;

        // À-trous multi-pass Gaussian gathering on 5D manifold
        BufferView<RadType4> src = input.direct;
        BufferView<RadType4> dst = spatial_angular_filter_->temp_buffer().view();
        BufferView<uint> sampling_mask = adaptive_sampler_->sampling_mask();
        BufferView<float> shear_magnitude = adaptive_sampler_->shear_magnitude();

        // Compute beta from focal plane width (paper's projection scale factor)
        float beta_computed = params_.beta;

        // Single-pass architecture
        uint num_iterations = 1;
        for (uint i = 0; i < num_iterations; ++i) {
            int step_size = static_cast<int>(1u << i);
            ret.commands << spatial_angular_filter_->dispatch_filter(
                src, dst, input.visibility, sampling_mask, shear_magnitude,
                input.camera_pos, lent, geom, input.l2w,
                params_.sigma_lum, params_.sigma_normal, params_.sigma_depth_angular,
                params_.sigma_x, params_.sigma_u, input.z_ref(), beta_computed,
                params_.angular_range,
                params_.spatial_radius, params_.angular_samples,
                step_size,
                subpixel_resolution_);
            std::swap(src, dst);
        }

        ret.spatial_result = src;
        return ret;
    }

    [[nodiscard]] CommandBatch dispatch_temporal_accumulation(
        vision::LightFieldDenoiseInput &input,
        BufferView<RadType4> spatial_result) noexcept {
        CommandBatch ret;
        if (!params_.enabled) {
            return ret;
        }
        const LenticularParams &lent = input.lenticular;
        const LightFieldGeometry &geom = input.geometry;

        // ================================================================
        // Phase 3: 5D Temporal Accumulation
        // ================================================================
        ret << temporal_accumulator_->dispatch_accumulate(
            spatial_result, input.direct, input.visibility, input.prev_visibility,
            input.motion_vec, lent, geom, input.l2w, input.prev_l2w,
            params_.alpha_base, params_.angular_bandwidth,
            params_.sigma_x, params_.sigma_u,
            input.frame_index, input.resolution);
        return ret;
    }

    /// Reconstruct one complete 2D view from valid path-traced light-field
    /// samples before Phase 2/3 can mix angular neighbors.
    [[nodiscard]] CommandBatch dispatch_view_reconstruction(
        BufferView<RadType4> source,
        BufferView<float4> output,
        const LenticularParams &lenticular,
        std::uint32_t view_index,
        std::uint32_t frame_index) noexcept {
        if (!params_.enabled) {
            return {};
        }
        return view_reconstructor_->dispatch(
            source, output, lenticular, view_index, frame_index);
    }

    [[nodiscard]] CommandBatch dispatch_lightfield(vision::LightFieldDenoiseInput &input) noexcept override {
        CommandBatch ret;
        if (!params_.enabled) {
            return ret;
        }
        SpatialFilterDispatch spatial = dispatch_spatial_filter(input);
        ret << spatial.commands;
        ret << dispatch_temporal_accumulation(input, spatial.spatial_result);
        return ret;
    }

    [[nodiscard]] CommandBatch prepass_sampling_mask(vision::LightFieldDenoiseInput &input) noexcept {
        CommandBatch ret;
        if (!params_.enabled || !params_.use_adaptive_sampling) {
            return ret;
        }
        uint runtime_total_subpixels = input.resolution.x * input.resolution.y;
        ensure_runtime_buffers(runtime_total_subpixels);
        if (last_sampling_mask_frame_ == input.frame_index) {
            return ret;
        }
        ret << adaptive_sampler_->compute_sampling_mask(
            input.prev_visibility, input.prev_camera_pos,
            input.lenticular, input.z_ref(),
            params_.rho_base, params_.alpha_shear,
            input.frame_index, input.resolution);
        last_sampling_mask_frame_ = input.frame_index;
        return ret;
    }

    [[nodiscard]] bool use_adaptive_sampling() const noexcept {
        return params_.enabled && params_.use_adaptive_sampling;
    }

    [[nodiscard]] BufferView<uint> sampling_mask() const noexcept {
        return adaptive_sampler_->sampling_mask();
    }
    
    // ========================================================================
    // UI
    // ========================================================================
    
    void render_sub_UI(Widgets *widgets) noexcept override {
        changed_ |= widgets->check_box("enabled", &params_.enabled);
        changed_ |= widgets->check_box("adaptive_sampling", &params_.use_adaptive_sampling);
        
        widgets->use_tree("Phase 1: Adaptive Sampling", [&] {
            changed_ |= widgets->drag_float("rho_base", &params_.rho_base, 0.01f, 0.1f, 1.0f);
            changed_ |= widgets->drag_float("alpha_shear", &params_.alpha_shear, 0.1f, 0.1f, 5.0f);
        });
        
        widgets->use_tree("Phase 2: Spatio-Angular Filter", [&] {
            changed_ |= widgets->drag_float("sigma_lum", &params_.sigma_lum, 0.1f, 0.1f, 10.0f);
            changed_ |= widgets->drag_float("sigma_normal", &params_.sigma_normal, 1.0f, 1.0f, 512.0f);
            changed_ |= widgets->drag_float("sigma_depth_angular", &params_.sigma_depth_angular, 0.1f, 0.1f, 5.0f);
            changed_ |= widgets->drag_float("sigma_x (spatial)", &params_.sigma_x, 0.1f, 0.1f, 10.0f);
            changed_ |= widgets->drag_float("sigma_u (angular)", &params_.sigma_u, 0.1f, 0.01f, 2.0f);
            changed_ |= widgets->drag_float("beta (projection)", &params_.beta, 0.1f, 0.1f, 5.0f);
            changed_ |= widgets->drag_float("angular_range", &params_.angular_range, 0.01f, 0.01f, 1.0f);
            changed_ |= widgets->input_int_limit("spatial_radius", &params_.spatial_radius, 1, SSATConfig::SpatialAngular::kMaxSpatialRadius);
            changed_ |= widgets->input_int_limit("angular_samples", &params_.angular_samples, 1, SSATConfig::SpatialAngular::kMaxAngularSamples);
        });
        
        widgets->use_tree("Phase 3: Temporal Accumulation", [&] {
            changed_ |= widgets->drag_float("alpha_base", &params_.alpha_base, 0.01f, 0.01f, 0.5f);
            changed_ |= widgets->drag_float("angular_bandwidth", &params_.angular_bandwidth, 0.01f, 0.01f, 1.0f);
        });
        
        widgets->use_tree("Stats", [&] {
            widgets->text(ocarina::format("Subpixel Resolution: {}x{}", 
                                         subpixel_resolution_.x, subpixel_resolution_.y));
            widgets->text(ocarina::format("Total Subpixels: {}", total_subpixels_));
        });
    }
    
    // ========================================================================
    // Accessors
    // ========================================================================
    
    [[nodiscard]] const Params& params() const noexcept { return params_; }
    [[nodiscard]] Params& params() noexcept { return params_; }
    
    [[nodiscard]] AdaptiveSampler* adaptive_sampler() noexcept { return adaptive_sampler_.get(); }
    [[nodiscard]] SpatialAngularFilter* spatial_angular_filter() noexcept { return spatial_angular_filter_.get(); }
    [[nodiscard]] TemporalAccumulator* temporal_accumulator() noexcept { return temporal_accumulator_.get(); }
};

}// namespace vision::ssat
