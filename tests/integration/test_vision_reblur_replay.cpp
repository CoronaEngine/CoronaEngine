#include "render_core/denoiser/ReBLUR/edsl_nrd.h"
#include "render_core/denoiser/ReBLUR/native_nrd.h"
#include "rhi/context.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "reblur_packed_replay.h"
#include "reblur_real_conversion.h"

// Fixed-input replay for the native NRD and original-algorithm EDSL backends.
// Run each backend in its own process, with the same replay environment.
// Inputs upload once, remain immutable, and use separate output allocations.
// Per-pass GPU timing is supplied by each backend's profiler; readback occurs
// only after the last measured frame and is excluded from the frame timings.
namespace {
using namespace ocarina;
using namespace vision::reblur;
constexpr uint64_t fnv_offset = 14695981039346656037ull;
constexpr uint64_t fnv_prime = 1099511628211ull;
constexpr uint layer_count = 3u;

uint option(const char *name, uint fallback, bool allow_zero = false) {
    const char *text = std::getenv(name);
    if (!text || !*text) return fallback;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (text[0] == '-' || !end || *end || value > UINT32_MAX || (!allow_zero && value == 0u))
        throw std::runtime_error(std::string("Invalid replay option: ") + name);
    return static_cast<uint>(value);
}

void hash_bytes(uint64_t &hash, const void *data, size_t bytes) {
    auto first = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i != bytes; ++i) { hash ^= first[i]; hash *= fnv_prime; }
}

uint64_t byte_hash(const void *data, size_t bytes) {
    uint64_t hash = fnv_offset;
    hash_bytes(hash, data, bytes);
    return hash;
}

std::string hex_hash(uint64_t hash) {
    char output[17]{};
    constexpr char digits[] = "0123456789abcdef";
    for (uint i = 0u; i != 16u; ++i) output[15u - i] = digits[(hash >> (i * 4u)) & 15u];
    return output;
}

void print_matrix(const char *name, const std::array<float, 16> &matrix) {
    std::cout << name << " (column-major bytes, displayed as rows):\n";
    for (uint row = 0; row != 4u; ++row) {
        std::cout << "  ";
        for (uint column = 0; column != 4u; ++column) std::cout << matrix[column * 4u + row] << (column == 3u ? '\n' : ',');
    }
}

void print_settings(const Settings &s) {
    std::cout << "Settings: max_history=" << s.max_history << " fast_history=" << s.fast_history
              << " history_fix_frames=" << s.history_fix_frames << " history_fix_stride=" << s.history_fix_stride
              << " diffuse_prepass=" << s.diffuse_prepass << " specular_prepass=" << s.specular_prepass
              << " min_radius=" << s.min_radius << " max_radius=" << s.max_radius
              << " lobe_fraction=" << s.lobe_fraction << " roughness_fraction=" << s.roughness_fraction
              << " plane_sensitivity=" << s.plane_sensitivity << " min_hit_weight=" << s.min_hit_weight
              << " stabilization=" << s.stabilization << " antilag_sigma=" << s.antilag_sigma
              << " antilag_sensitivity=" << s.antilag_sensitivity << " anti_firefly=" << s.anti_firefly
              << " reconstruction=" << static_cast<uint>(s.reconstruction) << " (AREA_5X5)\n";
}

struct LayerInput {
    ocarina::ByteBuffer guide;
    Buffer<float4> diffuse, specular, output_diffuse, output_specular;
};

uint hash_pattern(uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

std::array<LayerInput, layer_count> make_inputs(Device &device, Stream &stream, uint2 size) {
    const uint count = size.x * size.y;
    const float aspect = float(size.x) / float(size.y);
    std::array<LayerInput, layer_count> layers;
    uint64_t combined_hash = fnv_offset;
    for (uint layer = 0u; layer != layer_count; ++layer) {
        auto &input = layers[layer];
        input.guide = device.create_byte_buffer(size_t(count) * sizeof(Guide), "REBLUR replay immutable guides");
        input.diffuse = device.create_buffer<float4>(count, "REBLUR replay immutable diffuse");
        input.specular = device.create_buffer<float4>(count, "REBLUR replay immutable specular");
        input.output_diffuse = device.create_buffer<float4>(count, "REBLUR replay diffuse output");
        input.output_specular = device.create_buffer<float4>(count, "REBLUR replay specular output");
        std::vector<Guide> guides(count);
        std::vector<float4> diffuse(count), specular(count);
        uint invalid_count = 0u, zero_d_count = 0u, zero_s_count = 0u;
        const float z = 10.f + float(layer);
        const float k = float(layer + 1u);
        for (uint y = 0u; y != size.y; ++y) for (uint x = 0u; x != size.x; ++x) {
            const uint i = y * size.x + x;
            auto &g = guides[i];
            g = Guide{};
            g.position = make_float4((2.f * (float(x) + .5f) / float(size.x) - 1.f) * aspect * z,
                (1.f - 2.f * (float(y) + .5f) / float(size.y)) * z, z, z);
            g.identity = make_uint4(layer + 1u, 7u, 3u, 1u);
            const float roughness = .05f + .9f * float(x) / float(std::max(size.x - 1u, 1u));
            g.normal_roughness = make_float4(0.f, 0.f, -1.f, roughness);
            g.diffuse_factor = make_float4(.6f, .8f, 1.f, 0.f);
            g.specular_factor = make_float4(.8f, .7f, .6f, 0.f);
            const uint hash = hash_pattern(x + y * 65537u + layer * 104729u + 17u);
            const float p = float(hash & 255u) / 255.f;
            const float q = float((hash >> 8u) & 255u) / 255.f;
            const float r = float((hash >> 16u) & 255u) / 255.f;
            const bool zero_d = i % 11u == 0u, zero_s = i % 13u == 0u;
            diffuse[i] = make_float4((.2f + p) * k, (.3f + .5f * q) * k, (.4f + .25f * r) * k, zero_d ? 0.f : 1.f + 3.f * q);
            specular[i] = make_float4((.3f + .75f * r) * k, (.1f + p) * k, (.2f + .5f * q) * k, zero_s ? 0.f : 2.f + 4.f * r);
            // Whole 16x16 tiles are sky, exercising ClassifyTiles without
            // changing the input between warmup and measurement.
            if (((x / 16u) + 3u * (y / 16u) + layer) % 11u == 0u) {
                g.position = make_float4(0.f);
                g.identity.w = 0u;
                diffuse[i] = make_float4(.1f, .2f, .4f, -1.f);
                specular[i] = make_float4(.05f, .1f, .2f, -2.f);
                ++invalid_count;
            } else {
                zero_d_count += uint(zero_d); zero_s_count += uint(zero_s);
            }
        }
        HostByteBuffer packed;
        DynamicBufferLayoutCodec<Guide>::encode(guides.data(), count, packed, {}, DynamicBufferLayout::SOA);
        const size_t signal_bytes = size_t(count) * sizeof(float4);
        std::cout << "Input layer=" << layer << " guide_bytes=" << packed.size() << " signal_bytes_each=" << signal_bytes
                  << " invalid_pixels=" << invalid_count << " zero_diffuse_hits=" << zero_d_count << " zero_specular_hits=" << zero_s_count
                  << " guide_fnv64=" << hex_hash(byte_hash(packed.data(), packed.size()))
                  << " diffuse_fnv64=" << hex_hash(byte_hash(diffuse.data(), signal_bytes))
                  << " specular_fnv64=" << hex_hash(byte_hash(specular.data(), signal_bytes)) << '\n';
        hash_bytes(combined_hash, packed.data(), packed.size());
        hash_bytes(combined_hash, diffuse.data(), signal_bytes);
        hash_bytes(combined_hash, specular.data(), signal_bytes);
        // Complete uploads while all host source buffers remain alive.
        stream << input.guide.upload(packed.data()) << input.diffuse.upload(diffuse.data()) << input.specular.upload(specular.data())
               << synchronize() << commit();
    }
    std::cout << "Input combined_fnv64=" << hex_hash(combined_hash) << " order=layer0..2(GuideSOA,diffuseRGBA32F,specularRGBA32F)\n";
    return layers;
}

void print_output(const char *name, uint layer, uint2 size, const std::vector<float4> &values, uint64_t &combined_hash) {
    const size_t bytes = values.size() * sizeof(float4);
    hash_bytes(combined_hash, values.data(), bytes);
    double rgb_sum = 0.0, hit_sum = 0.0;
    size_t finite = 0u, nonzero = 0u;
    for (const auto &v : values) for (uint c = 0u; c != 4u; ++c) {
        if (std::isfinite(v[c])) {
            ++finite;
            if (c == 3u) hit_sum += double(v[c]); else rgb_sum += double(v[c]);
        }
        nonzero += size_t(v[c] != 0.f);
    }
    // This fixed fixture supplies positive radiance at every valid pixel.
    // A late reset clear used to leave most first-layer pixels exactly zero
    // when timestamp queries were disabled, despite valid whole-image sums.
    size_t zero_valid_pixels = 0u;
    for (uint y = 0u; y != size.y; ++y) for (uint x = 0u; x != size.x; ++x) {
        if (((x / 16u) + 3u * (y / 16u) + layer) % 11u == 0u) continue;
        const auto &v = values[size_t(y) * size.x + x];
        zero_valid_pixels += size_t(v.x == 0.f && v.y == 0.f && v.z == 0.f);
    }
    if (zero_valid_pixels) throw std::runtime_error("Replay lost positive radiance at valid pixels");
    std::cout << "Output layer=" << layer << " signal=" << name << " fnv64=" << hex_hash(byte_hash(values.data(), bytes))
              << " rgb_sum=" << rgb_sum << " hit_sum=" << hit_sum << " finite_components=" << finite
              << '/' << values.size() * 4u << " nonzero_components=" << nonzero << '\n';
    if (finite != values.size() * 4u || nonzero == 0u) throw std::runtime_error("Replay output is nonfinite or entirely zero");
}

void run(Device &device, Stream &stream) {
    const char *backend_environment = std::getenv("REBLUR_REPLAY_BACKEND");
    const std::string backend = backend_environment ? backend_environment : "edsl";
    if (backend != "native" && backend != "edsl") throw std::runtime_error("REBLUR_REPLAY_BACKEND must be native or edsl");
    if (const char *canonical = std::getenv("REBLUR_CONVERSION_INPUT_DIR"); canonical && *canonical) {
        vision::reblur::real_conversion::run(device, stream, backend,
            option("REBLUR_CONVERSION_WARMUP", 64u, true), option("REBLUR_CONVERSION_FRAMES", 128u),
            option("REBLUR_CONVERSION_FRAME_PAUSE_MS", 250u, true));
        return;
    }
    if (const char *capture = std::getenv("REBLUR_REPLAY_CAPTURE_DIR"); capture && *capture) {
        reblur_packed_replay::run(device, stream, backend, option("REBLUR_REPLAY_WARMUP", 64u, true),
            option("REBLUR_REPLAY_FRAME_PAUSE_MS", 250u, true));
        return;
    }
    const uint2 size = make_uint2(option("REBLUR_REPLAY_WIDTH", 1280u), option("REBLUR_REPLAY_HEIGHT", 720u));
    const uint warmup = option("REBLUR_REPLAY_WARMUP", 64u, true);
    const uint frames = option("REBLUR_REPLAY_FRAMES", 32u);
    const uint pause_ms = option("REBLUR_REPLAY_FRAME_PAUSE_MS", 250u, true);
    if (size.x > UINT16_MAX || size.y > UINT16_MAX || uint64_t(size.x) * size.y > UINT32_MAX ||
        uint64_t(warmup) + frames > UINT32_MAX) throw std::runtime_error("Replay dimensions/frame count exceed NRD limits");
    std::cout << std::setprecision(std::numeric_limits<float>::max_digits10)
              << "REBLUR fixed-input replay backend=" << backend << " size=" << size.x << 'x' << size.y
              << " layers=" << layer_count << " warmup=" << warmup << " measure_frames=" << frames << " frame_pause_ms=" << pause_ms << '\n';
    std::cout << "Input: stationary perspective camera, FOV_y=90deg, planes at viewZ=10/11/12, normal=(0,0,-1), "
                 "roughness=0.05..0.95 along x; fixed integer-hash RGB, sparse zero hits (1/11 diffuse,1/13 specular), "
                 "whole invalid 16x16 tiles, zero motion, separate immutable inputs/outputs.\n";
    for (const char *name : {"REBLUR_GPU_PROFILE", "REBLUR_GPU_PROFILE_MODE", "REBLUR_NATIVE_PROFILE_PASSES"}) {
        const char *value = std::getenv(name);
        std::cout << "Profiler " << name << '=' << (value ? value : "(unset)") << '\n';
    }
    Settings settings;
    print_settings(settings);
    NativeCameraParams camera;
    camera.world_to_view = camera.world_to_view_prev =
        {1.f,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,1.f,0.f, 0.f,0.f,0.f,1.f};
    const float aspect = float(size.x) / float(size.y), depth = 1000.f / 999.9f;
    camera.view_to_clip = camera.view_to_clip_prev =
        {1.f/aspect,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,depth,1.f, 0.f,0.f,-.1f*depth,0.f};
    camera.frame_delta_ms = 1000.f / 60.f;
    std::cout << "Camera: frame_delta_ms=" << camera.frame_delta_ms << " denoising_range=" << camera.denoising_range
              << " disocclusion_threshold=" << camera.disocclusion_threshold
              << " disocclusion_threshold_alternate=" << camera.disocclusion_threshold_alternate
              << " jitter=" << camera.jitter[0] << ',' << camera.jitter[1]
              << " jitter_prev=" << camera.jitter_prev[0] << ',' << camera.jitter_prev[1] << '\n';
    print_matrix("world_to_view", camera.world_to_view); print_matrix("world_to_view_prev", camera.world_to_view_prev);
    print_matrix("view_to_clip", camera.view_to_clip); print_matrix("view_to_clip_prev", camera.view_to_clip_prev);
    auto layers = make_inputs(device, stream, size);
    std::unique_ptr<NativeReblur> native;
    std::unique_ptr<EdslReblur> edsl;
    if (backend == "native") native = std::make_unique<NativeReblur>(device.context()->runtime_directory() / "reblur-native");
    else edsl = std::make_unique<EdslReblur>(device);
    std::cout << "Replay: GPU profiler owns pass timings; wall matrix below is host enqueue+frame sync only, excluding pauses/readback.\n";
    std::cout.flush();
    const uint total = warmup + frames;
    std::vector<double> wall_ms(total);
    for (uint frame = 0u; frame != total; ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        for (uint layer = 0u; layer != layer_count; ++layer) {
            const auto &input = layers[layer];
            FilterParam p;
            p.size = size; p.frame = frame; p.history_valid = uint(frame != 0u);
            p.guide = input.guide.descriptor();
            p.raw_diffuse = input.diffuse.descriptor(); p.raw_specular = input.specular.descriptor();
            p.output_diffuse = input.output_diffuse.descriptor(); p.output_specular = input.output_specular.descriptor();
            if (native) stream << native->dispatch(p, camera, layer, settings, frame == 0u);
            else stream << edsl->dispatch(p, camera, layer, settings, frame == 0u);
        }
        stream << synchronize() << commit();
        wall_ms[frame] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        if (pause_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
    }
    std::cout << "Wall timing matrix: frame,phase,three_layer_enqueue_and_sync_ms\n";
    for (uint frame = 0u; frame != total; ++frame)
        std::cout << frame << ',' << (frame < warmup ? "warmup" : "measure") << ',' << wall_ms[frame] << '\n';
    // The first and only output downloads are after all measured frames.
    std::array<std::vector<float4>, layer_count> final_d, final_s;
    for (uint layer = 0u; layer != layer_count; ++layer) {
        final_d[layer].resize(size_t(size.x) * size.y); final_s[layer].resize(size_t(size.x) * size.y);
        stream << layers[layer].output_diffuse.download(final_d[layer].data())
               << layers[layer].output_specular.download(final_s[layer].data());
    }
    stream << synchronize() << commit();
    uint64_t output_hash = fnv_offset;
    std::filesystem::path output_directory;
    if (const char *path = std::getenv("REBLUR_REPLAY_OUTPUT_DIR"); path && *path) {
        output_directory = path;
        std::filesystem::create_directories(output_directory);
    }
    for (uint layer = 0u; layer != layer_count; ++layer) {
        print_output("diffuse", layer, size, final_d[layer], output_hash);
        print_output("specular", layer, size, final_s[layer], output_hash);
        if (!output_directory.empty()) {
            auto save = [&](const char *signal, const std::vector<float4> &pixels) {
                std::ofstream file(output_directory / (std::string(signal) + "_l" + std::to_string(layer) + ".rgba32f"), std::ios::binary);
                file.write(reinterpret_cast<const char *>(pixels.data()), std::streamsize(pixels.size() * sizeof(float4)));
                if (!file) throw std::runtime_error("cannot write replay output");
            };
            save("diffuse", final_d[layer]); save("specular", final_s[layer]);
        }
    }
    std::cout << "Output combined_fnv64=" << hex_hash(output_hash) << " order=layer0..2(diffuseRGBA32F,specularRGBA32F)\n";
    // Destroy the backend now so pending GPU profile samples flush before exit.
    native.reset(); edsl.reset();
    std::cout << "PASS: replay completed; immutable inputs, three independent histories, no timed-loop readback\n";
}
} // namespace

int main() {
    try {
        using namespace ocarina;
        if (const char *capture = std::getenv("REBLUR_REPLAY_CAPTURE_DIR"); capture && *capture &&
            std::getenv("REBLUR_REPLAY_CAPTURE_VALIDATE_ONLY")) {
            reblur_packed_replay::validate_capture(capture);
            return 0;
        }
        Env::set_valid_check(false);
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        auto device = RHIContext::instance().create_device();
        auto stream = device.create_stream();
        run(device, stream);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
