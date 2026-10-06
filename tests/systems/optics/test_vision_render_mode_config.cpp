#include "vision/vision_render_mode_config.h"

#include "base/import/json_util.h"
#include "base/import/project_desc.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

using Corona::CameraVisionRenderMode;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

void expect_near(float actual, float expected, std::string_view message) {
    if (std::abs(actual - expected) > 0.0001f) {
        fail(message);
    }
}

std::optional<std::filesystem::path> find_external_ssat_scene() {
    const std::filesystem::path relative =
        std::filesystem::path{"test_vision"} / "render_scene" / "cbox-lf" /
        "vision_scene.json";
    const std::filesystem::path fixed =
        std::filesystem::path{"D:/Documents/GitHub/CoronaExample"} / relative;
    if (std::filesystem::exists(fixed)) {
        return fixed;
    }

    for (auto cursor = std::filesystem::current_path(); !cursor.empty();) {
        const auto candidate = cursor.parent_path() / "CoronaExample" / relative;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
        const auto parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    return std::nullopt;
}

vision::DataWrap make_base_scene() {
    return vision::DataWrap{
        {"scene",
         {{"camera", vision::DataWrap::object()},
          {"materials", vision::DataWrap::array()},
          {"mediums", vision::DataWrap::object()},
          {"shapes", vision::DataWrap::array()},
          {"lights", vision::DataWrap::array()}}},
        {"render",
         {{"integrator",
           {{"type", "pt"},
            {"param",
             {{"denoiser",
               {{"type", "SSAT"},
                {"param", {{"ssat_radius", 7}, {"history", 4}}}}}}}}}}},
        {"pipeline",
         {{"type", "fixed"},
          {"param",
           {{"frame_buffer",
             {{"type", "lightfield"},
              {"param", {{"resolution", {64, 32}}, {"view_count", 8}}}}}}}}},
        {"output", {{"denoise", true}, {"spp", 1}}},
    };
}

void path_tracing_preserves_source_lightfield_framebuffer() {
    auto data = make_base_scene();
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::PathTracing);

    expect(!data["output"]["denoise"].get<bool>(),
           "path_tracing must leave denoise to the independent camera toggle");
    expect(data["pipeline"]["param"]["frame_buffer"]["type"].get<std::string>() ==
               "lightfield",
           "path_tracing should preserve the source lightfield framebuffer");
    expect(data["render"]["integrator"]["param"]["denoiser"]["type"].get<std::string>() ==
               "svgf",
           "path_tracing should not keep SSAT denoiser descriptor");
}

void progressive_pt_enables_accumulation_without_denoise() {
    auto data = make_base_scene();
    data["pipeline"]["param"]["frame_buffer"]["param"]["accumulation"] = false;
    const auto progressive = CameraVisionRenderMode::ProgressivePathTracing;
    Corona::Systems::Vision::configure_vision_scene_for_mode(data, progressive);
    expect(!data["output"]["denoise"].get<bool>(),
           "progressive PT must disable realtime denoising");
    expect(data["pipeline"]["param"]["frame_buffer"]["param"]["accumulation"].get<bool>(),
           "progressive PT must override a source that disables accumulation");
    expect(Corona::Systems::Vision::vision_render_mode_name(progressive) == "progressive_path_tracing",
           "progressive PT must round trip through camera settings");
}

void selecting_pt_overrides_a_restir_source_integrator() {
    for (const auto mode : {CameraVisionRenderMode::PathTracing,
                            CameraVisionRenderMode::ProgressivePathTracing}) {
        auto data = make_base_scene();
        data["render"]["integrator"]["type"] = "rt";
        Corona::Systems::Vision::configure_vision_scene_for_mode(data, mode);
        expect(data["render"]["integrator"]["type"].get<std::string>() == "pt",
               "selecting PT must replace a source ReSTIR integrator");
    }
}

void restir_selects_rt_and_preserves_scene_parameters() {
    auto data = make_base_scene();
    data["render"]["integrator"]["param"]["direct"]["M_light"] = 12;
    data["pipeline"]["param"]["frame_buffer"]["param"]["accumulation"] = true;
    Corona::Systems::Vision::configure_vision_scene_for_mode(data, CameraVisionRenderMode::ReSTIR);
    vision::ProjectDesc desc;
    desc.init(data);
    expect(desc.renderer_desc.integrator_desc.sub_type == "rt", "ReSTIR must instantiate rt");
    expect(desc.renderer_desc.integrator_desc.denoiser_desc.sub_type == "svgf" &&
               !desc.output_desc.denoise, "ReSTIR must not implicitly enable SVGF");
    expect(desc.renderer_desc.integrator_desc["direct"]["M_light"].as_uint() == 12,
           "ReSTIR must preserve source sampling settings");
    for (const auto channel : {"direct", "indirect"}) {
        for (const auto reuse : {"spatial", "temporal"}) {
            expect(data["render"]["integrator"]["param"][channel][reuse].is_object(),
                   "ReSTIR reuse defaults require object parameters rather than null");
        }
    }
    expect(desc.pipeline_desc.frame_buffer_desc.sub_type == "lightfield",
           "ReSTIR must preserve source framebuffer type");
    expect(!desc.pipeline_desc.frame_buffer_desc["accumulation"].as_bool(),
           "ReSTIR must not accumulate stale progressive samples");
    expect(Corona::Systems::Vision::vision_render_mode_name(CameraVisionRenderMode::ReSTIR) == "restir",
           "ReSTIR must have a distinct runtime key");
}

void svgf_preserves_source_lightfield_framebuffer() {
    auto data = make_base_scene();
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::SVGF);

    expect(data["output"]["denoise"].get<bool>(),
           "svgf should set output.denoise=true");
    expect(data["pipeline"]["param"]["frame_buffer"]["type"].get<std::string>() ==
               "lightfield",
           "svgf should preserve the source lightfield framebuffer");
    expect(data["render"]["integrator"]["param"]["denoiser"]["type"].get<std::string>() ==
               "svgf",
           "svgf should use svgf integrator denoiser");
    expect(data["render"]["integrator"]["param"]["denoiser"]["param"]["ssat_radius"]
               .get<int>() == 7,
           "svgf compatibility path should preserve existing denoiser params");
}

void path_tracing_and_svgf_preserve_source_normal_framebuffer() {
    for (const auto mode : {CameraVisionRenderMode::PathTracing,
                            CameraVisionRenderMode::SVGF}) {
        auto data = make_base_scene();
        data["pipeline"]["param"]["frame_buffer"]["type"] = "normal";

        Corona::Systems::Vision::configure_vision_scene_for_mode(data, mode);

        expect(data["pipeline"]["param"]["frame_buffer"]["type"]
                       .get<std::string>() == "normal",
               "path_tracing and svgf should preserve a source normal framebuffer");
    }
}

void ssat_rewrites_svgf_scene_to_lightfield_ssat() {
    auto data = make_base_scene();
    data["pipeline"]["param"]["frame_buffer"]["type"] = "normal";
    data["render"]["integrator"]["param"]["denoiser"]["type"] = "svgf";
    data["output"]["denoise"] = false;

    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::SSAT);

    expect(data["output"]["denoise"].get<bool>(),
           "ssat should set output.denoise=true");
    expect(data["pipeline"]["param"]["frame_buffer"]["type"].get<std::string>() ==
               "lightfield",
           "ssat should use lightfield framebuffer");
    expect(data["render"]["integrator"]["param"]["denoiser"]["type"].get<std::string>() ==
               "SSAT",
           "ssat should use SSAT integrator denoiser");
}

void missing_blocks_are_created_for_requested_mode() {
    vision::DataWrap data = vision::DataWrap::object();
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::SVGF);

    expect(data["output"]["denoise"].get<bool>(),
           "missing output block should be created with denoise=true");
    expect(data["pipeline"]["param"]["frame_buffer"]["type"].get<std::string>() ==
               "normal",
           "missing pipeline block should be created for svgf framebuffer");
    expect(data["render"]["integrator"]["param"]["denoiser"]["type"].get<std::string>() ==
               "svgf",
           "missing render block should be created for svgf denoiser");
}

void ssat_defaults_from_cbox_lf_are_applied_to_minimal_scene() {
    vision::DataWrap data = vision::DataWrap::object();
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::SSAT);

    auto& denoiser_param =
        data["render"]["integrator"]["param"]["denoiser"]["param"];
    expect_near(denoiser_param["rho_base"].get<float>(), 1.0f,
                "SSAT should default rho_base from cbox-lf");
    expect_near(denoiser_param["sigma_lum"].get<float>(), 6.0f,
                "SSAT should default sigma_lum from cbox-lf");
    expect_near(denoiser_param["sigma_depth_angular"].get<float>(), 100.0f,
                "SSAT should default sigma_depth_angular from cbox-lf");
    expect_near(denoiser_param["sigma_x"].get<float>(), 3.0f,
                "SSAT should default sigma_x from cbox-lf");
    expect_near(denoiser_param["sigma_u"].get<float>(), 0.5f,
                "SSAT should default sigma_u from cbox-lf");
    expect_near(denoiser_param["angular_range"].get<float>(), 1.0f,
                "SSAT should default angular_range from cbox-lf");
    expect(denoiser_param["spatial_radius"].get<int>() == 1,
           "SSAT should default spatial_radius from cbox-lf");
    expect(denoiser_param["angular_samples"].get<int>() == 7,
           "SSAT should default angular_samples from cbox-lf");
    expect(denoiser_param["enabled"].get<bool>(),
           "SSAT should default enabled from cbox-lf");
    expect(denoiser_param["use_adaptive_sampling"].get<bool>(),
           "SSAT should default adaptive sampling from cbox-lf");

    auto& frame_buffer_param = data["pipeline"]["param"]["frame_buffer"]["param"];
    expect(frame_buffer_param["accumulation"].get<bool>(),
           "SSAT lightfield framebuffer should default accumulation from cbox-lf");
    expect(frame_buffer_param["lenticular"]["num_views"].get<int>() == 48,
           "SSAT lightfield framebuffer should default num_views from cbox-lf");
    expect(frame_buffer_param["lenticular"]["res_w"].get<int>() == 1280,
           "SSAT lightfield framebuffer should default res_w from cbox-lf");
    expect(frame_buffer_param["lenticular"]["res_h"].get<int>() == 720,
           "SSAT lightfield framebuffer should default res_h from cbox-lf");
    expect_near(frame_buffer_param["geometry"]["d_f"].get<float>(), 4.2f,
                "SSAT lightfield framebuffer should default d_f from cbox-lf");
    expect_near(frame_buffer_param["geometry"]["array_angle_deg"].get<float>(),
                5.0f,
                "SSAT lightfield framebuffer should default array angle from cbox-lf");
}

void ssat_defaults_do_not_override_existing_values() {
    auto data = make_base_scene();
    data["render"]["integrator"]["param"]["denoiser"]["param"]["sigma_lum"] = 2.5f;
    data["render"]["integrator"]["param"]["denoiser"]["param"]
        ["angular_samples"] = 5;
    data["pipeline"]["param"]["frame_buffer"]["param"]["lenticular"]
        ["num_views"] = 24;

    Corona::Systems::Vision::configure_vision_scene_for_mode(
        data, CameraVisionRenderMode::SSAT);

    auto& denoiser_param =
        data["render"]["integrator"]["param"]["denoiser"]["param"];
    expect_near(denoiser_param["sigma_lum"].get<float>(), 2.5f,
                "SSAT defaults should not override custom sigma_lum");
    expect(denoiser_param["angular_samples"].get<int>() == 5,
           "SSAT defaults should not override custom angular_samples");
    expect(data["pipeline"]["param"]["frame_buffer"]["param"]["lenticular"]
               ["num_views"]
                   .get<int>() == 24,
           "SSAT framebuffer defaults should not override custom num_views");
    expect_near(denoiser_param["sigma_depth_angular"].get<float>(), 100.0f,
                "SSAT defaults should still fill missing denoiser values");
}

void mode_names_and_denoise_flags_are_stable() {
    expect(Corona::Systems::Vision::vision_render_mode_name(
               CameraVisionRenderMode::PathTracing) == "path_tracing",
           "path_tracing mode name should be stable");
    expect(Corona::Systems::Vision::vision_render_mode_name(
               CameraVisionRenderMode::SVGF) == "svgf",
           "svgf mode name should be stable");
    expect(Corona::Systems::Vision::vision_render_mode_name(
               CameraVisionRenderMode::SSAT) == "ssat",
           "ssat mode name should be stable");
    expect(!Corona::Systems::Vision::vision_render_mode_uses_denoise(
               CameraVisionRenderMode::PathTracing),
           "path_tracing must not implicitly enable denoise");
    expect(Corona::Systems::Vision::vision_render_mode_uses_denoise(
               CameraVisionRenderMode::SVGF),
           "svgf should enable denoise");
    expect(Corona::Systems::Vision::vision_render_mode_uses_denoise(
               CameraVisionRenderMode::SSAT),
           "ssat should enable denoise");
}

void cbox_lf_scene_supports_pt_and_ssat_mode_import() {
    const auto scene_path = find_external_ssat_scene();
    if (!scene_path) {
        std::cout << "SKIP: cbox-lf external Vision scene sample not found\n";
        return;
    }

    auto pt_data = vision::create_json_from_file(*scene_path);
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        pt_data, CameraVisionRenderMode::PathTracing);
    vision::ProjectDesc pt_desc;
    pt_desc.scene_path = scene_path->parent_path();
    pt_desc.init(pt_data);

    expect(!pt_desc.output_desc.denoise,
           "PT algorithm configuration must not implicitly request denoise");
    expect(pt_desc.pipeline_desc.frame_buffer_desc.sub_type == "lightfield",
           "PT import from cbox-lf should preserve its lightfield framebuffer");
    expect(pt_desc.renderer_desc.integrator_desc.denoiser_desc.sub_type == "svgf",
           "PT import from cbox-lf should not keep the SSAT denoiser descriptor");

    auto ssat_data = vision::create_json_from_file(*scene_path);
    Corona::Systems::Vision::configure_vision_scene_for_mode(
        ssat_data, CameraVisionRenderMode::SSAT);
    vision::ProjectDesc ssat_desc;
    ssat_desc.scene_path = scene_path->parent_path();
    ssat_desc.init(ssat_data);

    expect(ssat_desc.output_desc.denoise,
           "SSAT import from cbox-lf should enable realtime denoise");
    expect(ssat_desc.pipeline_desc.frame_buffer_desc.sub_type == "lightfield",
           "SSAT import should use a lightfield framebuffer");
    expect(ssat_desc.renderer_desc.integrator_desc.denoiser_desc.sub_type == "SSAT",
           "SSAT import should use the SSAT integrator denoiser");
    expect(ssat_data["render"]["integrator"]["param"]["denoiser"]["param"]
                    ["angular_samples"]
                        .get<int>() == 7,
           "SSAT import should preserve cbox-lf angular_samples parameter");
}

}  // namespace

int main() {
    for (const auto mode : {CameraVisionRenderMode::PathTracing,
                            CameraVisionRenderMode::ReSTIR,
                            CameraVisionRenderMode::ProgressivePathTracing}) {
        expect(!Corona::Systems::Vision::vision_render_mode_uses_denoise(mode),
               "ray tracing algorithm selection must not implicitly enable SVGF");
    }
    selecting_pt_overrides_a_restir_source_integrator();
    restir_selects_rt_and_preserves_scene_parameters();
    progressive_pt_enables_accumulation_without_denoise();
    mode_names_and_denoise_flags_are_stable();
    path_tracing_preserves_source_lightfield_framebuffer();
    svgf_preserves_source_lightfield_framebuffer();
    path_tracing_and_svgf_preserve_source_normal_framebuffer();
    ssat_rewrites_svgf_scene_to_lightfield_ssat();
    missing_blocks_are_created_for_requested_mode();
    ssat_defaults_from_cbox_lf_are_applied_to_minimal_scene();
    ssat_defaults_do_not_override_existing_values();
    cbox_lf_scene_supports_pt_and_ssat_mode_import();
    return 0;
}
