#pragma once
#include "dsl/dsl.h"

namespace vision::relax {
using namespace ocarina;
struct Settings {
    float diffuse_history{30.f}, specular_history{30.f};
    float diffuse_fast_history{6.f}, specular_fast_history{6.f};
    float history_fix_frames{3.f}, history_fix_stride{14.f}, history_fix_normal_power{8.f};
    float variance_history_threshold{3.f};
    float diffuse_prepass{30.f}, specular_prepass{50.f}, min_hit_weight{.1f};
    float diffuse_phi{2.f}, specular_phi{1.f}, lobe_fraction{.5f}, roughness_fraction{.15f};
    float specular_variance_boost{0.f}, specular_lobe_slack{.15f};
    float color_box_sigma{2.f}, depth_threshold{.003f};
    float luminance_relaxation{.5f}, normal_relaxation{.3f}, roughness_relaxation{1.f};
    float antilag_acceleration{.3f}, antilag_spatial_sigma{4.5f}, antilag_temporal_sigma{.5f}, antilag_reset{.5f};
    uint atrous_iterations{5u}, anti_firefly{0u}, roughness_stopping{1u}, reconstruction{0u};
};
}

OC_STRUCT(vision::relax, Settings, diffuse_history, specular_history, diffuse_fast_history, specular_fast_history,
          history_fix_frames, history_fix_stride, history_fix_normal_power, variance_history_threshold,
          diffuse_prepass, specular_prepass, min_hit_weight, diffuse_phi, specular_phi, lobe_fraction, roughness_fraction,
          specular_variance_boost, specular_lobe_slack, color_box_sigma, depth_threshold,
          luminance_relaxation, normal_relaxation, roughness_relaxation,
          antilag_acceleration, antilag_spatial_sigma, antilag_temporal_sigma, antilag_reset,
          atrous_iterations, anti_firefly, roughness_stopping, reconstruction) {};
