// A real GPU regression for separating direct and indirect transport in ReSTIR.
// A lone diffuse triangle in a constant environment has no indirect paths:
// every BSDF continuation leaves the triangle and immediately hits the sky.
#include "../support/vision_test_scene.h"
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "rhi/context.h"
#include "rhi/stats.h"
#include "base/scattering/material.h"
#include "base/sampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <crtdbg.h>
#include <windows.h>
#endif

void check_restir_reservoir_lifetimes(vision::Pipeline& pipeline);
void check_stable_plane_storage_restore(vision::Pipeline& pipeline);
void check_restir_gi_depth_one_continuation(vision::Pipeline& pipeline);
void check_principled_stable_planes(vision::Pipeline& pipeline);

namespace {
namespace fs = std::filesystem;
constexpr unsigned kResolution = 16;
constexpr unsigned kFrames = 8;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Removing deterministic enumeration, swapping branch IDs, using f instead of
// p*f*cos/pdf, or consuming RNG in the query must fail this GPU contract.
template<typename Glass = vision::DielectricLobe, typename Mirror = vision::PureReflectionLobe,
         typename Evaluator = vision::MaterialEvaluator, typename Mix = vision::LobeSet>
void check_stable_lobes(vision::Pipeline& pipeline) {
    using namespace vision;
    if constexpr (!requires(const Glass& glass, const Float3& wo) { glass.eval_stable_lobes(wo); }) {
        expect(false, "missing deterministic stable lobe contract");
    } else {
        pipeline.activate_global_context();
        Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
        DielectricLobe::prepare();
        // Publish any LUT handles registered by this low-level contract before
        // compiling shaders that access them.
        pipeline.geometry().upload_bindless_array(pipeline.stream());
        pipeline.upload_data();
        auto& sampler = pipeline.renderer().sampler();
        constexpr uint samples = 4096u;
        Kernel kernel = [&](BufferVar<float4> output) {
            Env::instance().clear_global_vars();
            sampler->load_data();
            Uint id = dispatch_id();
            Uint scenario = id % 8u;
            SampledWavelengths swl{3u};
            Float eta = select(scenario == 2u || scenario == 3u, 1.f / 1.5f, 1.5f);
            Float x = select(scenario == 0u || scenario == 4u, 0.f, 0.5f);
            x = select(scenario == 3u, 0.9f, x);
            Float z = sqrt(1.f - x * x) * select(scenario == 2u || scenario == 3u, -1.f, 1.f);
            Float3 wo = make_float3(x, Float{0.f}, z);
            SampledSpectrum color{Float3{make_float3(0.25f, 0.f, 0.8f)}};
            color = select(scenario == 4u, SampledSpectrum::zero(swl), color);
            auto fresnel = make_shared<FresnelDielectric>(SampledSpectrum{swl, eta}, swl);
            auto microfacet = make_shared<GGXMicrofacet>(make_float2(0.001f), true);
            Glass glass{fresnel, microfacet, color, scenario == 6u,
                        select(scenario == 5u, SurfaceData::Glossy, SurfaceData::NearSpec)};
            sampler->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
            Float expected_rng = sampler->next_1d();
            sampler->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
            auto branches = glass.eval_stable_lobes(wo);
            auto repeated = glass.eval_stable_lobes(wo);
            Float actual_rng = sampler->next_1d();
            Bool ok = expected_rng == actual_rng;
            Float failed_stage = select(ok, 0.f, 1.f);
            Bool supported = scenario != 5u && scenario != 6u;
            ok &= branches.has_stochastic_remainder == !supported;
            for (uint branch = 0u; branch < 2u; ++branch) {
                auto& b = branches.lobes[branch];
                auto& repeat = repeated.lobes[branch];
                ok &= all(b.wi == repeat.wi) && b.probability == repeat.probability &&
                      all(b.weight.vec3() == repeat.weight.vec3()) && b.valid == repeat.valid;
                ok &= b.transmission == (branch == 0u);
                $if(!supported) { ok &= !b.valid; };
            }
            failed_stage = select(failed_stage == 0.f && !ok, 2.f, failed_stage);
            auto bs = glass.sample_delta_local(wo, sampler);
            $if(supported) {
                ok &= bs.stable_lobe_index < 2u;
                for (uint branch = 0u; branch < 2u; ++branch) {
                    auto& b = branches.lobes[branch];
                    $if(bs.stable_lobe_index == branch) {
                        Float3 expected = bs.eval.throughput().vec3() * abs_cos_theta(bs.wi) * b.probability;
                        ok &= b.valid && length(b.wi - bs.wi) < 1e-6f &&
                              all(abs(b.weight.vec3() - expected) <= 2e-5f * max(abs(expected), make_float3(1.f)));
                    };
                }
                failed_stage = select(failed_stage == 0.f && !ok, 3.f, failed_stage);
                ok &= abs(branches.lobes[0].probability + branches.lobes[1].probability - 1.f) < 1e-6f;
                ok &= length(branches.lobes[1].wi - make_float3(-wo.xy(), wo.z)) < 1e-6f;
                failed_stage = select(failed_stage == 0.f && !ok, 4.f, failed_stage);
                $if(scenario == 0u) {
                    // At normal incidence Fresnel=0.04; selection is tint weighted.
                    ok &= abs(branches.lobes[1].probability - (0.04f / (0.04f + 0.96f * 0.35f))) < 1e-5f;
                    ok &= length(branches.lobes[0].wi - make_float3(0.f, 0.f, -1.f)) < 1e-6f;
                };
                $if(scenario == 1u) {
                    ok &= length(branches.lobes[0].wi - make_float3(-1.f / 3.f, 0.f, -0.94280904f)) < 1e-5f;
                };
                $if(scenario == 2u) {
                    ok &= length(branches.lobes[0].wi - make_float3(-0.75f, 0.f, 0.66143783f)) < 1e-5f;
                };
                $if(scenario == 3u || scenario == 4u) {
                    ok &= !branches.lobes[0].valid && branches.lobes[0].probability == 0.f &&
                          all(branches.lobes[0].weight.vec3() == make_float3(0.f)) && bs.stable_lobe_index == 1u;
                };
                $if(branches.lobes[0].valid) { ok &= branches.lobes[0].weight[1] == 0.f; };
                failed_stage = select(failed_stage == 0.f && !ok, 5.f, failed_stage);
            }
            $else { ok &= bs.stable_lobe_index == InvalidUI32; };
            output.write(id, make_float4(select(ok, 1.f, -failed_stage), cast<float>(bs.stable_lobe_index),
                                         branches.lobes[0].weight[0], branches.lobes[1].weight[0]));
        };
        auto shader = pipeline.device().compile(kernel, "stable_lobe_contract");
        auto buffer = pipeline.device().create_buffer<float4>(samples, "stable_lobe_contract_results");
        std::vector<float4> data(samples);
        pipeline.stream() << shader(buffer).dispatch(samples) << buffer.download(data.data()) << synchronize() << commit();
        uint reflected = 0u, transmitted = 0u;
        for (uint i = 0; i < samples; ++i) {
            const auto& value = data[i];
            if (value.x != 1.f) std::cerr << "stable_lobe_failed_sample=" << i << " stage=" << value.x << " branch=" << value.y << " weights=" << value.z << ',' << value.w << '\n';
            expect(value.x == 1.f && std::isfinite(value.z) && std::isfinite(value.w),
                   "stable lobe enumeration must preserve RNG, branch IDs, directions and unconditioned transport");
            if (i % 8u == 0u) { reflected += value.y == 1.f; transmitted += value.y == 0.f; }
        }
        expect(reflected > 20u && transmitted > 100u, "contract fixture must sample both glass branches");
        Kernel adapter_kernel = [&](BufferVar<float4> output) {
            Env::instance().clear_global_vars();
            sampler->load_data();
            sampler->set_seed(dispatch_idx().xy(), Uint{19u}, Dimension::PathTracing);
            SampledWavelengths swl{3u};
            Float3 wo = normalize(Float3{make_float3(0.3f, 0.1f, 1.f)});
            auto fresnel = make_shared<FresnelConstant>(swl);
            auto microfacet = make_shared<GGXMicrofacet>(make_float2(0.001f), true);
            auto reflection = make_shared<Mirror>(fresnel,
                make_unique<MicrofacetReflection>(SampledSpectrum{swl, 0.7f}, swl, microfacet), SurfaceData::NearSpec);
            auto branches = reflection->eval_stable_lobes(wo);
            auto sample = reflection->sample_delta_local(wo, sampler);
            Bool ok = !branches.has_stochastic_remainder && !branches.lobes[0].valid &&
                      branches.lobes[1].valid && branches.lobes[1].probability == 1.f &&
                      sample.stable_lobe_index == 1u &&
                      all(abs(branches.lobes[1].weight.vec3() - sample.eval.throughput().vec3() * abs_cos_theta(sample.wi)) < 1e-5f);
            auto mix = Mix::create_mix(Float{0.5f}, reflection, make_shared<DiffuseLobe>(SampledSpectrum{swl, 0.4f}, swl));
            auto mixed = mix->eval_stable_lobes(wo);
            ok &= mixed.has_stochastic_remainder && !mixed.lobes[0].valid && !mixed.lobes[1].valid;
            // Principled keeps zero-weight children in its lobe set. A single
            // active reflection must preserve branch identity and transport.
            LobeSet::Lobes weighted;
            weighted.emplace_back(Float{1.f}, Float{.25f}, reflection);
            weighted.emplace_back(Float{0.f}, Float{0.f},
                make_shared<DiffuseLobe>(SampledSpectrum{swl, .4f}, swl));
            auto single = make_unique<LobeSet>(std::move(weighted));
            auto single_branches = single->eval_stable_lobes(wo);
            auto single_sample = single->sample_delta_local(wo, sampler);
            ok &= single->flag() == SurfaceData::NearSpec && single->supports_stable_reflection(wo) &&
                  !single_branches.has_stochastic_remainder && single_branches.lobes[1].valid &&
                  single_sample.valid() && single_sample.stable_lobe_index == 1u &&
                  all(abs(single_branches.lobes[1].weight.vec3() - branches.lobes[1].weight.vec3() * .25f) < 1e-5f) &&
                  all(abs(single_sample.eval.throughput().vec3() - sample.eval.throughput().vec3() * .25f) < 1e-5f);
            LobeSet::Lobes empty_weights;
            empty_weights.emplace_back(Float{0.f}, Float{0.f}, reflection);
            auto inactive = make_unique<LobeSet>(std::move(empty_weights));
            auto inactive_branches = inactive->eval_stable_lobes(wo);
            ok &= inactive->flag() == BxDFFlag::Unset && inactive_branches.has_stochastic_remainder &&
                  !inactive->sample_delta_local(wo, sampler).valid();
            Interaction it{false};
            it.shading.x = make_float3(0.f, 1.f, 0.f);
            it.shading.y = make_float3(0.f, 0.f, 1.f);
            it.shading.z = make_float3(1.f, 0.f, 0.f);
            it.ng = it.shading.z;
            Evaluator evaluator{it, swl};
            // No per-lobe frame: the adapter must use the existing delta frame,
            // including when individual_ns is enabled in the material registry.
            auto dielectric = make_shared<Glass>(make_shared<FresnelDielectric>(SampledSpectrum{swl, 1.5f}, swl),
                                                 microfacet, SampledSpectrum{swl, 0.7f}, false, SurfaceData::NearSpec);
            auto specular_mix = Mix::create_mix(Float{.5f}, reflection, dielectric);
            ok &= specular_mix->flag() == SurfaceData::NearSpec &&
                  specular_mix->eval_stable_lobes(wo).has_stochastic_remainder &&
                  !specular_mix->sample_delta_local(wo, sampler).valid();
            LobeSet::Lobes dielectric_children;
            dielectric_children.emplace_back(Float{1.f}, Float{.6f}, dielectric);
            dielectric_children.emplace_back(Float{0.f}, Float{0.f}, reflection);
            evaluator.link(make_unique<LobeSet>(std::move(dielectric_children)));
            auto world_wo = it.shading.to_world(wo);
            auto world = evaluator.eval_stable_lobes(world_wo);
            auto world_sample = evaluator.sample_delta(world_wo, sampler);
            for (uint branch = 0u; branch < 2u; ++branch) {
                $if(world_sample.stable_lobe_index == branch) {
                    ok &= world.lobes[branch].valid && length(world.lobes[branch].wi - world_sample.wi) < 1e-6f &&
                          all(abs(world.lobes[branch].weight.vec3() - world_sample.eval.throughput().vec3() * world.lobes[branch].probability) < 1e-5f);
                };
            }
            output.write(dispatch_id(), make_float4(cast<float>(ok)));
        };
        auto adapter = pipeline.device().compile(adapter_kernel, "stable_lobe_adapter_contract");
        pipeline.stream() << adapter(buffer).dispatch(samples) << buffer.download(data.data()) << synchronize() << commit();
        for (const auto& value : data) {
            expect(value.x == 1.f, "single-active reflection/transmission, mixed-lobe rejection and world-frame contracts must agree with sampling");
        }
        std::cout << "PASS: stable lobe contract, 4096 GPU samples including TIR and zero-colour transmission\n";
    }
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
    _putenv_s("VISION_RESTIR_GI_BACKEND", "");
#else
    setenv("VISION_DISABLE_DENOISER", "1", 1);
    unsetenv("VISION_RESTIR_GI_BACKEND");
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
        "materials":[{"type":"principled_bsdf","name":"receiver","param":{"color":[0.8,0.8,0.8],"subsurface_weight":0,"roughness":1,"spec_tint":[0,0,0]}}],
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
    parameters["direct"]["max_recursion"] = 1u;
    parameters["indirect"]["open"] = !direct;
    parameters["indirect"]["backend"] = "rtxpt";
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

ChannelStats render_channel(vision::Pipeline& pipeline, size_t expected_instances = 1u) {
    expect(pipeline.resolution().x == kResolution && pipeline.resolution().y == kResolution,
           "ReSTIR estimator fixture must render at 16x16");
    auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    expect(illumination != nullptr, "fixture must import the real illumination integrator");
    illumination->set_denoise_enabled(false);
    pipeline.frame_buffer()->set_enable_accumulation(false);
    pipeline.prepare();
    ocarina::MemoryStats::instance().foreach_buffer_info([](auto data) {
        expect(data.name.rfind("Sharc::", 0) != 0 || data.size == 0u,
               "ReSTIR must not allocate the retired SHARC transport cache");
    });
    pipeline.frame_buffer()->prepare_view_texture();
    pipeline.upload_data();
    expect(!pipeline.frame_buffer()->enable_accumulation(), "raw estimator test must not accumulate frames");
    expect(!illumination->denoiser() || !illumination->denoiser()->enabled(),
           "raw estimator test must not invoke a denoiser");
    expect(pipeline.scene().instances().size() == expected_instances, "fixture must import each requested receiver");
    if (const char* reason = illumination->render_incompatibility()) throw std::runtime_error(reason);
    expect(pipeline.scene().light_manager().env_light() != nullptr,
           "fixture must contain an actual environment light");

    std::vector<vision::float4> pixels(pipeline.pixel_num());
    std::vector<vision::TriangleHit> hits(pixels.size());
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
                   .subview(0, hits.size()).download(hits.data());
        pipeline.stream() << vision::synchronize() << vision::commit();
        if (frame == 0u) {
            first_hits = hits;
            first_pixels = pixels;
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

int main(int argc, char **argv) {
    configure_headless_process();
    fs::path fixture;
    try {
        const bool principled_only = argc == 2 && std::string_view{argv[1]} == "--principled-stable-planes";
        expect(argc == 1 || principled_only, "usage: corona_vision_restir_estimator_tests [--principled-stable-planes]");
        const auto runtime_directory = fs::current_path();
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        fixture = fs::temp_directory_path() / ("corona_restir_estimators_" + std::to_string(suffix));
        fs::create_directories(fixture);
        std::cout << "fixture=" << fixture.string() << '\n';
        // Counterclockwise from the camera: geometric and shading normals +Z.
        // At z=-1 the full 45-degree camera frustum fits within this triangle.
        write_file(fixture / "receiver.obj",
                   "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0 0 1\nf 1//1 2//1 3//1\n");
        // Match the embedded engine bootstrap. Standalone hotfix registration
        // can leave ../bin ahead of the chosen runtime directory in this list.
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(runtime_directory);
        // As in the other Vision integration tests, the device outlives the
        // global kernel tools and all pipelines through process shutdown.
        static auto device = ocarina::RHIContext::instance().create_device();
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        vision::Global::instance().set_scene_path(fixture);
        const auto direct_pipeline = Corona::Tests::make_vision_test_pipeline(scene_description(true), fixture);
        expect(bool(direct_pipeline), "could not import direct-channel fixture");
        if (std::getenv("VISION_TEST_STORAGE_RESTORE_ONLY")) {
            direct_pipeline->prepare();
            check_stable_plane_storage_restore(*direct_pipeline);
            fs::remove(fixture / "receiver.obj");
            std::error_code cleanup_error; fs::remove(fixture, cleanup_error);
            return 0;
        }
        if (principled_only) {
            direct_pipeline->prepare();
            direct_pipeline->upload_data();
            check_principled_stable_planes(*direct_pipeline);
            fs::remove(fixture / "receiver.obj");

            std::error_code cleanup_error;
            fs::remove(fixture, cleanup_error);
            return 0;
        }
        const auto direct = render_channel(*direct_pipeline);
        check_stable_lobes(*direct_pipeline);
        std::cout << "direct_mean=" << direct.mean() << " direct_max=" << direct.max_abs << '\n';
        std::cout << "raw_static_primary_bary_delta=" << direct.max_primary_bary_delta << '\n';
        expect(direct.max_primary_bary_delta <= 1e-6f,
               "raw ReSTIR with a stationary pinhole camera must not shift the primary sample every frame");
        expect(direct.max_lighting_delta > 1e-4f,
               "stabilizing the raw primary ray must not freeze stochastic lighting samples");
        expect(direct.mean() > 0.05, "DI must illuminate the receiver under the white environment");

        const auto indirect_pipeline = Corona::Tests::make_vision_test_pipeline(scene_description(false), fixture);
        expect(bool(indirect_pipeline), "could not import indirect-channel fixture");
        const auto indirect = render_channel(*indirect_pipeline);
        std::cout << "indirect_mean=" << indirect.mean() << " indirect_max=" << indirect.max_abs
                  << " measured_hit_samples=" << indirect.samples << '\n';
        expect(indirect.max_abs <= 1e-6f,
               "GI must exclude an initial BSDF miss to the environment: this scene has zero indirect transport");
        check_primary_sampling_modes(*indirect_pipeline);
        const auto reused_pipeline = Corona::Tests::make_vision_test_pipeline(scene_description(false, true), fixture);
        expect(bool(reused_pipeline), "could not import the GI null-history fixture");
        const auto reused = render_channel(*reused_pipeline);
        std::cout << "reused_indirect_mean=" << reused.mean() << " reused_indirect_max=" << reused.max_abs << '\n';
        expect(reused.max_abs <= 1e-6f,
               "temporal and spatial reuse must preserve zero GI when every initial candidate is null");
        std::cout << "PASS: ReSTIR keeps initial environment lighting exclusively in DI\n";
        check_restir_gi_depth_one_continuation(*direct_pipeline);
        check_restir_reservoir_lifetimes(*reused_pipeline);

        // These large facing triangles exercise RTXPT GI Fill on secondary hits.
        // The rear triangle is behind the camera, so it contributes only through GI.
        write_file(fixture / "bounce.obj",
                   "v -4 -4 1\nv 0 4 1\nv 4 -4 1\nvn 0 0 -1\nf 1//1 2//1 3//1\n");
        auto bounce_scene = scene_description(false, true);
        auto bounce = bounce_scene["scene"]["shapes"][0];
        bounce["param"]["fn"] = "bounce.obj";
        bounce_scene["scene"]["shapes"].push_back(bounce);
        auto bounce_pipeline = Corona::Tests::make_vision_test_pipeline(bounce_scene, fixture);
        expect(bool(bounce_pipeline), "could not import the diffuse two-receiver GI fixture");
        const auto bounced = render_channel(*bounce_pipeline, 2u);
        std::cout << "two_plane_gi_mean=" << bounced.mean() << " measured_hit_samples=" << bounced.samples << '\n';
        expect(bounced.samples == 8u * 8u * 8u, "two-plane GI must complete eight frames of 8x8 receiver samples");
        // The known-good renderer produced a mean of 0.205 on this fixture.
        expect(bounced.mean() > 0.05, "RTXPT GI must return indirect energy from the second surface");

        fs::remove(fixture / "receiver.obj");
        fs::remove(fixture / "bounce.obj");
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
