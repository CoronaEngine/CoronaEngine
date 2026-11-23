#pragma once

#include "rtxpt_gi_shadow_diagnostic.h"
#include "base/scattering/gi_principled_parameters.h"

namespace vision {
struct RtxptGIPrimaryLightDiagnostic {
    array<float4, 18> values{};
    array<uint4, 5> ids{};
};
static_assert(sizeof(RtxptGIPrimaryLightDiagnostic) == 368u);
}
OC_STRUCT(vision, RtxptGIPrimaryLightDiagnostic, values, ids) {};

namespace vision::rtxpt_gi {
[[nodiscard]] inline bool primary_light_diagnostic_enabled() noexcept {
    const char *enabled = std::getenv("VISION_GI_PRIMARY_LIGHT_DIAGNOSTIC");
    return enabled && enabled[0] == '1';
}

// Observe the original, already generated ReSTIR DI/GI BSDF proposal. No new
// samples, reservoir writes or image writes; each valid proposal traces two rays.
inline void download_primary_light_diagnostics(Device &device, Stream &stream, FrameBuffer &fb,
                                               uint frame, uint2 size, const DIRadiance &direct,
                                               const std::filesystem::path &directory) {
    if (!primary_light_diagnostic_enabled()) return;
    auto &diagnostic_geometry = Toolkit::scene().geometry();
    Global::SceneGpuContextScope scene_gpu_context{diagnostic_geometry.bindless_array(),
                                                  diagnostic_geometry.gpu_resource()->device()};
    if (Toolkit::spectrum()->impl_type() != "srgb")
        throw std::runtime_error("GI primary light diagnostic requires RGB");
    const auto indices = native_reblur_diagnostic_indices(size);
    const uint count = uint(indices.size()), pixels = size.x * size.y;
    const auto position_storage = fb.prepare_screen_storage("ReSTIR DI surface position and depth", make_uint2(1u),
                                                           2u * sizeof(uint4));
    const auto normal_storage = fb.prepare_screen_storage("ReSTIR DI surface normals", make_uint2(1u),
                                                         2u * sizeof(uint2));
    Buffer<uint4> positions{position_storage.view_as<uint4>()};
    Buffer<uint2> normals{normal_storage.view_as<uint2>()};
    auto index_buffer = device.create_buffer<uint>(count);
    auto output = device.create_buffer<RtxptGIPrimaryLightDiagnostic>(count);
    auto &geo = Toolkit::geometry();
    auto &lights = Toolkit::renderer().light_sampler();
    const bool has_environment = lights->env_light() != nullptr;
    Kernel kernel = [&] {
        Function::current()->require_ray_query_compute_workgroups();
        Uint sample_index = dispatch_id(), index = index_buffer.read(sample_index);
        RtxptGIPrimaryLightDiagnosticVar row;
        for (uint i = 0u; i < 18u; ++i) row.values[i] = make_float4(0.f);
        for (uint i = 0u; i < 5u; ++i) row.ids[i] = make_uint4(InvalidUI32);
        row.ids[1u] = make_uint4(0u);
        row.ids[4u] = make_uint4(index, frame, 0u, 0u);
        row.values[7u] = direct.read_radiance(index);
        auto hit = fb.cur_visibility_buffer_var(frame).read(index);
        $if(hit->is_hit()) {
            Uint flags = 1u;
            auto camera_ray = fb.rays().read(index)->to_ray_state().ray;
            auto it = geo.compute_surface_interaction(hit, camera_ray);
            const auto proposal = fb.hit_bsdfs().read(index);
            const Float3 wi = proposal.wi.as_vec3();
            const Uint surface_index = (frame & 1u) * pixels + index;
            const Float3 receiver = as<float4>(positions.read(surface_index, false)).xyz();
            const Uint2 packed_normal = normals.read(surface_index, false);
            const Float3 ns = restir_di::unpack_normal(packed_normal.x);
            const Float3 ng = restir_di::unpack_normal(packed_normal.y);
            row.values[0u] = make_float4(it.pos, proposal.pdf);
            row.values[1u] = make_float4(it.wo, dot(ns, it.wo));
            row.values[2u] = make_float4(ns, dot(ns, wi));
            row.values[3u] = make_float4(ng, 0.f);
            row.values[4u] = make_float4(wi, length(wi));
            row.values[5u] = make_float4(proposal.bsdf.as_vec3(), 0.f);
            row.ids[0u] = make_uint4(hit.inst_id, hit.prim_id, it.material_id(), 0u);
            const auto swl = environment_diagnostic_rgb_wavelengths();
            Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                const auto bsdf = material->create_evaluator(it, swl);
                const Uint bsdf_flags = bsdf.flag();
                row.ids[0u].w = bsdf_flags;
                $if(BxDFFlag::is_specular(bsdf_flags)) { flags |= 4u; };
                $if(dot(ns, wi) > 0.f) { flags |= 8u; };
                const Bool valid = proposal.pdf > 0.f && restir_di::finite(proposal.pdf) &&
                                   all(!ocarina::isnan(wi)) && all(!ocarina::isinf(wi)) && length_squared(wi) > 0.f;
                $if(valid) {
                    flags |= 2u;
                    const auto eval = bsdf.evaluate(it.wo, wi, MaterialEvalMode::All);
                    row.values[3u].w = eval.pdf();
                    row.values[6u] = make_float4(eval.f.vec3(), 0.f);
                    row.ids[1u].x = eval.flags;
                    $if(BxDFFlag::is_transmission(eval.flags)) { flags |= 8192u; };
                    $if(!BxDFFlag::is_specular(bsdf_flags) && dot(ns, wi) > 0.f) { flags |= 16u; };
                    const Float3 restir_di_origin = Interaction::custom_offset_ray_origin(receiver, ng, wi);
                    // Same proposal, exact current DI ray versus ordinary PT ray.
                    // Record both even when DI's hemisphere gate rejects the proposal.
                    for (uint mode = 0u; mode < 2u; ++mode) {
                        auto ray = mode == 0u
                            ? restir_di::make_light_ray(restir_di_origin, wi, Float{restir_di::ray_epsilon},
                                                       Float{restir_di::environment_ray_distance})
                            : it.spawn_ray(wi);
                        const uint base = 8u + 4u * mode;
                        row.values[base] = ray.org_min;
                        row.values[base + 1u] = make_float4(0.f);
                        LightSampleContext ref;
                        ref.pos = ray.org_min.xyz(); ref.ng = it.ng; ref.ns = ns;
                        Float3 emission = make_float3(0.f);
                        Float light_pdf = 0.f;
                        const auto first_hit = geo.trace_closest(ray);
                        row.ids[2u + mode] = make_uint4(first_hit.inst_id, first_hit.prim_id,
                                                       InvalidUI32, InvalidUI32);
                        $if(first_hit->is_hit()) {
                            flags |= mode == 0u ? 32u : 256u;
                            auto first_it = geo.compute_surface_interaction(first_hit, ray);
                            row.values[base + 1u] = make_float4(first_it.pos, length(first_it.pos - ref.pos));
                            row.ids[2u + mode].z = first_it.material_id();
                            row.ids[2u + mode].w = first_it.light_id();
                            $if(first_it.has_emission()) {
                                flags |= mode == 0u ? 64u : 512u;
                                const auto light = lights->evaluate_hit_wi(ref, first_it, swl);
                                emission = light.L.vec3(); light_pdf = light.pdf;
                            };
                        } $else {
                            if (has_environment) {
                                flags |= mode == 0u ? 128u : 1024u;
                                const auto light = lights->evaluate_miss_wi(ref, wi, swl);
                                emission = light.L.vec3(); light_pdf = light.pdf;
                            }
                        };
                        row.values[base + 2u] = make_float4(emission, light_pdf);
                        // Standalone BSDF first-light term, without MIS. This is
                        // evidence of a rejected proposal, never a corrected image.
                        row.values[base + 3u] = make_float4(proposal.bsdf.as_vec3() * emission / proposal.pdf, 0.f);
                        $if(any(emission > make_float3(0.f))) { flags |= mode == 0u ? 4096u : 2048u; };
                    }
                };
            });
            row.ids[1u].w = flags;
        };
        output.write(sample_index, row);
    };
    kernel.function()->set_block_dim(64u, 1u, 1u);
    auto shader = device.compile(kernel, "RTXPT GI read-only original primary BSDF first-light witness");
    // A separate lexical scope records the GI evaluator's actual flags without
    // changing the ordinary evaluator used above or any existing shaders.
    Kernel gi_kernel = [&] {
        gi_principled::CompilationScope compilation_scope{false};
        Uint sample_index = dispatch_id(), index = index_buffer.read(sample_index);
        auto row = output.read(sample_index);
        auto hit = fb.cur_visibility_buffer_var(frame).read(index);
        $if(hit->is_hit()) {
            auto ray = fb.rays().read(index)->to_ray_state().ray;
            auto it = geo.compute_surface_interaction(hit, ray);
            const auto swl = environment_diagnostic_rgb_wavelengths();
            Toolkit::scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                const auto bsdf = material->create_evaluator(it, swl);
                const Uint gi_flags = bsdf.flag();
                row.ids[1u].y = gi_flags;
                row.ids[1u].z = gi_principled::flags();
                row.values[16u] = make_float4(gi_principled::value(3u), gi_principled::value(7u),
                                             gi_principled::value(18u), gi_principled::value(16u));
                row.values[17u] = make_float4(it.shading.normal(), dot(it.shading.normal(), it.wo));
                $if(BxDFFlag::is_specular(gi_flags)) { row.ids[1u].w |= 16384u; };
                $if(gi_principled::captured(it.material_id())) { row.ids[1u].w |= 32768u; };
                $if((gi_principled::flags() & gi_principled::IdealGlassBit) != 0u) { row.ids[1u].w |= 65536u; };
            });
        };
        output.write(sample_index, row);
    };
    gi_kernel.function()->set_block_dim(64u, 1u, 1u);
    auto gi_shader = device.compile(gi_kernel, "RTXPT GI read-only primary material flags witness");
    vector<RtxptGIPrimaryLightDiagnostic> host(count);
    stream << index_buffer.upload(indices.data()) << shader().dispatch(count) << gi_shader().dispatch(count)
           << output.download(host.data()) << synchronize() << ocarina::commit();
    const auto write = [&](const char *name, const auto &data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(data.data()), data.size() * sizeof(data[0]));
        if (!file) throw std::runtime_error("GI primary light diagnostic write failed");
    };
    write("primary_light.indices.u32", indices);
    write("primary_light.records.bin", host);
    std::ofstream metadata(directory / "primary_light.json");
    metadata << "{\"version\":1,\"frame\":" << frame << ",\"width\":" << size.x << ",\"height\":" << size.y
        << ",\"count\":" << count << ",\"stride\":368,\"layout\":\"AOS float4 values[18],uint4 ids[5]\","
           "\"values\":[\"primaryPos,storedProposalPDF\",\"wo,dotPackedNsWo\",\"packedNs,dotPackedNsWi\","
           "\"packedNg,ordinarySameDirectionPDF\",\"storedWi,length\",\"storedF,0\",\"ordinarySameDirectionF,0\",\"rawDI\","
           "\"restirDIOrigin,tmin\",\"restirDIFirstPosition,distance\",\"restirDILe,ordinaryLightPDF\",\"restirDIBSDFTermWithoutMIS,0\","
           "\"ptOrigin,tmin\",\"ptFirstPosition,distance\",\"ptLe,ordinaryLightPDF\",\"ptBSDFTermWithoutMIS,0\","
           "\"GIroughness,metallic,transmission,ior\",\"GIshadingNormal,dotWo\"],"
           "\"ids\":[\"primaryInstance,primitive,material,ordinaryBSDFflags\",\"ordinaryEvalFlags,GIbsdfFlags,GIparameterFlags,bits\","
           "\"restirDIFirstInstance,primitive,material,light\",\"ptFirstInstance,primitive,material,light\",\"pixel,frame,0,0\"],"
           "\"bits\":{\"primaryHit\":1,\"validProposal\":2,\"ordinarySpecular\":4,\"positiveHemisphere\":8,\"DIeligible\":16,"
           "\"restirDIHit\":32,\"restirDIEmitterGeometry\":64,\"restirDIMissEnvironment\":128,\"ptHit\":256,\"ptEmitterGeometry\":512,"
           "\"ptMissEnvironment\":1024,\"ptPositiveFirstLight\":2048,\"restirDIPositiveFirstLight\":4096,\"ordinaryEvalTransmission\":8192,"
           "\"GIspecular\":16384,\"GIparametersCaptured\":32768,\"GIidealGlass\":65536},"
           "\"ownership\":\"Existing GI capture excludes first BSDF emission/miss at bounces<=1; independent BSDF term has no MIS and is not a corrected estimate\","
           "\"proposal\":\"Original fb.hit_bsdfs written before ReSTIR DI hemisphere/light-hit gates; no new sampling\","
           "\"rays\":\"Exact ReSTIR DI packed-surface custom offset,tmin.001,tmax1e16 versus ordinary it.spawn_ray stored wi\","
           "\"scope\":\"Only diagnostic buffers written; original image, sampler and reservoirs untouched\"}\n";
    if (!metadata) throw std::runtime_error("GI primary light diagnostic metadata write failed");
    OC_INFO_FORMAT("GI primary light diagnostic: downloaded {} original proposal witnesses", count);
}
}// namespace vision::rtxpt_gi
