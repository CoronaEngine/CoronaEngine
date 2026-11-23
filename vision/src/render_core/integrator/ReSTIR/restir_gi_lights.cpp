#include "restir_gi_lights.h"
#include "base/scattering/half_storage.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace vision::restir_gi {
using namespace ocarina;
namespace {
constexpr uint cube_dim = 2048u;
constexpr uint cube_mip_count = 3u; // Native diffuse-miss offset is two.
constexpr uint env_dim = 1024u;
constexpr uint env_nodes = 4096u;
constexpr uint native_max_lights = 1024u * 1024u - 2u;

uint mip_offset(uint dim, uint mip, uint faces = 1u) noexcept {
    uint offset = 0u;
    for (uint i = 0u; i < mip; ++i) { offset += dim * dim * faces; dim >>= 1u; }
    return offset;
}

}// namespace

void NativeLightSampler::prepare() {
    if (spectrum()->impl_type() != "srgb")
        throw std::runtime_error("ReSTIR GI native lighting currently requires the RGB spectrum");
    if (const char *value = std::getenv("VISION_RESTIR_GI_LIGHT_FEEDBACK"))
        feedback_enabled_ = std::atoi(value) != 0;
    size_t triangle_count = 0u;
    for (const auto &shape : scene().instances()) {
        if (!shape->has_emission()) continue;
        const auto *light = shape->emission().get();
        if (light->impl_type() != "area" || light->source_desc()["two_sided"].as_bool(false))
            throw std::runtime_error("ReSTIR GI native light table requires one-sided triangle emitters");
        triangle_count += shape->mesh()->triangles().size();
    }
    for (const auto &light : scene().light_manager().lights())
        if (!light->match(LightType::Area) && !light->match(LightType::Infinite))
            throw std::runtime_error("ReSTIR GI native light table does not yet support analytic lights");
    if (triangle_count >= native_max_lights - env_nodes)
        throw std::runtime_error("ReSTIR GI native light count exceeds the matched native light table limit");
    triangle_count_ = uint(triangle_count);
    light_count_ = env_nodes + triangle_count_;

    lights_ = device().create_buffer<uint4>(size_t(light_count_) * 2u, "ReSTIR GI native PolymorphicLightInfo32");
    environment_nodes_ = device().create_buffer<uint4>(env_nodes + env_dim * env_dim, "ReSTIR GI native environment quads and direction lookup");
    const uint env_texels = mip_offset(env_dim, 11u);
    environment_mips_ = device().create_buffer<float4>(env_texels, "ReSTIR GI native equal-area radiance/importance mips");
    environment_cube_ = device().create_buffer<uint2>(mip_offset(cube_dim, cube_mip_count, 6u),
                                                     "ReSTIR GI native 2048 cube half4 mip0-2");
    const uint proxy_capacity = 8u * std::max(light_count_, native_max_lights / 20u) + light_count_;
    proxy_indices_ = device().create_buffer<uint>(proxy_capacity, "ReSTIR GI native global proxy indices");
    proxy_counts_ = device().create_buffer<uint>(light_count_, "ReSTIR GI native global proxy counts");
    control_ = device().create_buffer<uint>(8u, "ReSTIR GI native global control");
    update_resolution(pipeline()->resolution());
    OC_INFO_FORMAT("ReSTIR GI native lights: {} env quad slots, {} emitter triangles", env_nodes, triangle_count_);
}

void NativeLightSampler::update_resolution(uint2 resolution) noexcept {
    std::array<uint, 8> control{};
    control[2] = resolution.x;
    control[3] = resolution.y;
    control[7] = uint(feedback_enabled_);
    control_.upload_immediately(control.data());
}

void NativeLightSampler::download_validation(const fs::path &directory) const {
    fs::create_directories(directory);
    vector<uint> control(8u), counts(light_count_);
    vector<uint4> lights(size_t(light_count_) * 2u), nodes(env_nodes);
    vector<float4> coarse_environment(21u);
    struct CubeProbe { uint mip, face, x, y; uint2 bits{}; };
    vector<CubeProbe> probes;
    for (uint mip : {0u, 2u}) for (uint face = 0u; face < 6u; ++face) {
        const uint dim = cube_dim >> mip;
        for (uint2 xy : {make_uint2(dim / 2u), make_uint2(0u), make_uint2(dim - 1u, 0u),
                         make_uint2(0u, dim - 1u), make_uint2(dim - 1u)})
            probes.push_back({mip, face, xy.x, xy.y});
    }
    auto &stream = pipeline()->stream();
    stream << control_.download(control.data()) << proxy_counts_.download(counts.data())
           << lights_.download(lights.data()) << environment_nodes_.view(0u, env_nodes).download(nodes.data())
           << environment_mips_.view(mip_offset(env_dim, 8u), 21u).download(coarse_environment.data());
    for (auto &probe : probes) {
        const uint dim = cube_dim >> probe.mip;
        const uint index = mip_offset(cube_dim, probe.mip, 6u) + probe.face * dim * dim + probe.y * dim + probe.x;
        stream << environment_cube_.view(index, 1u).download(&probe.bits);
    }
    stream << synchronize() << commit();
    vector<uint> proxies(control[0]);
    if (!proxies.empty()) stream << proxy_indices_.view(0u, proxies.size()).download(proxies.data()) << synchronize() << commit();
    const auto binary = [&]<typename T>(const char *name, const vector<T> &values) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(values.data()), std::streamsize(values.size() * sizeof(T)));
    };
    binary("lights32.u32", lights);
    binary("proxy-counts.u32", counts);
    binary("proxy-indices.u32", proxies);
    binary("control.u32", control);
    binary("environment-nodes.u32", nodes);
    binary("environment-coarse-rgba.f32", coarse_environment);
    DataWrap summary;
    summary["native_polymorphic_light_bytes"] = 32u;
    summary["light_count"] = light_count_;
    summary["environment_quad_slots"] = env_nodes;
    summary["triangle_slots"] = triangle_count_;
    summary["sampling_proxy_count"] = control[0];
    summary["power_weight_sum"] = std::bit_cast<float>(control[1]);
    summary["importance_sampling_type"] = feedback_enabled_ ? 2u : 1u;
    summary["temporal_feedback_enabled"] = feedback_enabled_;
    summary["valid_feedback_count"] = control[5];
    uint nonzero_environment = 0u, nonzero_triangles = 0u;
    for (uint i = 0u; i < light_count_; ++i)
        if (counts[i] != 0u) { if (i < env_nodes) ++nonzero_environment; else ++nonzero_triangles; }
    summary["positive_proxy_environment_lights"] = nonzero_environment;
    summary["positive_proxy_triangle_lights"] = nonzero_triangles;
    summary["cube_half4_probe"] = DataWrap::array();
    for (const auto &probe : probes) {
        const auto rg = unpack_storage_half2_host(probe.bits.x), ba = unpack_storage_half2_host(probe.bits.y);
        summary["cube_half4_probe"].push_back({{"mip", probe.mip}, {"face", probe.face}, {"x", probe.x}, {"y", probe.y},
            {"rgba", {rg.x, rg.y, ba.x, ba.y}}, {"packed_u32", {probe.bits.x, probe.bits.y}}});
    }
    summary["remaining_resource_differences"] = {"Manual cube seam filtering", "Triangle emissive texture uses center LOD0 instead of native anisotropic SampleGrad", "Native BC6 cube compression must be disabled in controlled comparison"};
    std::ofstream file(directory / "summary.json");
    file << summary.dump(2);
}
}// namespace vision::restir_gi
