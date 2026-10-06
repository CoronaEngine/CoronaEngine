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
