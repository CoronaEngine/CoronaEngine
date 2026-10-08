// Opt-in readback for identifying the first invalid stage of a noisy pixel.
#define VISION_PLUGIN_NAME "SVGF"
#define VISION_CATEGORY "denoiser"
#include "render_core/denoiser/SVGF/svgf.h"
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/evaluation_debug.h"
#include "ext/nlohmann/json.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace vision;
using namespace ocarina;

void save_pixel_diagnostics(Pipeline &pipeline) {
    if (pipeline.frame_index() == 0u) return;
    const auto root = evaluation_debug::directory(pipeline.frame_index() - 1u);
    if (root.empty()) return;
    fs::create_directories(root);
    const auto count = pipeline.pixel_num();
    const auto rendered = pipeline.frame_index() - 1u;
    auto &fb = *pipeline.frame_buffer();
    vector<TriangleHit> hits(count);
    vector<SurfaceData> surfaces(count);
    vector<float2> motion(count);
    pipeline.stream() << fb.cur_visibility_buffer_view(rendered).download(hits.data())
        << fb.cur_surfaces_view(rendered).download(surfaces.data())
        << fb.motion_vectors().view().download(motion.data()) << synchronize() << commit();
    auto write = [&](const std::string &name, const auto &data) {
        std::ofstream out(root / name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), data.size() * sizeof(data[0]));
        if (!out) throw std::runtime_error("pixel diagnostic write failed");
    };
    vector<float4> visibility(count), shaded(count), normals(count), positions(count);
    vector<uint> stable_branches(count);
    for (uint i = 0; i < count; ++i) {
        visibility[i] = make_float4(float(hits[i].inst_id), float(hits[i].prim_id), hits[i].bary.x, hits[i].bary.y);
        const auto &s = surfaces[i];
        shaded[i] = make_float4(float(s.hit.inst_id), float(s.hit.prim_id), float(s.is_replaced), float(s.flag));
        normals[i] = s.normal_depth;
        positions[i] = s.pos_diff;
        stable_branches[i] = s.stable_branch;
    }
    write("stable_branch.u32", stable_branches);
    write("visibility.f32", visibility); write("shaded.f32", shaded);
    write("normal_depth.f32", normals); write("position.f32", positions); write("motion.f32", motion);
    auto *illumination = dynamic_cast<IlluminationIntegrator *>(pipeline.renderer().integrator().get());
    auto *denoiser = illumination ? dynamic_cast<svgf::SVGF *>(illumination->denoiser()) : nullptr;
    const bool layered = illumination && illumination->stable_planes_enabled();
    const bool denoise = denoiser && denoiser->enabled() && !evaluation_debug::denoiser_disabled();
    if (layered) {
        vector<uint> dominant(count);
        vector<float4> emission(count);
        pipeline.stream() << fb.stable_dominant().view().download(dominant.data())
            << fb.stable_radiance().view().download(emission.data()) << synchronize() << commit();
        write("dominant.u32", dominant);
        write("stable_radiance.f32", emission);
        for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
            vector<StablePlaneData> planes(count);
            pipeline.stream() << fb.cur_stable_planes_view(rendered, layer).download(planes.data())
                << synchronize() << commit();
            const auto prefix = "layer_" + std::to_string(layer) + "_";
            auto export_field = [&](const char *name, auto extract) {
                vector<decltype(extract(planes[0]))> packed(count);
                for (uint i = 0u; i < count; ++i) packed[i] = extract(planes[i]);
                write(prefix + name, packed);
            };
            export_field("identity.u32", [](const auto &p) { return make_uint4(p.branch_sequence, p.instance_hash, p.depth, p.material_id); });
            export_field("state.u32", [](const auto &p) { return make_uint4(p.valid, p.reservoir_eligible, p.surface.approximate, p.surface.stable_branch); });
            export_field("hit.u32", [](const auto &p) { return make_uint2(p.surface.hit.inst_id, p.surface.hit.prim_id); });
            export_field("motion.f32", [](const auto &p) { return p.motion; });
            export_field("physical_normal_depth.f32", [](const auto &p) { return p.surface.normal_depth; });
            export_field("physical_position.f32", [](const auto &p) { return p.surface.pos_diff; });
            export_field("virtual_position.f32", [](const auto &p) { return make_float4(p.surface.virtual_position, 0.f); });
            export_field("virtual_normal.f32", [](const auto &p) { return make_float4(p.surface.virtual_normal, 0.f); });
            export_field("virtual_geometric_normal.f32", [](const auto &p) { return make_float4(p.surface.virtual_geometric_normal, 0.f); });
            export_field("depth_position.f32", [](const auto &p) { return make_float4(p.depth_position, 0.f); });
            export_field("diffuse_roughness.f32", [](const auto &p) { return p.surface.diffuse_roughness; });
            export_field("specular_roughness.f32", [](const auto &p) { return p.surface.specular_roughness; });
            export_field("albedo.f32", [](const auto &p) { return make_float4(p.surface.denoiser_albedo, 0.f); });
            export_field("throughput.f32", [](const auto &p) { return make_float4(p.extension.throughput, 0.f); });
        }
    }
    if (denoise && denoiser->has_prepared_resources()) {
      for (uint layer = 0u; layer < (layered ? StablePlaneCount : 1u); ++layer) {
        vector<svgf::SVGFDataDual> history(count);
        pipeline.stream() << denoiser->svgf_buffer_cur(rendered, layer).download(history.data()) << synchronize() << commit();
        auto pack = [&](auto member) {
            vector<float4> result(count);
            for (uint i = 0; i < count; ++i) {
                const auto &v = history[i].*member;
                result[i] = make_float4(float(v.x), float(v.y), float(v.z), float(v.w));
            }
            return result;
        };
        const auto prefix = layered ? "layer_" + std::to_string(layer) + "_" : std::string{};
        write(prefix + "history_direct.f32", pack(&svgf::SVGFDataDual::illumi_direct));
        write(prefix + "history_indirect.f32", pack(&svgf::SVGFDataDual::illumi_indirect));
        write(prefix + "moments_direct.f32", pack(&svgf::SVGFDataDual::moments_direct));
        write(prefix + "moments_indirect.f32", pack(&svgf::SVGFDataDual::moments_indirect));
        write(prefix + "shading_normal.f32", pack(&svgf::SVGFDataDual::surface_normal));
        vector<float2> ages(count);
        for (uint i = 0u; i < count; ++i) ages[i] = make_float2(float(history[i].moments_direct.z), float(history[i].moments_direct.w));
        write(prefix + "history_count.f32", ages);
      }
    }
    nlohmann::json metadata;
    metadata["resolution"] = {pipeline.resolution().x, pipeline.resolution().y};
    metadata["frame"] = rendered;
    metadata["schema"] = 2;
    metadata["denoise"] = denoise;
    metadata["stable_planes"] = layered;
    metadata["layer_count"] = layered ? StablePlaneCount : 1u;
    metadata["layout"] = {{"identity.u32", {"branch_sequence", "instance_hash", "depth", "material_id"}},
        {"state.u32", {"valid", "reservoir_eligible", "approximate", "stable_branch"}},
        {"hit.u32", {"instance", "primitive"}}, {"history_count.f32", {"direct", "indirect"}},
        {"motion.f32", {"x", "y"}}};
    metadata["float4_note"] = "Other f32 outputs are tightly packed float4; normal_depth.w is linear guide depth; position.w is diffuse factor; radiance rgb is linear.";
    metadata["legacy_radiance_note"] = "raw/filtered_direct/indirect without layer suffix are legacy dominant reservoir buffers; layered results use raw/filtered_layer_N_direct/indirect.";
    uint instance_index = 0u;
    for (const auto &instance : pipeline.scene().instances()) {
        metadata["instances"].push_back({{"index", instance_index++}, {"name", instance->name()},
            {"material", instance->material_name()}, {"type", std::string(instance->material()->impl_type())}});
    }
    std::ofstream(root / "metadata.json") << metadata.dump(2);
    // Isolate lighting error from subpixel coverage: hold the exact ReSTIR
    // primary rays fixed while independently sampling the full PT integrand.
    const char *pt_samples = std::getenv("VISION_EVAL_DEBUG_FIXED_RAY_PT_SPP");
    if (pt_samples && illumination) {
        const uint samples = std::min(4096u, uint(std::strtoul(pt_samples, nullptr, 10)));
        if (samples == 0u) return;
        vector<uint> selected;
        if (const char *pixels = std::getenv("VISION_EVAL_DEBUG_PT_PIXELS")) {
            std::istringstream input(pixels);
            uint x, y;
            while (input >> x) {
                if (!(input >> y))
                    throw std::runtime_error("diagnostic PT pixels must be x y pairs");
                if (x >= pipeline.resolution().x || y >= pipeline.resolution().y)
                    throw std::runtime_error("diagnostic PT pixel out of bounds");
                selected.push_back(y * pipeline.resolution().x + x);
            }
            if (!input.eof() || selected.empty())
                throw std::runtime_error("diagnostic PT pixels must be x y pairs");
        }
        const uint output_count = selected.empty() ? count : uint(selected.size());
        vector<uint> pixel_indices(output_count);
        for (uint i = 0; i < output_count; ++i)
            pixel_indices[i] = selected.empty() ? i : selected[i];
        uint max_depth = 8u;
        if (const char *depth = std::getenv("VISION_EVAL_DEBUG_PT_MAX_DEPTH"))
            max_depth = std::max(1u, std::min(64u, uint(std::strtoul(depth, nullptr, 10))));
        metadata["fixed_pt_samples"] = samples;
        metadata["fixed_pt_max_depth"] = max_depth;
        metadata["fixed_pt_pixel_indices"] = selected;
        std::ofstream(root / "metadata.json") << metadata.dump(2);
        pipeline.activate_global_context();
        Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
        auto &sampler = pipeline.renderer().sampler();
        auto &spectrum = pipeline.renderer().spectrum();
        Kernel kernel = [&](Uint sample, BufferVar<uint> indices, BufferVar<float4> output,
                            BufferVar<float4> statistics) {
            Env::instance().clear_global_vars();
            sampler->load_data();
            illumination->load_data();
            RenderEnv environment;
            environment.initial(sampler, sample, spectrum);
            Uint pixel_index = indices.read(dispatch_id());
            Uint2 pixel = make_uint2(pixel_index % pipeline.resolution().x,
                                    pixel_index / pipeline.resolution().x);
            sampler->set_seed(pixel, sample, Dimension::PathTracing);
            RayDataVar ray = fb.rays().read(pixel_index);
            Float3 value = illumination->Li(ray->to_ray_state(), Float{1e16f}, Uint{max_depth},
                spectrum->one(), false, HitContext{}, environment);
            Float4 sum = make_float4(value, 1.f);
            Float energy = (value.x + value.y + value.z) / 3.f;
            Float4 stats = make_float4(ocarina::select(energy > 1e-10f, 1.f, 0.f),
                                       energy, energy * energy, energy);
            $if(sample > 0u) {
                sum += output.read(dispatch_id());
                Float4 previous = statistics.read(dispatch_id());
                stats.xyz() += previous.xyz();
                stats.w = max(stats.w, previous.w);
            };
            output.write(dispatch_id(), sum);
            statistics.write(dispatch_id(), stats);
        };
        auto shader = pipeline.device().compile(kernel, "diagnostic_fixed_primary_pt");
        auto indices = pipeline.device().create_buffer<uint>(output_count, "diagnostic_pt_pixel_indices");
        indices.upload_immediately(pixel_indices.data());
        auto output = pipeline.device().create_buffer<float4>(output_count, "diagnostic_fixed_primary_pt");
        auto statistics = pipeline.device().create_buffer<float4>(output_count, "diagnostic_pt_statistics");
        for (uint sample = 0; sample < samples; ++sample) {
            pipeline.stream() << shader(sample, indices, output, statistics).dispatch(output_count);
        }
        vector<float4> pixels(output_count), stats(output_count);
        pipeline.stream() << output.download(pixels.data()) << statistics.download(stats.data())
                          << synchronize() << commit();
        for (auto &pixel : pixels) pixel /= float(samples);
        write("fixed_ray_pt.f32", pixels);
        write("fixed_ray_pt_statistics.f32", stats);
    }
}
