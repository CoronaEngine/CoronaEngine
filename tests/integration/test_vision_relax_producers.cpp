#include "render_core/denoiser/ReLAX/relax.h"
#include "render_core/denoiser/ReLAX/utils.h"
#include "rhi/context.h"
#include "base/import/importer.h"
#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <chrono>
#include <cstring>

namespace {
using namespace vision;
void expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void check_material(Pipeline &pipeline) {
    std::cout << "surface/extension/plane/history bytes: " << sizeof(SurfaceData) << '/' << sizeof(SurfaceExtend)
              << '/' << sizeof(StablePlaneData) << '/' << sizeof(StablePlaneHistory) << '\n';
    expect(sizeof(SurfaceData) <= 160u && sizeof(SurfaceExtend) <= 48u &&
           sizeof(StablePlaneData) <= 256u && sizeof(StablePlaneHistory) <= 96u,
           "full-precision stable-plane storage must not retain padded float3 payloads");
    pipeline.activate_global_context();
    StablePlaneData original{};
    original.surface.virtual_position = {16777216.f, -0.f, 1e-30f};
    original.surface.virtual_normal = {.123456791f, -.765432119f, 1.f};
    original.surface.virtual_geometric_normal = {-.25f, .5f, .75f};
    original.surface.denoiser_albedo = {65504.f, 100000.f, .0000001f};
    original.surface.depth_position = {-123456.75f, .03125f, 19.75f};
    original.extension.throughput = {.123456791f, 1e-30f, 100000.f};
    original.extension.view_pos = {-0.f, -100000.f, 3.1415927f};
    original.extension.final_direction = {.3f, -.7f, .1f};
    original.depth_position = {17.25f, -.123456791f, 65504.f};
    auto packed_input = pipeline.device().create_buffer<StablePlaneData>(1u, "compact FP32 input");
    auto packed_output = pipeline.device().create_buffer<StablePlaneData>(1u, "compact FP32 output");
    auto packed_history = pipeline.device().create_buffer<StablePlaneHistory>(1u, "compact FP32 history");
    Kernel roundtrip = [](BufferVar<StablePlaneData> in, BufferVar<StablePlaneData> out, BufferVar<StablePlaneHistory> history) {
        auto p = in.read(0u);
        for (auto *v : {&p.surface.virtual_position, &p.surface.virtual_normal, &p.surface.virtual_geometric_normal,
                       &p.surface.denoiser_albedo, &p.surface.depth_position, &p.extension.throughput,
                       &p.extension.view_pos, &p.extension.final_direction, &p.depth_position}) {
            v->set(v->as_vec3());
        }
        out.write(0u, p); history.write(0u, p->history());
    };
    auto packed_shader = pipeline.device().compile(roundtrip, "stable-plane-lossless-FP32-storage");
    StablePlaneData restored{}; StablePlaneHistory previous{};
    pipeline.stream() << packed_input.upload(&original) << packed_shader(packed_input, packed_output, packed_history).dispatch(1u)
        << packed_output.download(&restored) << packed_history.download(&previous) << synchronize() << commit();
    auto same_bits = [&](const auto &a, const auto &b) {
        expect(std::memcmp(a.data(), b.data(), sizeof(a)) == 0, "compact FP32 GPU storage must preserve every scalar bit");
    };
    same_bits(restored.surface.virtual_position, original.surface.virtual_position);
    same_bits(restored.surface.virtual_normal, original.surface.virtual_normal);
    same_bits(restored.surface.virtual_geometric_normal, original.surface.virtual_geometric_normal);
    same_bits(restored.surface.denoiser_albedo, original.surface.denoiser_albedo);
    same_bits(restored.surface.depth_position, original.surface.depth_position);
    same_bits(restored.extension.throughput, original.extension.throughput);
    same_bits(restored.extension.view_pos, original.extension.view_pos);
    same_bits(restored.extension.final_direction, original.extension.final_direction);
    same_bits(restored.depth_position, original.depth_position);
    same_bits(previous.surface.virtual_position, original.surface.virtual_position);
    same_bits(previous.surface.virtual_geometric_normal, original.surface.virtual_geometric_normal);
    same_bits(previous.depth_position, original.depth_position);
    auto &sampler = pipeline.renderer().sampler();
    SamplerDesc seed_desc{"independent"}; seed_desc.set_value("seed", 104729u);
    auto independent = Node::create_shared<Sampler>(seed_desc);
    Kernel seed_test = [&](BufferVar<float2> result) {
        sampler->load_data(); independent->load_data();
        sampler->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
        independent->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
        result.write(dispatch_id(), make_float2(sampler->next_1d(), independent->next_1d()));
    };
    auto seed_shader = pipeline.device().compile(seed_test, "RELAX-independent-reference-seed");
    auto seeds = pipeline.device().create_buffer<float2>(16u);
    std::array<float2, 16> samples{};
    pipeline.stream() << seed_shader(seeds).dispatch(16u) << seeds.download(samples.data()) << synchronize() << commit();
    uint different = 0u;
    for (auto value : samples) different += std::abs(value.x - value.y) > 1e-6f;
    expect(different > 12u, "independent PT reference seeds must generate different sample streams");
    for (bool individual : {false, true}) {
        MaterialRegistry::instance().individual_ns() = individual;
        Kernel kernel = [&](BufferVar<float4> output) {
            Env::instance().clear_global_vars(); sampler->load_data();
            sampler->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
            SampledWavelengths swl{3u};
            Interaction it{false};
            it.shading.x = make_float3(1.f, 0.f, 0.f); it.shading.y = make_float3(0.f, 1.f, 0.f);
            it.shading.z = make_float3(0.f, 0.f, 1.f); it.ng = it.shading.z;
            auto diffuse = make_shared<DiffuseLobe>(SampledSpectrum{make_float3(.7f, .2f, .1f)}, swl);
            auto reflection = make_shared<PureReflectionLobe>(make_shared<FresnelConstant>(swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{make_float3(.1f, .4f, .8f)}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.3f), true)), SurfaceData::Glossy);
            diffuse->set_shading_frame(it.shading); reflection->set_shading_frame(it.shading);
            MaterialEvaluator evaluator{it, swl};
            evaluator.link(LobeSet::create_mix(Float{.6f}, reflection, diffuse));
            Float3 wo = make_float3(0.f, 0.f, 1.f), wi = normalize(make_float3(.2f, .3f, 1.f));
            auto expected_d = diffuse->evaluate(wo, wi, MaterialEvalMode::All, BxDFFlag::All, TransportMode::Radiance).f.vec3() * .6f;
            auto expected_s = reflection->evaluate(wo, wi, MaterialEvalMode::All, BxDFFlag::All, TransportMode::Radiance).f.vec3() * .4f;
            auto total = evaluator.evaluate(wo, wi).f.vec3();
            auto fraction = evaluator.specular_fraction_rgb(wo, wi);
            Bool ok = all(abs(total * fraction - expected_s) < 1e-5f) && all(abs(total * (1.f - fraction) - expected_d) < 1e-5f);
            auto d = evaluator.sample_lobe(wo, sampler, false);
            auto s = evaluator.sample_lobe(wo, sampler, true);
            ok &= d.valid() && !BxDFFlag::is_glossy(d.eval.flags) && !BxDFFlag::is_specular(d.eval.flags);
            ok &= !s.valid() || BxDFFlag::is_glossy(s.eval.flags) || BxDFFlag::is_specular(s.eval.flags);
            MaterialEvaluator mirror{it, swl};
            auto mirror_lobe = make_unique<PureReflectionLobe>(make_shared<FresnelConstant>(swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{swl, .7f}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.001f), true)), SurfaceData::NearSpec);
            mirror_lobe->set_shading_frame(it.shading); mirror.link(std::move(mirror_lobe));
            auto mirror_fraction = mirror.specular_fraction_rgb(wo, wo);
            auto mirror_sample = mirror.sample_lobe(wo, sampler, true);
            ok &= all(abs(mirror_fraction - make_float3(1.f)) < 1e-5f) && mirror_sample.valid();
            SampledSpectrum mirror_d{swl.dimension()}, mirror_s{swl.dimension()};
            mirror.denoiser_factors(wo, mirror_d, mirror_s);
            ok &= mirror_d.max() == 0.f && mirror_s.max() > .1f;
            auto tilted_frame = it.shading;
            tilted_frame.update(normalize(make_float3(1.f, 0.f, .2f)));
            auto tilted_lobe = make_unique<PureReflectionLobe>(make_shared<FresnelDielectric>(SampledSpectrum{swl, 1.5f}, swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{swl, 1.f}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.1f), true)), SurfaceData::Glossy);
            tilted_lobe->set_shading_frame(tilted_frame);
            auto expected_factor = tilted_lobe->albedo(individual ? abs(dot(tilted_frame.normal(), wo)) : Float{1.f});
            MaterialEvaluator tilted{it, swl}; tilted.link(std::move(tilted_lobe));
            SampledSpectrum tilted_d{swl.dimension()}, tilted_s{swl.dimension()};
            tilted.denoiser_factors(wo, tilted_d, tilted_s);
            ok &= all(abs(tilted_s.vec3() - expected_factor.vec3()) < 1e-5f);
            output.write(dispatch_id(), make_float4(fraction, select(ok, 1.f, 0.f)));
        };
        auto shader = pipeline.device().compile(kernel, "denoiser_material_contract");
        auto buffer = pipeline.device().create_buffer<float4>(128);
        std::array<float4, 128> result;
        pipeline.stream() << shader(buffer).dispatch(128u) << buffer.download(result.data()) << synchronize() << commit();
        for (auto value : result) expect(value.w == 1.f && std::abs(value.x - value.z) > .1f,
            "colored BSDF lobes must split per channel and guide sampling must select the requested class");
    }
    std::cout << "PASS: colored BSDF energy and independent lobe guide sampling in both normal modes\n";
}

void check_compound_material(Pipeline &pipeline) {
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto &sampler = pipeline.renderer().sampler();
    Kernel kernel = [&](BufferVar<float4> output) {
        Env::instance().clear_global_vars(); sampler->load_data();
        sampler->set_seed(dispatch_idx().xy(), Uint{73u}, Dimension::PathTracing);
        SampledWavelengths swl{3u};
        Interaction it{false};
        it.shading.x = make_float3(1.f, 0.f, 0.f); it.shading.y = make_float3(0.f, 1.f, 0.f);
        it.shading.z = make_float3(0.f, 0.f, 1.f); it.ng = it.shading.z; it.wo = it.ng;
        pipeline.scene().materials().dispatch(Uint{0u}, [&](const Material *material) {
            auto evaluator = material->create_evaluator(it, swl);
            auto guide = evaluator.sample_lobe(it.wo, sampler, false);
            SampledSpectrum dn{swl.dimension()}, sn{swl.dimension()}, dg{swl.dimension()}, sg{swl.dimension()};
            evaluator.denoiser_factors(it.wo, dn, sn);
            evaluator.denoiser_factors(normalize(make_float3(1.f, 0.f, .1f)), dg, sg);
            output.write(dispatch_id(), make_float4(select(guide.valid(), 1.f, 0.f), sn.average(), sg.average(), dg.average() / dn.average()));
        });
    };
    auto shader = pipeline.device().compile(kernel, "denoiser_compound_material_contract");
    auto buffer = pipeline.device().create_buffer<float4>(128);
    std::array<float4, 128> result;
    pipeline.stream() << shader(buffer).dispatch(128u) << buffer.download(result.data()) << synchronize() << commit();
    for (auto value : result) {
        expect(value.x == 1.f, "smooth compound materials must retain diffuse guide sampling at exact normal incidence");
        expect(value.z > value.y + .1f && value.w > 0.f && value.w < .8f,
            "compound demodulation factors must include directional Fresnel and diffuse attenuation");
    }
    std::cout << "PASS: smooth compound guide support and directional material factors\n";
}

void check_scene_signals(bool restir, bool lifecycle = false) {
    namespace fs = std::filesystem;
    const auto fixture = fs::temp_directory_path() / ("corona_denoiser_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(fixture);
    std::ofstream(fixture / "receiver.obj") << "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    auto scene = DataWrap::parse(R"({
      "scene": {
        "camera":{"type":"thin_lens","param":{"fov_y":45,"lens_radius":0,"filter":{"type":"box"},"transform":{"type":"look_at","param":{"position":[0,0,0],"target_pos":[0,0,-1],"up":[0,1,0]}}}},
        "materials":[{"type":"substrate","name":"receiver","param":{"color":[0.7,0.2,0.1],"spec":[0.1,0.4,0.8],"roughness":0.3}}],
        "shapes":[{"type":"model","param":{"fn":"receiver.obj","material":"receiver","normalize_to_unit_bounds":false}}],
        "lights":[{"type":"spherical","param":{"color":{"channels":"xyz","node":{"type":"number","param":{"value":[1,1,1]}}},"scale":1}}]
      },
      "render":{"sampler":{"type":"independent","param":{"spp":1}},"light_sampler":{"type":"uniform"},
        "integrator":{"type":"pt","param":{"max_depth":3,"min_depth":3,"denoiser":{"type":"ReLAX"},
          "direct":{"M_light":8,"M_bsdf":1,"temporal":{"open":false},"spatial":{"open":false}},
          "indirect":{"temporal":{"open":false},"spatial":{"open":false}}}}},
      "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[16,16]}}}},
      "output":{"spp":0,"save_exit":false,"denoise":false,"fn":"unused.exr"}
    })");
    std::vector<SP<Pipeline>> pipelines;
    const auto receiver_materials = scene["scene"]["materials"];
    const auto receiver_shapes = scene["scene"]["shapes"];
    std::ofstream(fixture / "back.obj") << "v -4 -4 -3\nv 4 -4 -3\nv 0 4 -3\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    std::ofstream(fixture / "front.obj") << "v -4 -4 1\nv 0 4 1\nv 4 -4 1\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
    std::ofstream(fixture / "rear.obj") << "v -4 -4 -1.2\nv 0 4 -1.2\nv 4 -4 -1.2\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
    for (uint scenario = 0; scenario < 12u; ++scenario) {
        const bool pt = scenario == 0u || scenario >= 7u, stable = scenario >= 3u && !pt;
        if (pt == restir) continue;
        if (lifecycle && scenario != 5u) continue;
        scene["scene"]["materials"] = receiver_materials;
        scene["scene"]["shapes"] = receiver_shapes;
        if (scenario >= 10u) {
            auto &material = scene["scene"]["materials"][0];
            if (scenario == 11u) {
                material = DataWrap::parse(R"({"type":"plastic","name":"receiver","param":{"color":[0.7,0.2,0.1],"ior":1.5}})");
            }
            material["param"]["roughness"] = .0001f;
            material["param"]["remapping_roughness"] = false;
        }
        if (scenario == 5u || scenario == 6u || scenario == 9u) {
            scene["scene"]["materials"] = DataWrap::parse(R"([
                {"name":"glass","type":"glass","param":{"material_name":"BK7","roughness":0.001,"remapping_roughness":false}},
                {"name":"red","type":"diffuse","param":{"color":[0.8,0.1,0.1]}},
                {"name":"blue","type":"diffuse","param":{"color":[0.1,0.1,0.8]}}])");
            scene["scene"]["shapes"] = DataWrap::parse(R"([
                {"type":"model","param":{"fn":"receiver.obj","material":"glass","normalize_to_unit_bounds":false}},
                {"type":"model","param":{"fn":"back.obj","material":"red","normalize_to_unit_bounds":false}},
                {"type":"model","param":{"fn":"front.obj","material":"blue","normalize_to_unit_bounds":false}}])");
            if (scenario == 9u) scene["scene"]["shapes"].push_back(DataWrap::parse(
                R"({"type":"model","param":{"fn":"rear.obj","material":"glass","normalize_to_unit_bounds":false}})"));
        }
        auto &integrator = scene["render"]["integrator"];
        integrator["type"] = pt ? "pt" : "rt";
        integrator["param"]["direct"]["stable_planes"] = stable;
        const bool direct_on = scenario != 2u && scenario != 6u;
        const float di_factor = !pt && (scenario == 3u || scenario == 4u) ? .25f : 1.f;
        const float gi_factor = !pt && scenario >= 2u ? .75f : 1.f;
        integrator["param"]["direct"]["factor"] = di_factor;
        integrator["param"]["indirect"]["factor"] = gi_factor;
        integrator["param"]["max_depth"] = scenario == 7u ? 1u : 4u;
        integrator["param"]["mis_mode"] = scenario >= 8u ? 2u : 0u;
        integrator["param"]["direct"]["open"] = direct_on;
        integrator["param"]["indirect"]["open"] = scenario != 4u;
        { std::ofstream file(fixture / "scene.json"); file << scene.dump(2); }
        Global::instance().set_scene_path(fixture);
        auto pipeline = Importer::import_scene(fixture / "scene.json"); pipelines.push_back(pipeline);
        auto *illumination = dynamic_cast<IlluminationIntegrator *>(pipeline->renderer().integrator().get());
        expect(illumination && illumination->denoiser()->requires_lobe_signals(), "explicit RELAX must load the real plugin");
        illumination->set_denoise_enabled(false);
        pipeline->frame_buffer()->set_enable_accumulation(false);
        pipeline->prepare(); pipeline->frame_buffer()->prepare_view_texture();
        pipeline->upload_data();
        if (pt) check_material(*pipeline);
        if (scenario >= 10u) check_compound_material(*pipeline);
        auto &fb = *pipeline->frame_buffer();
        expect(fb.denoiser_diffuse().size() >= pipeline->pixel_num(),
               "selected RELAX requires the producer to allocate explicit lobe signals");
        const uint count = pipeline->pixel_num(), layers = stable ? 3u : 1u;
        std::vector<float4> pixels(count), d(count * 3u), s(d.size()), b(d.size()), emission(count);
        for (uint frame = 0; frame < 4u; ++frame) {
            pipeline->upload_data(); pipeline->display(1.0 / 60.0);
            expect(illumination->cur_denoise_time() == 0.0, "disabled denoising must report zero time");
            pipeline->stream() << fb.rt_buffer().device_buffer().download(pixels.data())
                << fb.denoiser_diffuse().view().download(d.data()) << fb.denoiser_specular().view().download(s.data())
                << fb.denoiser_bypass().view().download(b.data());
            if (stable) pipeline->stream() << fb.stable_radiance().view().download(emission.data());
            pipeline->stream() << synchronize() << commit();
            if (scenario == 8u) {
                float diffuse_sum = 0.f, specular_sum = 0.f;
                for (uint y = 4u; y < 12u; ++y) for (uint x = 4u; x < 12u; ++x) {
                    diffuse_sum += d[y * 16u + x].x; specular_sum += s[y * 16u + x].z;
                }
                expect(diffuse_sum > .01f && specular_sum > .01f,
                    "BSDF-sampled environment from a mixed material must retain both primary lobe contributions");
            }
            if (scenario == 9u) {
                uint continued = 0u;
                for (uint y = 4u; y < 12u; ++y) for (uint x = 4u; x < 12u; ++x)
                    continued += s[y * 16u + x].w > 1.5f;
                expect(continued > 45u, "glass hit distance must continue through the rear interface to the distant target");
            }
            for (uint i = 0; i < count; ++i) for (uint c = 0; c < 3u; ++c) {
                float sum = stable && direct_on ? emission[i][c] * di_factor : 0.f;
                for (uint layer = 0; layer < layers; ++layer) {
                    const uint j = layer * count + i;
                    sum += d[j][c] + s[j][c] + b[j][c];
                }
                expect(std::isfinite(sum) && std::abs(sum - pixels[i][c]) < 1e-4f * std::max(1.f, std::abs(pixels[i][c])),
                    "raw per-lobe energy plus bypass must equal the original estimator, including DI/GI switches and stable layers");
            }
        }
        illumination->set_denoise_enabled(true);
        pipeline->output_desc().denoise = true;
        for (uint frame = 0; frame < 8u; ++frame) {
            pipeline->upload_data(); pipeline->display(1.0 / 60.0);
            const double denoise_ms = illumination->cur_denoise_time();
            expect(std::isfinite(denoise_ms) && denoise_ms > 0.0 && denoise_ms <= pipeline->cur_render_time(),
                "completed denoising must report a finite elapsed time within the rendered frame");
        }
        std::cout << "denoise ms=" << illumination->cur_denoise_time() << ", render ms=" << pipeline->cur_render_time()
                  << ", synchronized stage profile=" << illumination->cur_stage_profile().enabled << '\n';
        pipeline->stream() << fb.rt_buffer().device_buffer().download(pixels.data()) << synchronize() << commit();
        for (auto pixel : pixels) expect(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z),
            "enabling the actual RELAX plugin after raw frames must produce finite colors");
        expect(illumination->denoiser()->enabled() && illumination->denoiser()->has_prepared_resources(),
            "scene evaluation must execute the selected RELAX filter after runtime enable");
        if (lifecycle) {
            auto draw = [&] {
                pipeline->upload_data(); pipeline->display(1.0 / 60.0);
                expect(illumination->denoiser()->enabled() ? illumination->cur_denoise_time() > 0.0
                                                        : illumination->cur_denoise_time() == 0.0,
                    "denoise time must follow runtime enable, resize and layer transitions without stale values");
                std::vector<float4> result(pipeline->pixel_num());
                pipeline->stream() << fb.rt_buffer().device_buffer().download(result.data()) << synchronize() << commit();
                for (auto p : result) expect(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                    "RELAX lifecycle transition must preserve finite output");
            };
            for (auto resolution : {make_uint2(127u, 73u), make_uint2(1u), make_uint2(16u)}) {
                illumination->set_denoise_enabled(false);
                pipeline->output_desc().denoise = false;
                pipeline->change_resolution(resolution); draw();
                illumination->set_denoise_enabled(true); pipeline->output_desc().denoise = true; draw(); draw();
                for (bool layered : {false, true}) { illumination->set_stable_planes_enabled(layered); draw(); draw(); }
            }
            auto &camera = pipeline->scene().sensor();
            camera->set_position(camera->position() + make_float3(.1f, 0.f, 0.f));
            camera->set_fov_y(50.f); camera->update_device_data(); draw();
            pipeline->invalidate(); draw(); pipeline->compile(); draw();
            expect(illumination->denoiser()->has_prepared_resources(), "RELAX must keep reusable resources after lifecycle transitions");
        }
        std::cout << "PASS: producer energy, runtime enable, scenario=" << scenario << '\n';
    }
    fs::remove_all(fixture);
}


}
int main(int argc, char **argv) {
    try {
        using namespace vision;
        Env::set_valid_check(false);
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        static auto device = RHIContext::instance().create_device();
        device.init_rtx();
        Global::instance().set_device(&device);
        const std::string_view group = argc > 1 ? argv[1] : "pt";
        check_scene_signals(group != "pt", group == "lifecycle");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
