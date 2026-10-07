// A real GPU regression for separating direct and indirect transport in ReSTIR.
// A lone diffuse triangle in a constant environment has no indirect paths:
// every BSDF continuation leaves the triangle and immediately hits the sky.
#include "base/import/importer.h"
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "rhi/context.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <crtdbg.h>
#include <windows.h>
#endif

void check_restir_reprojection(vision::Pipeline& pipeline);
void check_stable_plane(vision::Pipeline& pipeline, bool tinted_mirror = true);
void check_restir_material_reuse(vision::Pipeline& pipeline, const std::vector<vision::SurfaceData>& surfaces);
void check_restir_gi_depth_one_continuation(vision::Pipeline& pipeline);
void check_restir_gi_receiver_support(vision::Pipeline& pipeline);

namespace {
namespace fs = std::filesystem;
constexpr unsigned kResolution = 16;
constexpr unsigned kFrames = 8;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void configure_headless_process() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _putenv_s("VISION_DISABLE_DENOISER", "1");
#else
    setenv("VISION_DISABLE_DENOISER", "1", 1);
#endif
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    expect(bool(output), "could not write the ReSTIR estimator fixture");
}

vision::DataWrap scene_description(bool direct, bool reuse = false) {
    auto scene = vision::DataWrap::parse(R"({
      "scene": {
        "camera": {"type":"thin_lens", "param": {
          "fov_y":45, "lens_radius":0,
          "filter":{"type":"box"},
          "transform":{"type":"look_at", "param": {
            "position":[0,0,0], "target_pos":[0,0,-1], "up":[0,1,0]
          }}
        }},
        "materials":[{"type":"diffuse", "name":"receiver", "param": {
          "color":[0.8,0.8,0.8], "sigma":0
        }}],
        "shapes":[{"type":"model", "param": {
          "fn":"receiver.obj", "material":"receiver", "normalize_to_unit_bounds":false
        }}],
        "lights":[{"type":"spherical", "param": {
          "color":{"channels":"xyz", "node":{"type":"number", "param":{"value":[1,1,1]}}},
          "scale":1
        }}]
      },
      "render": {
        "sampler":{"type":"independent", "param":{"spp":1}},
        "light_sampler":{"type":"uniform"},
        "integrator":{"type":"rt", "param": {
          "max_depth":3, "min_depth":3,
          "direct": {
            "M_light":8, "M_bsdf":1,
            "temporal":{"open":false}, "spatial":{"open":false}
          },
          "indirect": {
            "temporal":{"open":false}, "spatial":{"open":false}
          }
        }}
      },
      "pipeline":{"type":"fixed", "param": {
        "frame_buffer":{"type":"normal", "param":{"resolution":[16,16]}}
      }},
      "output":{"spp":0, "save_exit":false, "denoise":false, "fn":"unused.exr"}
    })");
    // The rt integrator owns its DI/GI ScreenBuffers, rather than publishing
    // them in FrameBuffer::direct_lighting()/indirect_lighting(). Its public
    // scene switches select exactly one channel in the production combine pass.
    auto& parameters = scene["render"]["integrator"]["param"];
    parameters["direct"]["open"] = direct;
    parameters["indirect"]["open"] = !direct;
    parameters["indirect"]["temporal"]["open"] = reuse;
    parameters["indirect"]["spatial"]["open"] = reuse;
    return scene;
}

struct ChannelStats {
    double sum{};
    float max_abs{};
    float max_primary_bary_delta{};
    float max_lighting_delta{};
    unsigned samples{};
    [[nodiscard]] double mean() const { return sum / samples; }
};

ChannelStats render_channel(vision::Pipeline& pipeline) {
    expect(pipeline.resolution().x == kResolution && pipeline.resolution().y == kResolution,
           "ReSTIR estimator fixture must render at 16x16");
    auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    expect(illumination != nullptr, "fixture must import the real illumination integrator");
    illumination->set_denoise_enabled(false);
    pipeline.frame_buffer()->set_enable_accumulation(false);
    pipeline.prepare();
    pipeline.frame_buffer()->prepare_view_texture();
    pipeline.upload_data();
    expect(!pipeline.frame_buffer()->enable_accumulation(), "raw estimator test must not accumulate frames");
    expect(!illumination->denoiser() || !illumination->denoiser()->enabled(),
           "raw estimator test must not invoke a denoiser");
    expect(pipeline.scene().instances().size() == 1u,
           "fixture must contain only the diffuse receiver, with no indirect reflectors");
    expect(pipeline.scene().light_manager().env_light() != nullptr,
           "fixture must contain an actual environment light");

    std::vector<vision::float4> pixels(pipeline.pixel_num());
    std::vector<vision::TriangleHit> hits(pixels.size());
    std::vector<vision::SurfaceData> surfaces(pixels.size());
    std::vector<vision::TriangleHit> first_hits;
    std::vector<vision::float4> first_pixels;
    ChannelStats stats;
    for (unsigned frame = 0; frame < kFrames; ++frame) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        const auto rendered_frame = pipeline.frame_index() - 1u;
        pipeline.stream()
            << pipeline.frame_buffer()->rt_buffer().device_buffer().download(pixels.data())
            << pipeline.frame_buffer()->cur_visibility_buffer_view(rendered_frame)
                   .subview(0, hits.size()).download(hits.data())
            << pipeline.frame_buffer()->cur_surfaces_view(rendered_frame)
                   .download(surfaces.data())
            << vision::synchronize() << vision::commit();
        if (frame == 0u) {
            first_hits = hits;
            first_pixels = pixels;
        }
        // Even on one large triangle, different camera samples disagree in
        // barycentrics. At silhouettes that becomes a different surface: SVGF
        // would demodulate and accumulate radiance with the wrong guides.
        for (size_t index = 0; index < hits.size(); ++index) {
            const auto& primary = hits[index];
            const auto& shaded = surfaces[index].hit;
            expect(primary.inst_id == shaded.inst_id && primary.prim_id == shaded.prim_id,
                   "ReSTIR and the denoiser GBuffer must cover the same primary surface");
            if (primary.inst_id != vision::InvalidUI32) {
                expect(std::abs(primary.bary.x - shaded.bary.x) < 1e-6f &&
                       std::abs(primary.bary.y - shaded.bary.y) < 1e-6f,
                       "ReSTIR must shade the exact GBuffer camera ray, including pixel jitter");
            }
        }
        // The central 8x8 is comfortably inside the triangle for all pixel
        // jitter positions. Sky/background pixels can never satisfy this test.
        for (unsigned y = 4; y < 12; ++y) {
            for (unsigned x = 4; x < 12; ++x) {
                const auto index = y * kResolution + x;
                expect(hits[index].inst_id != vision::InvalidUI32,
                       "every measured pixel must hit the diffuse receiver");
                stats.max_primary_bary_delta = std::max({stats.max_primary_bary_delta,
                    std::abs(hits[index].bary.x - first_hits[index].bary.x),
                    std::abs(hits[index].bary.y - first_hits[index].bary.y)});
                const auto& pixel = pixels[index];
                stats.max_lighting_delta = std::max(stats.max_lighting_delta,
                                                    std::abs(pixel.x - first_pixels[index].x));
                expect(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z),
                       "raw ReSTIR channel must be finite");
                expect(pixel.x >= -1e-6f && pixel.y >= -1e-6f && pixel.z >= -1e-6f,
                       "raw ReSTIR radiance must be nonnegative");
                stats.sum += (double(pixel.x) + pixel.y + pixel.z) / 3.0;
                stats.max_abs = std::max({stats.max_abs, std::abs(pixel.x), std::abs(pixel.y), std::abs(pixel.z)});
                ++stats.samples;
            }
        }
    }
    return stats;
}

void check_primary_sampling_modes(vision::Pipeline& pipeline) {
    // Exercise live runtime switches: these must not require shader recompilation.
    std::vector<vision::TriangleHit> first(pipeline.pixel_num()), second(first.size());
    auto read_hits = [&](auto& hits) {
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        pipeline.stream() << pipeline.frame_buffer()->cur_visibility_buffer_view(pipeline.frame_index() - 1u)
                                 .subview(0, hits.size()).download(hits.data())
                          << vision::synchronize() << vision::commit();
    };
    for (bool accumulate : {true, false}) {
        pipeline.frame_buffer()->set_enable_accumulation(accumulate);
        // Match the UI's resource lifecycle when toggling after prepare().
        pipeline.frame_buffer()->auto_manage_accumulation_buffer(accumulate);
        read_hits(first);
        read_hits(second);
        float max_delta = 0.f;
        for (unsigned y = 4; y < 12; ++y) {
            for (unsigned x = 4; x < 12; ++x) {
                auto i = y * kResolution + x;
                max_delta = std::max({max_delta, std::abs(first[i].bary.x - second[i].bary.x),
                                               std::abs(first[i].bary.y - second[i].bary.y)});
            }
        }
        std::cout << "accumulation=" << accumulate << " primary_bary_delta=" << max_delta << '\n';
        expect(accumulate ? max_delta > 1e-5f : max_delta <= 1e-6f,
               "ReSTIR must jitter accumulated frames and restore stable pixel centres when accumulation is disabled");
    }
}
} // namespace

int main() {
    configure_headless_process();
    fs::path fixture;
    try {
        const auto runtime_directory = fs::current_path();
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        fixture = fs::temp_directory_path() / ("corona_restir_estimators_" + std::to_string(suffix));
        fs::create_directories(fixture);
        std::cout << "fixture=" << fixture.string() << '\n';
        // Counterclockwise from the camera: geometric and shading normals +Z.
        // At z=-1 the full 45-degree camera frustum fits within this triangle.
        write_file(fixture / "receiver.obj",
                   "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0 0 1\nf 1//1 2//1 3//1\n");
        write_file(fixture / "direct.json", scene_description(true).dump(2));
        write_file(fixture / "indirect.json", scene_description(false).dump(2));
        write_file(fixture / "indirect-reuse.json", scene_description(false, true).dump(2));
        write_file(fixture / "receiver-tilted.obj",
                   "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0.7071067812 0 0.7071067812\nf 1//1 2//1 3//1\n");
        auto support_scene = scene_description(false);
        support_scene["scene"]["shapes"][0]["param"]["fn"] = "receiver-tilted.obj";
        write_file(fixture / "support.json", support_scene.dump(2));

        // Match the embedded engine bootstrap. Standalone hotfix registration
        // can leave ../bin ahead of the chosen runtime directory in this list.
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(runtime_directory);
        // As in the other Vision integration tests, the device outlives the
        // global kernel tools and all pipelines through process shutdown.
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        vision::Global::instance().set_scene_path(fixture);
        const auto direct_pipeline = vision::Importer::import_scene(fixture / "direct.json");
        expect(bool(direct_pipeline), "could not import direct-channel fixture");
        const auto direct = render_channel(*direct_pipeline);
        std::cout << "direct_mean=" << direct.mean() << " direct_max=" << direct.max_abs << '\n';
        std::cout << "raw_static_primary_bary_delta=" << direct.max_primary_bary_delta << '\n';
        expect(direct.max_primary_bary_delta <= 1e-6f,
               "raw ReSTIR with a stationary pinhole camera must not shift the primary sample every frame");
        expect(direct.max_lighting_delta > 1e-4f,
               "stabilizing the raw primary ray must not freeze stochastic lighting samples");
        expect(direct.mean() > 0.05, "DI must illuminate the receiver under the white environment");
        check_primary_sampling_modes(*direct_pipeline);

        const auto indirect_pipeline = vision::Importer::import_scene(fixture / "indirect.json");
        expect(bool(indirect_pipeline), "could not import indirect-channel fixture");
        const auto indirect = render_channel(*indirect_pipeline);
        std::cout << "indirect_mean=" << indirect.mean() << " indirect_max=" << indirect.max_abs
                  << " measured_hit_samples=" << indirect.samples << '\n';
        expect(indirect.max_abs <= 1e-6f,
               "GI must exclude an initial BSDF miss to the environment: this scene has zero indirect transport");
        const auto reused_pipeline = vision::Importer::import_scene(fixture / "indirect-reuse.json");
        expect(bool(reused_pipeline), "could not import the GI null-history fixture");
        const auto reused = render_channel(*reused_pipeline);
        std::cout << "reused_indirect_mean=" << reused.mean() << " reused_indirect_max=" << reused.max_abs << '\n';
        expect(reused.max_abs <= 1e-6f,
               "temporal and spatial reuse must preserve zero GI when every initial candidate is null");
        std::cout << "PASS: ReSTIR keeps initial environment lighting exclusively in DI\n";
        bool regression_failed = false;
        auto check = [&](auto function, vision::Pipeline& pipeline) {
            try { function(pipeline); }
            catch (const std::exception& error) {
                std::cerr << "FAIL: " << error.what() << '\n';
                regression_failed = true;
            }
        };
        check(check_restir_reprojection, *reused_pipeline);
        check(check_restir_gi_depth_one_continuation, *direct_pipeline);
        const auto support_pipeline = vision::Importer::import_scene(fixture / "support.json");
        expect(bool(support_pipeline), "could not import the tilted receiver support fixture");
        check(check_restir_gi_receiver_support, *support_pipeline);
        // Capture real ReSTIR surface guides, including material/texture evaluation.
        // Equal geometry must not hide a material boundary; similar separately
        // imported materials must remain reusable regardless of their identities.
        const char* materials[]{
            R"({"type":"diffuse","param":{"color":[0.8,0.1,0.1]}})",
            R"({"type":"diffuse","param":{"color":[0.1,0.8,0.1]}})",
            R"({"type":"diffuse","param":{"color":[0.75,0.12,0.1]}})",
            R"({"type":"substrate","param":{"color":[0.5,0.5,0.5],"spec":[0.04,0.04,0.04],"roughness":0.2}})",
            R"({"type":"substrate","param":{"color":[0.5,0.5,0.5],"spec":[0.8,0.8,0.8],"roughness":0.2}})",
            R"({"type":"plastic","param":{"color":[0.5,0.5,0.5],"roughness":0.1}})",
            R"({"type":"plastic","param":{"color":[0.5,0.5,0.5],"roughness":0.8}})",
            R"({"type":"diffuse","param":{"color":[0,0,0]}})",
            R"({"type":"glass","param":{"color":[1,1,1],"ior":1.1,"roughness":0.5}})",
            R"({"type":"glass","param":{"color":[1,1,1],"ior":5.0,"roughness":0.5}})",
            R"({"type":"principled_bsdf","param":{"color":[0.8,0.2,0.1],"coat_weight":1,"coat_ior":2.5,"roughness":0.4}})",
            R"({"type":"principled_bsdf","param":{"color":[0.8,0.2,0.1],"coat_weight":1,"coat_ior":2.5,"roughness":0.4}})",
            R"({"type":"diffuse","param":{"color":[0.8,0.2,0.1]}})",
            R"({"type":"diffuse","param":{"color":[0.8,0.2,0.1]}})",
            R"({"type":"principled_bsdf","param":{"color":[0.8,0.2,0.1],"coat_weight":1,"coat_ior":2.5,"roughness":0.4,"normal":{"node":"test_normal","channels":"xyz"}},"node_tab":{"test_normal":{"type":"number","param":{"value":[0.6,0,0.8]}}}})",
            R"({"type":"principled_bsdf","param":{"color":[0.8,0.2,0.1],"coat_weight":1,"coat_ior":2.5,"roughness":0.4,"normal":{"node":"test_normal","channels":"xyz"}},"node_tab":{"test_normal":{"type":"number","param":{"value":[0.6,0,0.8]}}}})",
        };
        std::vector<vision::SurfaceData> material_surfaces;
        // Compare live scenes: the existing MaterialLut cache is keyed by a raw
        // BindlessArray address, so destroying fixtures here can alias stale LUTs.
        std::vector<vision::SP<vision::Pipeline>> material_fixtures;
        vision::SP<vision::Pipeline> material_pipeline;
        for (const char* material_json : materials) {
            auto description = scene_description(true);
            const auto material_index = material_surfaces.size();
            if (material_index == 11u || material_index == 15u)
                description["scene"]["camera"]["param"]["transform"]["param"]["position"] = vision::DataWrap::parse("[3,0,0]");
            if (material_index == 12u || material_index == 13u)
                description["render"]["spectrum"] = vision::DataWrap::parse(R"({"type":"hero","param":{"dimension":3}})");
            auto material = vision::DataWrap::parse(material_json);
            material["name"] = "receiver";
            description["scene"]["materials"][0] = material;
            write_file(fixture / "material.json", description.dump(2));
            material_pipeline = vision::Importer::import_scene(fixture / "material.json");
            material_fixtures.push_back(material_pipeline);
            material_pipeline->frame_buffer()->set_enable_accumulation(false);
            material_pipeline->prepare();
            material_pipeline->frame_buffer()->prepare_view_texture();
            for (unsigned frame = 0; frame < (material_index == 13u ? 7u : 1u); ++frame) {
                material_pipeline->upload_data();
                material_pipeline->display(1.0 / 60.0);
            }
            std::vector<vision::SurfaceData> data(material_pipeline->pixel_num());
            material_pipeline->stream() << material_pipeline->frame_buffer()->cur_surfaces_view(material_pipeline->frame_index() - 1u).download(data.data())
                << vision::synchronize() << vision::commit();
            expect(data[8u * 16u + 8u].hit.inst_id != vision::InvalidUI32, "material fixture must hit receiver");
            material_surfaces.push_back(data[8u * 16u + 8u]);
            if (material_index == 11u || material_index == 13u || material_index == 15u) {
                const auto& a = material_surfaces[material_index - 1u];
                const auto& b = material_surfaces[material_index];
                float delta = 0.f;
                for (unsigned c = 0; c < 4u; ++c) {
                    delta = std::max(delta, std::abs(a.diffuse_roughness[c] - b.diffuse_roughness[c]));
                    delta = std::max(delta, std::abs(a.specular_roughness[c] - b.specular_roughness[c]));
                }
                std::cout << "Material guide stable index=" << material_index << " max_delta=" << delta << '\n';
                if (delta > 1e-5f) {
                    std::cerr << "FAIL: material guide must not vary with view or sampled wavelengths\n";
                    regression_failed = true;
                }
            }
        }
        check_restir_material_reuse(*material_pipeline, material_surfaces);
        // A planar mirror at z=-1 reflects the receiver at z=+1. Its virtual
        // image is z=-3: endpoint guides and primary mirror guides must differ.
        write_file(fixture / "mirror-receiver.obj",
                   "v -4 -4 1\nv 0 4 1\nv 4 -4 1\nvn 0 0 -1\nf 1//1 2//1 3//1\n");
#ifdef _WIN32
        _putenv_s("VISION_DISABLE_DENOISER", "0");
#else
        setenv("VISION_DISABLE_DENOISER", "0", 1);
#endif
        auto mirror_scene = scene_description(true, true);
        mirror_scene["output"]["denoise"] = true;
        mirror_scene["render"]["integrator"]["param"]["direct"]["temporal"]["open"] = true;
        mirror_scene["render"]["integrator"]["param"]["direct"]["spatial"]["open"] = true;
        mirror_scene["render"]["integrator"]["param"]["indirect"]["open"] = true;
        mirror_scene["render"]["denoiser"] = vision::DataWrap::parse(R"({"type":"svgf","param":{"enabled":true}})");
        mirror_scene["render"]["integrator"]["param"]["denoiser"] = vision::DataWrap::parse(R"({"type":"svgf","param":{"enabled":true}})");
        mirror_scene["scene"]["materials"][0]["type"] = "mirror";
        mirror_scene["scene"]["materials"][0]["param"] = vision::DataWrap::parse(R"({"color":[0.6,0.8,1],"roughness":0.001})");
        mirror_scene["scene"]["materials"].push_back(vision::DataWrap::parse(
            R"({"type":"diffuse","name":"reflected","param":{"color":[0.8,0.2,0.1]}})"));
        mirror_scene["scene"]["shapes"].push_back(vision::DataWrap::parse(
            R"({"type":"model","param":{"fn":"mirror-receiver.obj","material":"reflected","normalize_to_unit_bounds":false}})"));
        write_file(fixture / "mirror.json", mirror_scene.dump(2));
        auto mirror_pipeline = vision::Importer::import_scene(fixture / "mirror.json");
        mirror_pipeline->frame_buffer()->set_enable_accumulation(false);
        mirror_pipeline->prepare();
        mirror_pipeline->frame_buffer()->prepare_view_texture();
        mirror_pipeline->upload_data();
        mirror_pipeline->display(1.0 / 60.0);
        std::vector<vision::SurfaceData> mirror_surfaces(mirror_pipeline->pixel_num());
        mirror_pipeline->stream() << mirror_pipeline->frame_buffer()->cur_surfaces_view(0u).download(mirror_surfaces.data())
            << vision::synchronize() << vision::commit();
        const auto& reflected = mirror_surfaces[8u * 16u + 8u];
        std::cout << "mirror endpoint_z=" << reflected.pos_diff.z << " depth=" << reflected.normal_depth.w << '\n';
        expect(reflected.is_replaced && std::abs(reflected.pos_diff.z - 1.f) < 1e-4f,
               "stable plane must retain the reflected receiver position, not the mirror position");
        expect(std::abs(reflected.normal_depth.w - 3.f) < 1e-4f,
               "stable plane depth must describe the virtual image behind the mirror");
        check_stable_plane(*mirror_pipeline);
        // One mesh contains a coplanar two-triangle mirror on the left and
        // another plane on the right. Branch identity must describe the plane,
        // not just the instance and not the individual triangle.
        write_file(fixture / "faceted-mirror.obj",
            "v -0.6 -0.6 -1\nv 0 -0.6 -1\nv 0 0.6 -1\nv -0.6 0.6 -1\n"
            "v 0.6 -0.6 -0.94\nv 0.6 0.6 -0.94\n"
            "vn 0 0 1\nvn -0.099503719 0 0.99503719\n"
            "f 1//1 2//1 3//1\nf 1//1 3//1 4//1\n"
            "f 2//2 5//2 6//2\nf 2//2 6//2 3//2\n");
        mirror_scene["scene"]["shapes"][0]["param"]["fn"] = "faceted-mirror.obj";
        write_file(fixture / "mirror.json", mirror_scene.dump(2));
        auto facets = vision::Importer::import_scene(fixture / "mirror.json");
        facets->frame_buffer()->set_enable_accumulation(false);
        facets->prepare(); facets->frame_buffer()->prepare_view_texture();
        facets->upload_data(); facets->display(1.0 / 60.0);
        std::vector<vision::SurfaceData> facet_surfaces(facets->pixel_num());
        std::vector<vision::TriangleHit> facet_primary(facets->pixel_num());
        facets->stream() << facets->frame_buffer()->cur_surfaces_view(0u).download(facet_surfaces.data())
            << facets->frame_buffer()->cur_visibility_buffer_view(0u).download(facet_primary.data())
            << vision::synchronize() << vision::commit();
        unsigned branches[4]{};
        for (size_t i = 0; i < facet_surfaces.size(); ++i) {
            const auto prim = facet_primary[i].prim_id;
            if (facet_primary[i].inst_id == 0u && prim < 4u && facet_surfaces[i].is_replaced)
                branches[prim] = facet_surfaces[i].stable_branch;
        }
        expect(branches[0] && branches[1] && branches[2] && branches[3], "faceted mirror test must see all four triangles");
        expect(branches[0] == branches[1] && branches[2] == branches[3] && branches[0] != branches[2],
               "stable branches must share coplanar triangles and separate different planes in one instance");
        std::cout << "PASS: mirror plane identity across four triangles\n";
        // A scene-level opt-out must retain the legacy mirror-surface guides.
        mirror_scene["scene"]["shapes"][0]["param"]["fn"] = "receiver.obj";
        mirror_scene["render"]["integrator"]["param"]["direct"]["stable_planes"] = false;
        write_file(fixture / "mirror-off.json", mirror_scene.dump(2));
        auto disabled_planes = vision::Importer::import_scene(fixture / "mirror-off.json");
        disabled_planes->frame_buffer()->set_enable_accumulation(false);
        disabled_planes->prepare(); disabled_planes->frame_buffer()->prepare_view_texture();
        disabled_planes->upload_data(); disabled_planes->display(1.0 / 60.0);
        std::vector<vision::SurfaceData> disabled_surfaces(disabled_planes->pixel_num());
        disabled_planes->stream() << disabled_planes->frame_buffer()->cur_surfaces_view(0u).download(disabled_surfaces.data())
            << vision::synchronize() << vision::commit();
        const auto& disabled = disabled_surfaces[8u * 16u + 8u];
        expect(disabled.is_replaced && disabled.stable_branch == vision::InvalidUI32 &&
                   std::abs(disabled.pos_diff.z + 1.f) < 1e-4f && std::abs(disabled.normal_depth.w - 1.f) < 1e-4f,
               "stable_planes=false must disable stable reuse and restore the primary mirror guides");
        // No material named/type mirror exists in this scene. The same planar
        // continuation must work for Kitchen's low-roughness conductor.
        auto metal_scene = mirror_scene;
        metal_scene["render"]["integrator"]["param"]["direct"]["stable_planes"] = true;
        metal_scene["scene"]["materials"][0]["type"] = "metal";
        metal_scene["scene"]["materials"][0]["param"] = vision::DataWrap::parse(
            R"({"material_name":"Cr","roughness":0.002,"remapping_roughness":false})");
        write_file(fixture / "stable-metal.json", metal_scene.dump(2));
        auto metal_pipeline = vision::Importer::import_scene(fixture / "stable-metal.json");
        metal_pipeline->frame_buffer()->set_enable_accumulation(false);
        metal_pipeline->prepare(); metal_pipeline->frame_buffer()->prepare_view_texture();
        metal_pipeline->upload_data(); metal_pipeline->display(1.0 / 60.0);
        std::vector<vision::SurfaceData> metal_surfaces(metal_pipeline->pixel_num());
        metal_pipeline->stream() << metal_pipeline->frame_buffer()->cur_surfaces_view(0u).download(metal_surfaces.data())
            << vision::synchronize() << vision::commit();
        const auto& metal = metal_surfaces[8u * 16u + 8u];
        expect(metal.is_replaced && metal.stable_branch != 0u && metal.stable_branch != vision::InvalidUI32 &&
                   std::abs(metal.pos_diff.z - 1.f) < 1e-4f && std::abs(metal.normal_depth.w - 3.f) < 1e-4f,
               "low-roughness metal must use stable endpoint guides without any mirror material in the scene");
        check_stable_plane(*metal_pipeline, false);
        const char* rejected_materials[] = {
            R"({"type":"metal","param":{"material_name":"Cr","roughness":0.3,"remapping_roughness":false}})",
            R"({"type":"glass","param":{"material_name":"BK7","roughness":0.001,"remapping_roughness":false}})",
            R"({"type":"metal","param":{"material_name":"Cr","roughness":0.002,"remapping_roughness":false,"normal":{"node":"tilted","channels":"xyz"}},"node_tab":{"tilted":{"type":"number","param":{"value":[0.6,0,0.8]}}}})",
            R"({"type":"mix","param":{"frac":0.5,"mat0":{"type":"mirror","param":{"color":[1,1,1],"roughness":0.001}},"mat1":{"type":"diffuse","param":{"color":[0.5,0.5,0.5]}}}})",
        };
        for (const char* material_json : rejected_materials) {
            auto rejected = metal_scene;
            auto material = vision::DataWrap::parse(material_json);
            material["name"] = rejected["scene"]["materials"][0]["name"];
            rejected["scene"]["materials"][0] = material;
            write_file(fixture / "stable-rejected.json", rejected.dump(2));
            auto rejected_pipeline = vision::Importer::import_scene(fixture / "stable-rejected.json");
            material_fixtures.push_back(rejected_pipeline);
            rejected_pipeline->frame_buffer()->set_enable_accumulation(false);
            rejected_pipeline->prepare(); rejected_pipeline->frame_buffer()->prepare_view_texture();
            rejected_pipeline->upload_data(); rejected_pipeline->display(1.0 / 60.0);
            std::vector<vision::SurfaceData> data(rejected_pipeline->pixel_num());
            rejected_pipeline->stream() << rejected_pipeline->frame_buffer()->cur_surfaces_view(0u).download(data.data())
                << vision::synchronize() << vision::commit();
            const auto& center = data[8u * 16u + 8u];
            expect(center.stable_branch == 0u || center.stable_branch == vision::InvalidUI32,
                   "rough, transmitting, perturbed or mixed scattering must not form a stable reflection branch");
        }
        std::cout << "PASS: BSDF stable capability for metal and conservative scattering rejection\n";
        fs::remove(fixture / "stable-rejected.json");
        fs::remove(fixture / "stable-metal.json");
        fs::remove(fixture / "mirror-off.json");
        fs::remove(fixture / "faceted-mirror.obj");
        fs::remove(fixture / "mirror.json");
        fs::remove(fixture / "mirror-receiver.obj");
        fs::remove(fixture / "material.json");
        expect(!regression_failed, "ReSTIR regression checks failed");
        // Remove only the files this test created; preserve the fixture on failure.
        fs::remove(fixture / "direct.json");
        fs::remove(fixture / "indirect.json");
        fs::remove(fixture / "indirect-reuse.json");
        fs::remove(fixture / "receiver.obj");
        fs::remove(fixture / "receiver-tilted.obj");
        fs::remove(fixture / "support.json");
        // Importers may have left their own cache files in this directory.
        std::error_code cleanup_error;
        fs::remove(fixture, cleanup_error);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        if (!fixture.empty()) std::cerr << "Preserved fixture: " << fixture.string() << '\n';
        return 1;
    }
}
