#pragma once

#include "rtxpt_gi_denoiser_diagnostic.h"
#include "di_radiance.h"
#include "render_core/integrator/restir_di/reservoir.h"
#include "base/mgr/global.h"
#include "math/util.h"
#include <cstdlib>

namespace vision {
struct RtxptGIShadowDiagnostic {
    array<float4, 14> values{};
    array<uint4, 4> ids{};
};
struct RtxptGIEnvironmentDiagnostic {
    array<float4, 6> values{};
    uint4 identity{};
};
static_assert(sizeof(RtxptGIShadowDiagnostic) == 288u);
static_assert(sizeof(RtxptGIEnvironmentDiagnostic) == 112u);
}
OC_STRUCT(vision, RtxptGIShadowDiagnostic, values, ids) {};
OC_STRUCT(vision, RtxptGIEnvironmentDiagnostic, values, identity) {};

namespace vision::rtxpt_gi {
// Opt-in observation of already finalized ReSTIR DI samples. Only diagnostic
// buffers are written; the existing reservoirs, rays and radiance stay read-only.
inline void download_shadow_diagnostics(Device &device, Stream &stream, FrameBuffer &fb,
                                        uint frame, uint2 size, const DIRadiance &direct,
                                        const std::filesystem::path &directory) {
    const char *enabled = std::getenv("VISION_GI_SHADOW_DIAGNOSTIC");
    if (!enabled || enabled[0] != '1') return;
    auto &diagnostic_geometry = Toolkit::scene().geometry();
    Global::SceneGpuContextScope scene_gpu_context{diagnostic_geometry.bindless_array(),
                                                   diagnostic_geometry.gpu_resource()->device()};
    OC_INFO("GI shadow diagnostic: host mapping begin");
    if (Toolkit::spectrum()->impl_type() != "srgb")
        throw std::runtime_error("GI shadow diagnostic requires the active ReSTIR DI RGB spectrum");

    vector<uint4> primitives;
    const auto &instances = Toolkit::scene().instances();
    for (uint instance = 0u; instance < instances.size(); ++instance) {
        const auto &shape = instances[instance];
        if (!shape->has_emission()) continue;
        const auto *light = shape->emission().get();
        const auto *color = light->input_slot("color");
        if (!color || !color->node()) throw std::runtime_error("GI shadow diagnostic emitter has no public color slot");
        const auto areas = shape->surface_areas();
        for (uint primitive = 0u; primitive < areas.size(); ++primitive)
            primitives.push_back(make_uint4(instance, primitive, shape->handle().light_id,
                                             uint(color->node()->is_uniform())));
    }
    const auto *environment = Toolkit::scene().light_manager().env_light();
    // Spherical environment power is a positive scene-radius factor times its
    // average radiance. The public query gives the same nonzero-light test.
    const bool has_environment = environment && any(environment->power() != make_float3(0.f));
    const uint environment_index = has_environment ? uint(primitives.size()) : InvalidUI32;
    if (has_environment) primitives.push_back(make_uint4(InvalidUI32));
    if (primitives.empty()) throw std::runtime_error("GI shadow diagnostic found no ReSTIR DI lights");
    OC_INFO_FORMAT("GI shadow diagnostic: {} light entries; recording witness kernel", primitives.size());

    // These exact names/layouts are already registered by the active ReSTIR DI
    // instance. FrameBuffer returns their existing allocations without writes.
    const auto reservoir_storage = fb.prepare_screen_storage("ReSTIR DI packed reservoir", make_uint2(16u),
                                                             16u * 16u * 3u * sizeof(restir_di::Reservoir));
    const auto position_storage = fb.prepare_screen_storage("ReSTIR DI surface position and depth", make_uint2(1u),
                                                            2u * sizeof(uint4));
    const auto normal_storage = fb.prepare_screen_storage("ReSTIR DI surface normals", make_uint2(1u),
                                                          2u * sizeof(uint2));
    Buffer<restir_di::Reservoir> reservoirs{reservoir_storage.view_as<restir_di::Reservoir>()};
    Buffer<uint4> positions{position_storage.view_as<uint4>()};
    Buffer<uint2> normals{normal_storage.view_as<uint2>()};
    const auto indices = native_reblur_diagnostic_indices(size);
    const uint count = uint(indices.size()), pixels = size.x * size.y;
    auto index_buffer = device.create_buffer<uint>(count);
    auto primitive_buffer = device.create_buffer<uint4>(primitives.size());
    auto output = device.create_buffer<RtxptGIShadowDiagnostic>(count);
    const uint slice = 1u + (frame & 1u);
    auto &geo = Toolkit::geometry();
    auto &spec = Toolkit::spectrum();
    auto &sampler = Toolkit::renderer().sampler();
    auto &lights = Toolkit::renderer().light_sampler();
    Kernel kernel = [&] {
        Function::current()->require_ray_query_compute_workgroups();
        Uint sample_index = dispatch_id();
        Uint index = index_buffer.read(sample_index);
        Uint2 pixel = make_uint2(index % size.x, index / size.x);
        RtxptGIShadowDiagnosticVar row;
        for (uint i = 0u; i < 14u; ++i) row.values[i] = make_float4(0.f);
        for (uint i = 0u; i < 4u; ++i) row.ids[i] = make_uint4(InvalidUI32);
        row.values[7u] = direct.read_radiance(index);
        Uint flags = 0u;
        Uint pt_instance = InvalidUI32, pt_primitive = InvalidUI32;
        auto hit = fb.cur_visibility_buffer_var(frame).read(index);
        auto reservoir = reservoirs.read(restir_di::reservoir_index(size, pixel, Uint{slice}), false);
        row.ids[0u] = make_uint4(reservoir.light_index, InvalidUI32, InvalidUI32, reservoir.packed_uv);
        row.ids[3u] = make_uint4(hit.prim_id, InvalidUI32, frame, slice);
        $if(restir_di::valid_reservoir(reservoir)) { flags |= 2u; };
        $if(reservoir.light_index == environment_index && environment_index != InvalidUI32) { flags |= 4u; };
        $if(hit->is_hit()) {
            flags |= 1u;
            auto ray = fb.rays().read(index)->to_ray_state().ray;
            auto it = geo.compute_surface_interaction(hit, ray);
            row.ids[3u] = make_uint4(hit.prim_id, it.material_id(), frame, slice);
            row.values[0u] = make_float4(it.pos, 0.f);
            row.values[1u] = make_float4(it.ng, 0.f);
            row.values[2u] = make_float4(make_float3(0.f), reservoir.weight);
            row.values[3u] = make_float4(make_float3(0.f), reservoir.target);
            row.values[4u] = make_float4(make_float3(0.f), reservoir.count);
            $if(restir_di::valid_reservoir(reservoir) && reservoir.light_index < uint(primitives.size()) &&
                reservoir.light_index != environment_index) {
                const Uint4 primitive = primitive_buffer.read(reservoir.light_index);
                row.ids[0u] = make_uint4(reservoir.light_index, primitive.x, primitive.y, reservoir.packed_uv);
                Float2 uv = reservoir->uv();
                uv = select(uv.x + uv.y <= 1.f, uv, 1.f - uv);
                const Float2 bary = make_float2(1.f - uv.x - uv.y, uv.x);
                const auto p0 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(1.f, 0.f));
                const auto p1 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f, 1.f));
                const auto p2 = geo.compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f));
                const Bool cacheable = primitive.w != 0u && p0.PDF_pos > 0.f && restir_di::finite(p0.PDF_pos) &&
                                       all(p0.ng == p1.ng) && all(p0.ng == p2.ng);
                auto point = geo.compute_light_eval_context(primitive.x, primitive.y, bary);
                $if(cacheable) {
                    point.pos = triangle_lerp(bary, p0.pos, p1.pos, p2.pos);
                    point.ng = p0.ng;
                    point.PDF_pos = p0.PDF_pos;
                };
                // RGB sample_wavelength does not consume sampler state.
                const auto swl = spec->sample_wavelength(sampler);
                Float3 emission = make_float3(0.f);
                lights->dispatch_light(primitive.z, [&](const Light *light) {
                    if (!light->match(LightType::Area)) return;
                    $if(primitive.w != 0u) {
                        emission = light->input_slot("color")->eval_illumination_spectrum(p0.uv, swl).sample.vec3() * light->scale();
                    } $else {
                        LightSampleContext front;
                        front.pos = point.pos + point.ng;
                        emission = light->Le(front, point, swl).vec3();
                    };
                });
                const Uint surface_index = (frame & 1u) * pixels + index;
                const Float3 receiver = as<float4>(positions.read(surface_index, false)).xyz();
                const Float3 receiver_ng = restir_di::unpack_normal(normals.read(surface_index, false).y);
                const Float3 segment = point.pos - receiver;
                const Float distance = length(segment);
                const Float3 wi = segment / distance;
                const Float pdf = vision::PDF_wi(point.PDF_pos, point.ng, receiver - point.pos);
                row.values[0u] = make_float4(it.pos, distance);
                row.values[1u] = make_float4(it.ng, pdf);
                row.values[2u] = make_float4(point.pos, reservoir.weight);
                row.values[3u] = make_float4(point.ng, reservoir.target);
                row.values[4u] = make_float4(emission, reservoir.count);
                $if(dot(segment, point.ng) < 0.f && distance > restir_di::ray_epsilon &&
                    pdf > 0.f && restir_di::finite(pdf)) {
                    flags |= 8u;
                    auto old_ray = it.spawn_ray(wi, max(0.f, distance - 1e-4f));
                    const Float3 reuse_origin = Interaction::custom_offset_ray_origin(receiver, receiver_ng, wi);
                    const Float3 reuse_segment = wi * distance + (receiver - reuse_origin);
                    const Float reuse_distance = length(reuse_segment);
                    auto reuse_ray = restir_di::make_light_ray(reuse_origin, reuse_segment / reuse_distance,
                                                               Float{0.f}, reuse_distance - restir_di::ray_epsilon);
                    auto pt_ray = it.spawn_ray_to(point.pos);
                    Float3 evaluated = make_float3(0.f);
                    Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                        auto bsdf = material->create_evaluator(it, swl);
                        evaluated = bsdf.evaluate(it.wo, wi, MaterialEvalMode::F).f.vec3();
                    });
                    row.values[5u] = make_float4(evaluated, 1.f);
                    row.values[6u] = make_float4(evaluated * (emission / pdf) * reservoir.weight, 1.f);
                    Float old_t = 0.f, reuse_t = 0.f, pt_t = 0.f;
                    auto old_hit = geo.accel().trace_closest(old_ray, old_t);
                    auto reuse_hit = geo.accel().trace_closest(reuse_ray, reuse_t);
                    auto pt_hit = geo.accel().trace_closest(pt_ray, pt_t);
                    $if(geo.trace_occlusion(old_ray)) { flags |= 16u; };
                    $if(geo.trace_occlusion(reuse_ray)) { flags |= 32u; };
                    $if(geo.trace_occlusion(pt_ray)) { flags |= 64u; };
                    $if(old_hit->is_hit() && old_hit.inst_id == primitive.x) { flags |= 128u;
                        $if(old_hit.prim_id == primitive.y) { flags |= 256u; };
                    };
                    $if(reuse_hit->is_hit() && reuse_hit.inst_id == primitive.x) { flags |= 512u;
                        $if(reuse_hit.prim_id == primitive.y) { flags |= 1024u; };
                    };
                    $if(pt_hit->is_hit() && pt_hit.inst_id == primitive.x) { flags |= 2048u;
                        $if(pt_hit.prim_id == primitive.y) { flags |= 4096u; };
                    };
                    row.values[8u] = make_float4(old_ray.org_min.xyz(), old_ray.dir_max.w);
                    row.values[9u] = make_float4(old_ray.dir_max.xyz(), old_t);
                    row.values[10u] = make_float4(reuse_ray.org_min.xyz(), reuse_ray.dir_max.w);
                    row.values[11u] = make_float4(reuse_ray.dir_max.xyz(), reuse_t);
                    row.values[12u] = make_float4(pt_ray.org_min.xyz(), pt_ray.dir_max.w);
                    row.values[13u] = make_float4(pt_ray.dir_max.xyz(), pt_t);
                    row.ids[1u] = make_uint4(old_hit.inst_id, old_hit.prim_id, reuse_hit.inst_id, reuse_hit.prim_id);
                    pt_instance = pt_hit.inst_id; pt_primitive = pt_hit.prim_id;
                };
            };
        };
        row.ids[2u] = make_uint4(pt_instance, pt_primitive, flags, hit.inst_id);
        output.write(sample_index, row);
    };
    kernel.function()->set_block_dim(64u, 1u, 1u);
    OC_INFO("GI shadow diagnostic: witness kernel recorded");
    auto shader = device.compile(kernel, "RTXPT GI read-only ReSTIR DI shadow witness");
    vector<RtxptGIShadowDiagnostic> host(count);
    stream << index_buffer.upload(indices.data()) << primitive_buffer.upload(primitives.data())
           << shader().dispatch(count) << output.download(host.data()) << synchronize() << ocarina::commit();
    const auto write = [&](const char *name, const auto &data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(data.data()), data.size() * sizeof(data[0]));
        if (!file) throw std::runtime_error("GI shadow diagnostic write failed");
    };
    write("restir_di_shadow.indices.u32", indices);
    write("restir_di_shadow.records.bin", host);
    write("restir_di_shadow.light_primitives.u32", primitives);
    std::ofstream metadata(directory / "restir_di_shadow.json");
    metadata << "{\n\"version\":1,\"frame\":" << frame << ",\"width\":" << size.x << ",\"height\":" << size.y
        << ",\"count\":" << count << ",\"stride\":288,\"layout\":\"AOS: float4 values[14], uint4 ids[4]\","
        << "\"environment_index\":" << environment_index << ",\"slice\":" << slice << ",\n"
        << "\"values\":[\"receiver_distance\",\"geometric_normal_conditional_pdf\",\"light_point_weight\","
           "\"light_normal_target\",\"emission_count\",\"bsdf_f_valid\",\"unoccluded_di_valid\",\"raw_di\","
           "\"old_origin_tmax\",\"old_direction_hit_t\",\"reuse_origin_tmax\",\"reuse_direction_hit_t\","
           "\"pt_origin_tmax\",\"pt_direction_hit_t\"],\n"
        << "\"ids\":[\"light_index,target_instance,target_primitive,packed_uv\","
           "\"old_instance,old_primitive,reuse_instance,reuse_primitive\",\"pt_instance,pt_primitive,flags,receiver_instance\","
           "\"receiver_primitive,material,frame,slice\"],\n"
        << "\"flags\":{\"receiver_hit\":1,\"valid_reservoir\":2,\"environment_skipped\":4,\"valid_finite_light\":8,"
           "\"old_blocked\":16,\"reuse_blocked\":32,\"pt_blocked\":64,\"old_target_instance\":128,"
           "\"old_target_primitive\":256,\"reuse_target_instance\":512,\"reuse_target_primitive\":1024,"
           "\"pt_target_instance\":2048,\"pt_target_primitive\":4096}}\n";
    if (!metadata) throw std::runtime_error("GI shadow diagnostic metadata write failed");
}

inline void copy_physical_di_snapshots(Stream &stream, uint frame) {
    const char *enabled = std::getenv("VISION_GI_NRD_SNAPSHOT_COPY");
    const char *root = std::getenv("REBLUR_EDSL_SNAPSHOT_DIR");
    if (!enabled || enabled[0] != '1' || !root || !root[0]) return;
    stream << synchronize() << ocarina::commit();
    const std::filesystem::path source(root), destination = source / "physical-di";
    std::filesystem::create_directories(destination);
    const std::string prefix = "f" + std::to_string(frame) + "_l0_p";
    uint copied = 0u;
    for (const auto &entry : std::filesystem::directory_iterator(source)) {
        if (!entry.is_regular_file() || !entry.path().filename().string().starts_with(prefix)) continue;
        std::filesystem::copy_file(entry.path(), destination / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing);
        ++copied;
    }
    std::ifstream input(source / "metadata.jsonl");
    std::ofstream metadata(destination / "metadata.jsonl");
    const std::string selected = "\"frame\":" + std::to_string(frame) + ",\"layer\":0,";
    uint metadata_rows = 0u;
    for (std::string line; std::getline(input, line);)
        if (line.find(selected) != std::string::npos) { metadata << line << '\n'; ++metadata_rows; }
    if (!copied || !metadata_rows || !metadata)
        throw std::runtime_error("GI physical DI snapshot copy found no selected-frame payload");
}

struct EnvironmentDiagnosticSample {
    Float3 value{make_float3(0.f)};
    // Incoming solar, visible supported solar, rays traced, rays blocked.
    Float4 events{make_float4(0.f)};
};

[[nodiscard]] inline SampledWavelengths environment_diagnostic_rgb_wavelengths() {
    SampledWavelengths swl{3u};
    for (uint i = 0u; i < 3u; ++i) {
        swl.set_lambda(i, rgb_spectrum_peak_wavelengths[i]);
        swl.set_pdf(i, 1.f);
    }
    return swl;
}

[[nodiscard]] inline EnvironmentDiagnosticSample environment_diagnostic_sample(
    const Interaction &it, const MaterialEvaluator &bsdf,
    const SampledWavelengths &swl, const Float2 &u, bool importance) {
    EnvironmentDiagnosticSample result;
    Float3 incoming = make_float3(0.f), endpoint = make_float3(0.f);
    Float pdf = 0.f;
    // Use the normal public dispatch boundary to decode the environment and
    // reset its texture-node cache for this lexical material/sample scope.
    Toolkit::renderer().light_sampler()->dispatch_environment([&](const Environment *environment) {
        if (importance) {
            const auto light = environment->sample_wi(it, u, swl);
            incoming = light.eval.L.vec3(); endpoint = light.p_light; pdf = light.eval.pdf;
        } else {
            const Float3 wi = restir_di::environment_direction(u);
            endpoint = it.pos + wi * Toolkit::scene().world_diameter();
            LightEvalContext point{endpoint};
            incoming = environment->evaluate_wi(it, point, swl, LightEvalMode::L).L.vec3();
            pdf = uniform_sphere_PDF();
        }
    });
    const Bool solar = luminance(incoming) > 1000.f;
    result.events.x = cast<float>(solar);
    $if(pdf > 0.f && restir_di::finite(pdf) && any(incoming > make_float3(0.f))) {
        const Float3 wi = normalize(endpoint - it.pos);
        const Float3 f = bsdf.evaluate(it.wo, wi, MaterialEvalMode::F).f.vec3();
        $if(any(f > make_float3(0.f))) {
            result.events.z = 1.f;
            const Bool blocked = Toolkit::geometry().occluded(it, endpoint);
            result.events.w = cast<float>(blocked);
            $if(!blocked) {
                result.value = ocarina::zero_if_nan_inf(f * incoming / pdf);
                result.events.y = cast<float>(solar);
            };
        };
    };
    return result;
}

// Independent direct-light oracle for an environment-only scene. The local tea
// and LCG state never touches a renderer sampler, reservoir or final image.
inline void download_environment_diagnostics(Device &device, Stream &stream, FrameBuffer &fb,
                                             uint frame, uint2 size,
                                             const std::filesystem::path &directory) {
    const char *enabled = std::getenv("VISION_GI_ENV_DIAGNOSTIC");
    if (!enabled || enabled[0] != '1') return;
    auto &geo = Toolkit::scene().geometry();
    Global::SceneGpuContextScope scene_gpu_context{geo.bindless_array(), geo.gpu_resource()->device()};
    for (const auto &shape : Toolkit::scene().instances())
        if (shape->has_emission()) throw std::runtime_error("GI environment diagnostic requires zero area emitters");
    const auto *environment = Toolkit::renderer().light_sampler()->env_light();
    if (!environment || !any(environment->power() != make_float3(0.f)) || Toolkit::spectrum()->impl_type() != "srgb")
        throw std::runtime_error("GI environment diagnostic requires one nonzero environment and RGB spectrum");
    constexpr uint full_samples = 64u, sparse_samples = 4096u;
    const uint pixels = size.x * size.y;
    const auto indices = native_reblur_diagnostic_indices(size);
    const uint count = uint(indices.size());
    auto full_mean = device.create_buffer<float4>(pixels);
    auto full_second = device.create_buffer<float4>(pixels);
    auto index_buffer = device.create_buffer<uint>(count);
    auto sparse = device.create_buffer<RtxptGIEnvironmentDiagnostic>(count);
    Kernel full_kernel = [&] {
        Function::current()->require_ray_query_compute_workgroups();
        Uint index = dispatch_id();
        Float3 sum = make_float3(0.f), square = make_float3(0.f);
        Float solar_count = 0.f, valid = 0.f;
        auto hit = fb.cur_visibility_buffer_var(frame).read(index);
        auto ray = fb.rays().read(index)->to_ray_state().ray;
        const auto swl = environment_diagnostic_rgb_wavelengths();
        $if(hit->is_hit()) {
            valid = 1.f;
            auto it = geo.compute_surface_interaction(hit, ray);
            Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                const auto bsdf = material->create_evaluator(it, swl);
                Uint state = tea(index, Uint{0xe1100000u ^ frame});
                $for(sample, full_samples) {
                    const Float ux = lcg(state), uy = lcg(state);
                    const auto contribution = environment_diagnostic_sample(it, bsdf, swl,
                                                                             make_float2(ux, uy), true);
                    sum += contribution.value;
                    square += contribution.value * contribution.value;
                    solar_count += contribution.events.x;
                };
            });
            sum /= float(full_samples); square /= float(full_samples);
        } $else {
            LightSampleContext ref;
            ref.pos = ray.org_min.xyz();
            sum = Toolkit::renderer().light_sampler()->evaluate_miss_wi(ref, ray.dir_max.xyz(), swl, LightEvalMode::L).L.vec3();
            square = sum * sum;
        };
        full_mean.write(index, make_float4(sum, valid));
        full_second.write(index, make_float4(square, solar_count / float(full_samples)));
    };
    full_kernel.function()->set_block_dim(64u, 1u, 1u);
    auto full_shader = device.compile(full_kernel, "RTXPT GI read-only environment importance 64spp oracle");
    Kernel sparse_kernel = [&] {
        Function::current()->require_ray_query_compute_workgroups();
        Uint sample_index = dispatch_id(), index = index_buffer.read(sample_index);
        RtxptGIEnvironmentDiagnosticVar row;
        for (uint i = 0u; i < 6u; ++i) row.values[i] = make_float4(0.f);
        row.identity = make_uint4(InvalidUI32);
        auto hit = fb.cur_visibility_buffer_var(frame).read(index);
        $if(hit->is_hit()) {
            auto ray = fb.rays().read(index)->to_ray_state().ray;
            auto it = geo.compute_surface_interaction(hit, ray);
            const auto swl = environment_diagnostic_rgb_wavelengths();
            row.identity = make_uint4(hit.inst_id, hit.prim_id, it.material_id(), 1u);
            row.values[5u] = make_float4(it.pos, 1.f);
            Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                const auto bsdf = material->create_evaluator(it, swl);
                Float4 visibility = make_float4(0.f);
                for (uint mode = 0u; mode < 2u; ++mode) {
                    Float3 sum = make_float3(0.f), square = make_float3(0.f);
                    Float4 events = make_float4(0.f);
                    Float positive = 0.f;
                    Uint state = tea(index, Uint{(mode == 0u ? 0xe1200000u : 0xe1300000u) ^ frame});
                    $for(sample, sparse_samples) {
                        const Float ux = lcg(state), uy = lcg(state);
                        const auto contribution = environment_diagnostic_sample(it, bsdf, swl,
                                                                                 make_float2(ux, uy), mode != 0u);
                        sum += contribution.value;
                        square += contribution.value * contribution.value;
                        events += contribution.events;
                        positive += cast<float>(any(contribution.value > make_float3(0.f)));
                    };
                    row.values[mode * 2u] = make_float4(sum / float(sparse_samples), positive / float(sparse_samples));
                    row.values[mode * 2u + 1u] = make_float4(square / float(sparse_samples), events.x / float(sparse_samples));
                    if (mode == 0u) { visibility.x = events.y; visibility.z = events.w; }
                    else { visibility.y = events.y; visibility.w = events.w; }
                }
                row.values[4u] = visibility / float(sparse_samples);
            });
        };
        sparse.write(sample_index, row);
    };
    sparse_kernel.function()->set_block_dim(64u, 1u, 1u);
    auto sparse_shader = device.compile(sparse_kernel, "RTXPT GI read-only uniform versus importance 4096spp oracle");
    vector<float4> host_mean(pixels), host_second(pixels);
    vector<RtxptGIEnvironmentDiagnostic> host_sparse(count);
    stream << index_buffer.upload(indices.data()) << full_shader().dispatch(pixels)
           << sparse_shader().dispatch(count) << full_mean.download(host_mean.data())
           << full_second.download(host_second.data()) << sparse.download(host_sparse.data())
           << synchronize() << ocarina::commit();
    const auto write = [&](const char *name, const auto &data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(data.data()), data.size() * sizeof(data[0]));
        if (!file) throw std::runtime_error("GI environment diagnostic write failed");
    };
    write("environment_probe.full_mean.f32", host_mean);
    write("environment_probe.full_second_moment.f32", host_second);
    write("environment_probe.indices.u32", indices);
    write("environment_probe.sparse.bin", host_sparse);
    std::ofstream metadata(directory / "environment_probe.json");
    metadata << "{\"version\":1,\"frame\":" << frame << ",\"width\":" << size.x << ",\"height\":" << size.y
        << ",\"count\":" << count << ",\"full_samples\":64,\"sparse_samples_per_distribution\":4096,"
           "\"full_layout\":\"full image float4 mean RGB,geometryHit; float4 E[X^2] RGB,incomingSolarRate\","
           "\"sparse_stride\":112,\"sparse_layout\":\"AOS float4 values[6],uint4 identity\","
           "\"sparse_values\":[\"uniform_mean_RGB,positiveRate\",\"uniform_second_RGB,incomingSolarRate\","
           "\"importance_mean_RGB,positiveRate\",\"importance_second_RGB,incomingSolarRate\","
           "\"uniformVisibleSolar,importanceVisibleSolar,uniformBlocked,importanceBlocked\",\"receiverXYZ,valid\"],"
           "\"identity\":\"instance,primitive,material,valid\",\"sun_threshold_incoming_Le_luminance\":1000,"
           "\"uniform_pdf\":\"1/(4*pi)\",\"importance_pdf\":\"public environment.sample_wi conditional solid-angle PDF\","
           "\"estimator\":\"BSDF_f_including_receiver_cosine * Le / PDF, ordinary spawn_ray_to visibility\","
           "\"rng\":\"private tea(pixel,mode^frame) then LCG; no renderer sampler reads/writes\","
           "\"scope\":\"read-only diagnostic, original renderer image and NEE1/1 unchanged\"}\n";
    if (!metadata) throw std::runtime_error("GI environment diagnostic metadata write failed");
}
}// namespace vision::rtxpt_gi
