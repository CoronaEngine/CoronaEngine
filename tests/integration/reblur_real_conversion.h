#pragma once

// Diagnostic common-format conversion. Production Pack/Unpack stay intact.
#include "render_core/denoiser/ReBLUR/edsl_nrd_common.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd_resources.h"
#include "render_core/denoiser/ReBLUR/native_nrd.h"
#include "ext/nlohmann/json.hpp"
#include "math/half.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace vision::reblur::real_conversion {
using namespace ocarina;
using namespace vision::reblur::edsl;

// Matches CommonFrontend.hlsl b0 and the actual RTXPT capture, byte for byte.
struct alignas(16) FrontendConstants {
    uint2 size{};
    float clamp_max{}, padding{};
    float4 hit_distance_parameters{};
};
static_assert(sizeof(FrontendConstants) == 32u);
static_assert(offsetof(FrontendConstants, clamp_max) == 8u);
static_assert(offsetof(FrontendConstants, hit_distance_parameters) == 16u);

#include "reblur_real_conversion_post.h"

inline PassShader compile_frontend(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Ulong field_bytes = p.external.size.x.cast<ulong>() * p.external.size.y.cast<ulong>() * 16ull;
        Ulong offset = index.cast<ulong>() * 16ull;
        Float4 motion_view_z = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, offset);
        Int2 position = make_int2(pixel);
        Image<float4, Format::RGBA16Float> diffuse_output{p.outputs.t0};
        Image<float4, Format::RGBA16Float> specular_output{p.outputs.t1};
        Image<float4, Format::RGB10A2Unorm> normal_output{p.outputs.t2};
        Image<float, Format::R32Float> depth_output{p.outputs.t3};
        Image<float4, Format::RGBA16Float> motion_output{p.outputs.t4};
        Image<float, Format::R8Unorm> mix_output{p.outputs.t5};
        depth_output.write_in_bounds(position, motion_view_z.w);
        // Match the original exact no-surface marker and sky's depth-only store.
        $if(as<uint>(motion_view_z.w) == 0x7f7fffffu) { $return(); };

        Float4 normal_roughness = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes + offset);
        Float4 diffuse_factor_mix = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 2ull + offset);
        Float4 specular_factor_suppression = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 3ull + offset);
        Float4 diffuse = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 4ull + offset);
        Float4 specular = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 5ull + offset);
        motion_output.write_in_bounds(position, make_float4(motion_view_z.xyz(), 0.f));
        mix_output.write_in_bounds(position, diffuse_factor_mix.w);
        Float filtering_roughness = saturate(normal_roughness.w + diffuse_factor_mix.w);
        diffuse = make_float4(diffuse.xyz() / diffuse_factor_mix.xyz(), diffuse.w);
        specular = make_float4(specular.xyz() / specular_factor_suppression.xyz(), specular.w);
        specular = make_float4(specular.xyz() * specular_factor_suppression.w, specular.w);
        normal_output.write_in_bounds(position, PackNormalRoughness(normal_roughness.xyz(), filtering_roughness, 0.f));

        // The source clamp branches before dividing, exactly like RTXPT.
        Float clamp_max = p.previous_view_depth_row.x;
        Float diffuse_luminance = _NRD_Luminance(diffuse.xyz());
        $if(diffuse_luminance > clamp_max) {
            diffuse = make_float4(diffuse.xyz() * (clamp_max / diffuse_luminance), diffuse.w);
        };
        Float specular_luminance = _NRD_Luminance(specular.xyz());
        $if(specular_luminance > clamp_max) {
            specular = make_float4(specular.xyz() * (clamp_max / specular_luminance), specular.w);
        };
        // Do not add the production Guide frontend's finite-hit precheck.
        Float diffuse_hit = saturate(diffuse.w / HitDistanceNormalization(motion_view_z.w, p.constants.gHitDistParams, 1.f));
        diffuse_output.write_in_bounds(position, PackRadiance(diffuse.xyz(), diffuse_hit));
        Float specular_hit = saturate(specular.w / HitDistanceNormalization(motion_view_z.w, p.constants.gHitDistParams, normal_roughness.w));
        specular_output.write_in_bounds(position, PackRadiance(specular.xyz(), specular_hit));
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Pack");
}

namespace detail {
using Json = nlohmann::json;
inline constexpr std::array<const char *, 6> names{
    "IN_MV", "IN_NORMAL_ROUGHNESS", "IN_VIEWZ", "IN_DIFF_RADIANCE_HITDIST",
    "IN_SPEC_RADIANCE_HITDIST", "IN_DISOCCLUSION_THRESHOLD_MIX"};
inline constexpr std::array<uint, 6> pixel_bytes{8u, 4u, 4u, 8u, 8u, 1u};
inline void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error("Common frontend: " + message);
}
inline std::vector<std::byte> read_bytes(const std::filesystem::path &path, size_t count) {
    require(std::filesystem::file_size(path) == count, "wrong byte size: " + path.string());
    std::vector<std::byte> bytes(count);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(count));
    require(bool(file), "cannot read " + path.string());
    return bytes;
}
inline float canonical_depth(const std::vector<std::byte> &bytes, size_t pixel) {
    float value; std::memcpy(&value, bytes.data() + pixel * 16u + 12u, sizeof(value)); return value;
}
inline double component(const std::vector<std::byte> &bytes, size_t pixel, uint slot, uint channel) {
    if (slot == 1u) {
        uint value; std::memcpy(&value, bytes.data() + pixel * 4u, sizeof(value));
        return channel == 3u ? double(value >> 30u) / 3. : double((value >> (channel * 10u)) & 1023u) / 1023.;
    }
    if (slot == 5u) return double(std::to_integer<unsigned char>(bytes[pixel])) / 255.;
    if (slot == 2u) {
        float value; std::memcpy(&value, bytes.data() + pixel * 4u, sizeof(value)); return value;
    }
    uint16_t value; std::memcpy(&value, bytes.data() + pixel * 8u + channel * 2u, sizeof(value));
    return half_to_float(value);
}
inline Json compare(const std::vector<std::byte> &canonical, const std::vector<std::byte> &reference,
                    const std::vector<std::byte> &actual, uint slot) {
    const size_t pixels = canonical.size() / 96u;
    uint64_t surface_pixels = 0u, compared_pixels = 0u, changed_pixels = 0u, changed_bytes = 0u, nonfinite = 0u;
    double maximum = 0.;
    for (size_t pixel = 0u; pixel != pixels; ++pixel) {
        const bool surface = canonical_depth(canonical, pixel) != std::numeric_limits<float>::max();
        surface_pixels += surface;
        if (!surface && slot != 2u) continue;
        ++compared_pixels;
        const size_t offset = pixel * pixel_bytes[slot];
        const bool changed = std::memcmp(reference.data() + offset, actual.data() + offset, pixel_bytes[slot]) != 0;
        changed_pixels += changed;
        for (uint byte = 0u; byte != pixel_bytes[slot]; ++byte)
            changed_bytes += reference[offset + byte] != actual[offset + byte];
        const uint components = slot == 2u || slot == 5u ? 1u : 4u;
        for (uint channel = 0u; channel != components; ++channel) {
            const double a = component(actual, pixel, slot, channel), b = component(reference, pixel, slot, channel);
            if (!std::isfinite(a) || !std::isfinite(b)) ++nonfinite;
            else maximum = std::max(maximum, std::abs(a - b));
        }
    }
    return {{"signal", names[slot]}, {"surface_pixels", surface_pixels},
        {"ignored_sky_pixels", slot == 2u ? 0u : pixels - surface_pixels}, {"compared_pixels", compared_pixels},
        {"changed_pixels", changed_pixels}, {"changed_bytes", changed_bytes}, {"nonfinite_components", nonfinite},
        {"maximum_absolute_storage_component_error", maximum}, {"byte_equal", changed_bytes == 0u}};
}
} // namespace detail

// Called only from the explicit REBLUR_CONVERSION_INPUT_DIR diagnostic branch.
// Canonical buffers upload once; the loop repeats actual stateless conversion.
inline void run(Device &device, Stream &stream, const std::string &backend,
                uint warmup, uint measured_frames, uint pause_ms) {
    using namespace detail;
    const char *input_option = std::getenv("REBLUR_CONVERSION_INPUT_DIR");
    const char *output_option = std::getenv("REBLUR_CONVERSION_OUTPUT_DIR");
    require(input_option && *input_option && output_option && *output_option, "input/output directories are required");
    require(backend == "native" || backend == "edsl", "backend must be native or edsl");
    require(measured_frames && uint64_t(warmup) + measured_frames <= UINT32_MAX, "invalid iteration count");
    const char *scope_option = std::getenv("REBLUR_CONVERSION_SCOPE");
    const std::string scope = scope_option && *scope_option ? scope_option : "frontend";
    require(scope == "frontend" || scope == "full", "conversion scope must be frontend or full");
    const bool full = scope == "full";
    const std::filesystem::path input = input_option, output = output_option;
    const auto constant_bytes = read_bytes(input / "canonical_frontend_constants.bin", sizeof(FrontendConstants));
    FrontendConstants constants{}; std::memcpy(&constants, constant_bytes.data(), sizeof(constants));
    require(constants.size.x && constants.size.y && std::isfinite(constants.clamp_max) && constants.clamp_max >= 0.f,
            "invalid captured constants");
    const uint2 size = constants.size;
    const size_t pixels = size_t(size.x) * size.y;
    std::filesystem::create_directories(output);
    std::array<std::shared_ptr<std::vector<std::byte>>, 3> canonical;
    std::array<std::shared_ptr<PackedNrdInputs>, 3> actual;
    std::array<std::shared_ptr<PackedNrdOutputs>, 3> nrd_outputs, linear_outputs;
    std::array<ByteBuffer, 3> edsl_inputs;
    std::array<std::array<Texture2D, 6>, 3> images;
    std::array<std::array<Texture2D, 2>, 3> fixed_nrd_images;
    std::array<std::array<Buffer<float4>, 2>, 3> linear_buffers;
    std::array<Dispatch, 3> parameters{};
    std::unique_ptr<NativeReblur> native;
    PassShader frontend, endpoint, unpack;
    if (backend == "native") native = std::make_unique<NativeReblur>(device.context()->runtime_directory() / "reblur-native");
    else {
        frontend = compile_frontend(device);
        Kernel empty = [](Var<Dispatch>) { configure_block(16u, 8u); };
        // These empty dispatches close the existing shared-point profiler state
        // machine. Their NRD/Unpack/Filter rows are not conversion measurements.
        endpoint = device.compile(empty, "NRD-EDSL-REBLUR-TemporalStabilization");
        unpack = full ? compile_common_backend(device) : device.compile(empty, "NRD-EDSL-REBLUR-Unpack");
    }
    const char *spirv_option = std::getenv("REBLUR_CONVERSION_SPIRV");
    require(!native || (spirv_option && *spirv_option), "native frontend requires REBLUR_CONVERSION_SPIRV");
    const char *backend_spirv_option = std::getenv("REBLUR_CONVERSION_BACKEND_SPIRV");
    require(!native || !full || (backend_spirv_option && *backend_spirv_option), "native full conversion requires REBLUR_CONVERSION_BACKEND_SPIRV");
    const std::array<PixelStorage, 6> storage{PixelStorage::HALF4, PixelStorage::RGB10A2, PixelStorage::FLOAT1,
        PixelStorage::HALF4, PixelStorage::HALF4, PixelStorage::UNORM1};
    auto zeros = std::make_shared<PackedNrdInputs>();
    for (uint slot = 0u; slot != 6u; ++slot) (*zeros)[slot].resize(pixels * pixel_bytes[slot]);
    std::vector<float4> linear_zeros(full ? pixels : 0u);
    uint64_t input_hash = 14695981039346656037ull;
    auto hash = [&](const std::vector<std::byte> &bytes) {
        for (std::byte value : bytes) { input_hash ^= std::to_integer<unsigned char>(value); input_hash *= 1099511628211ull; }
    };
    hash(constant_bytes);
    for (uint layer : {2u, 1u, 0u}) {
        canonical[layer] = std::make_shared<std::vector<std::byte>>(read_bytes(
            input / ("plane" + std::to_string(layer)) / "canonical_inputs.bin", pixels * 96u));
        hash(*canonical[layer]);
        actual[layer] = std::make_shared<PackedNrdInputs>();
        if (full) {
            nrd_outputs[layer] = std::make_shared<PackedNrdOutputs>();
            linear_outputs[layer] = std::make_shared<PackedNrdOutputs>();
            for (uint slot = 0u; slot != 2u; ++slot) {
                (*nrd_outputs[layer])[slot] = read_bytes(input / ("plane" + std::to_string(layer)) /
                    (slot == 0u ? "OUT_DIFF_RADIANCE_HITDIST.bin" : "OUT_SPEC_RADIANCE_HITDIST.bin"), pixels * 8u);
                hash((*nrd_outputs[layer])[slot]);
            }
        }
        if (native) {
            // Defined zero initial contents make sky's untouched bytes stable;
            // these uploads and all canonical copies are outside GPU timings.
            stream << native->upload_replay_inputs(size, layer, zeros)
                   << native->upload_canonical_inputs(size, layer, canonical[layer], spirv_option,
                       nrd_outputs[layer], full ? backend_spirv_option : "");
        } else {
            edsl_inputs[layer] = device.create_byte_buffer(pixels * 96u, "Canonical RTXPT frontend input");
            stream << edsl_inputs[layer].upload(canonical[layer]->data());
            for (uint slot = 0u; slot != 6u; ++slot) {
                images[layer][slot] = device.create_texture2d(size, storage[slot], "Canonical frontend output");
                stream << images[layer][slot].upload((*zeros)[slot].data());
            }
            auto &p = parameters[layer];
            p.external.size = size; p.external.guide = edsl_inputs[layer].descriptor();
            p.constants.gHitDistParams = constants.hit_distance_parameters;
            p.previous_view_depth_row.x = constants.clamp_max;
            const uint output_to_input_slot[]{3u, 4u, 1u, 2u, 0u, 5u};
            for (uint slot = 0u; slot != 6u; ++slot) p.outputs[slot] = {images[layer][output_to_input_slot[slot]], size};
            if (full) for (uint slot = 0u; slot != 2u; ++slot) {
                fixed_nrd_images[layer][slot] = device.create_texture2d(size, PixelStorage::HALF4, "Canonical fixed NRD output");
                linear_buffers[layer][slot] = device.create_buffer<float4>(pixels, "Canonical common linear output");
                stream << fixed_nrd_images[layer][slot].upload((*nrd_outputs[layer])[slot].data())
                       << linear_buffers[layer][slot].upload(linear_zeros.data());
                // A complete write occurs at every pixel, including defined zero sky.
                p.inputs[slot] = {fixed_nrd_images[layer][slot], size};
            }
            if (full) {
                p.external.output_diffuse = linear_buffers[layer][0].descriptor();
                p.external.output_specular = linear_buffers[layer][1].descriptor();
            }
        }
    }
    stream << synchronize() << commit();
    std::cout << "Common conversion scope=" << scope << " backend=" << backend << " size=" << size.x << 'x' << size.y
              << " layers=3 warmup=" << warmup << " measured=" << measured_frames << " canonical_source_frame=64\n"
              << "Timing: Pack" << (full ? "+Unpack" : " only") << "; transfers, endpoint markers and readbacks excluded.\n";
    for (uint iteration = 0u; iteration != warmup + measured_frames; ++iteration) {
        const auto start = std::chrono::steady_clock::now();
        for (uint layer : {2u, 1u, 0u}) {
            if (native) stream << native->dispatch_canonical_conversion(size, layer, constants.hit_distance_parameters, constants.clamp_max);
            else {
                // Match the NRD fixture's separate profiling batch per plane.
                stream << native_callback([](const NativeInteropContext &) {});
                stream << frontend(parameters[layer]).dispatch(size)
                       << endpoint(parameters[layer]).dispatch(size) << unpack(parameters[layer]).dispatch(size);
            }
        }
        stream << synchronize() << commit();
        if (iteration % 16u == 0u || iteration + 1u == warmup + measured_frames)
            std::cout << "Conversion iteration " << iteration + 1u << '/' << warmup + measured_frames << '\n' << std::flush;
        if (pause_ms && iteration + 1u != warmup + measured_frames)
            std::this_thread::sleep_until(start + std::chrono::milliseconds(pause_ms));
    }
    for (uint layer : {2u, 1u, 0u}) {
        if (native) stream << native->readback_replay_inputs(size, layer, actual[layer]);
        else for (uint slot = 0u; slot != 6u; ++slot) {
            (*actual[layer])[slot].resize(pixels * pixel_bytes[slot]);
            stream << images[layer][slot].download((*actual[layer])[slot].data());
        }
        if (full) {
            if (native) stream << native->readback_canonical_outputs(size, layer, linear_outputs[layer]);
            else for (uint slot = 0u; slot != 2u; ++slot) {
                (*linear_outputs[layer])[slot].resize(pixels * 16u);
                stream << linear_buffers[layer][slot].download((*linear_outputs[layer])[slot].data());
            }
        }
    }
    stream << synchronize() << commit();
    native.reset();
    Json comparisons = Json::array(); bool byte_equal = true; uint64_t nonfinite = 0u;
    for (uint layer : {2u, 1u, 0u}) {
        const auto destination = output / ("plane" + std::to_string(layer)); std::filesystem::create_directories(destination);
        for (uint slot = 0u; slot != 6u; ++slot) {
            const auto reference = read_bytes(input / ("plane" + std::to_string(layer)) / (std::string(names[slot]) + ".bin"),
                                              pixels * pixel_bytes[slot]);
            const auto &bytes = (*actual[layer])[slot];
            auto comparison = compare(*canonical[layer], reference, bytes, slot); comparison["layer"] = layer;
            byte_equal &= comparison["byte_equal"].get<bool>(); nonfinite += comparison["nonfinite_components"].get<uint64_t>();
            comparisons.push_back(comparison);
            std::ofstream file(destination / (std::string(names[slot]) + ".bin"), std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
            require(bool(file), "cannot write converted output");
        }
        if (full) for (uint slot = 0u; slot != 2u; ++slot) {
            const auto &bytes = (*linear_outputs[layer])[slot];
            std::ofstream file(destination / (slot == 0u ? "COMMON_DIFFUSE_RGBA32F.bin" : "COMMON_SPECULAR_RGBA32F.bin"), std::ios::binary);
            file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
            require(bool(file), "cannot write common backend output");
        }
    }
    Json report{{"schema", 1}, {"backend", backend}, {"size", {size.x, size.y}}, {"source_frame", 64},
        {"warmup", warmup}, {"measured_iterations", measured_frames}, {"layers", 3}, {"plane_execution_order", {2, 1, 0}},
        {"canonical_bytes_per_pixel", 96}, {"canonical_and_constants_fnv64", input_hash}, {"clamp_max", constants.clamp_max},
        {"conversion_scopes", full ? Json::array({"Pack", "Unpack"}) : Json::array({"Pack"})},
        {"timing_scope", "Pack and enabled common Unpack each include output visibility; transfers and endpoint markers excluded"},
        {"ignored_profile_scopes", {"REBLUR-NRD", "REBLUR-Epilogue", "REBLUR-Filter"}},
        {"backend_semantics", "fixed same-capture NRD outputs; real factors, world-hit restoration; zero sky; extends original RGB-only RTXPT Merge"},
        {"comparison", "Original RTXPT prepared native storage; sky ignored except depth; no inverse or fabricated inputs"},
        {"byte_equal_on_compared_pixels", byte_equal}, {"nonfinite_components", nonfinite}, {"comparisons", comparisons}};
    std::ofstream metadata(output / "conversion.json"); metadata << report.dump(2) << '\n';
    require(bool(metadata), "cannot write conversion metadata");
    require(nonfinite == 0u, "nonfinite component at a compared pixel");
    std::cout << "Common frontend execution complete; byte_equal_on_compared_pixels=" << byte_equal << '\n';
}

} // namespace vision::reblur::real_conversion
