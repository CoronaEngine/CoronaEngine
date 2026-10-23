#pragma once

#include <NRD.h>
#include "ext/nlohmann/json.hpp"
#include "math/half.h"
#include <cstddef>
#include <cstring>
#include <sstream>

// This fixture consumes exact post-PrepareInputs RTXPT texture bytes. It never
// reconstructs Corona guides or runs the Corona Pack/Unpack conversions.
namespace reblur_packed_replay {
using namespace ocarina;
using namespace vision::reblur;
using Json = nlohmann::json;
constexpr std::array<const char *, 6> input_names{
    "IN_MV", "IN_NORMAL_ROUGHNESS", "IN_VIEWZ", "IN_DIFF_RADIANCE_HITDIST",
    "IN_SPEC_RADIANCE_HITDIST", "IN_DISOCCLUSION_THRESHOLD_MIX"};
constexpr std::array<const char *, 6> input_formats{
    "RGBA16_FLOAT", "R10G10B10A2_UNORM", "R32_FLOAT", "RGBA16_FLOAT", "RGBA16_FLOAT", "R8_UNORM"};
constexpr std::array<uint, 6> input_bytes{8u, 4u, 4u, 8u, 8u, 1u};
constexpr std::array<const char *, 2> output_names{"OUT_DIFF_RADIANCE_HITDIST", "OUT_SPEC_RADIANCE_HITDIST"};

inline void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error("Packed replay: " + message);
}
inline Json read_json(const std::filesystem::path &path) {
    std::ifstream file(path);
    require(bool(file), "cannot open " + path.string());
    Json data; file >> data; return data;
}
inline std::vector<std::byte> read_bytes(const std::filesystem::path &path, size_t expected) {
    require(std::filesystem::file_size(path) == expected, "wrong byte size: " + path.string());
    std::vector<std::byte> bytes(expected);
    std::ifstream file(path, std::ios::binary);
    file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(expected));
    require(bool(file), "cannot read " + path.string());
    return bytes;
}
template<typename T> T read_record(const std::filesystem::path &path) {
    const auto bytes = read_bytes(path, sizeof(T));
    T value; std::memcpy(&value, bytes.data(), sizeof(T)); return value;
}
inline void hash_bytes(uint64_t &hash, const void *data, size_t count) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0u; i != count; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
}
inline std::string hash_text(uint64_t hash) {
    std::ostringstream text; text << std::hex << std::setw(16) << std::setfill('0') << hash; return text.str();
}
inline std::filesystem::path plane_path(const std::filesystem::path &root, uint frame, uint layer) {
    std::ostringstream name; name << "frame" << std::setw(6) << std::setfill('0') << frame;
    return root / name.str() / ("plane" + std::to_string(layer));
}
struct Capture {
    std::filesystem::path root;
    uint2 size{};
    uint frames{};
};
inline Capture open_capture(const std::filesystem::path &root) {
    const auto complete = read_json(root / "capture_complete.json");
    require(complete.at("schema") == 1 && complete.at("first_frame") == 0 &&
        complete.at("plane_execution_order") == Json::array({2, 1, 0}), "unsupported capture protocol");
    Capture capture{root, {}, complete.at("frames").get<uint>()};
    require(capture.frames && complete.at("last_frame").get<uint>() == capture.frames - 1u,
        "capture sequence is incomplete");
    const auto layout = read_json(root / "settings_layout.json");
    require(layout.at("common_sizeof") == sizeof(nrd::CommonSettings) &&
        layout.at("reblur_sizeof") == sizeof(nrd::ReblurSettings), "NRD settings ABI size mismatch");
#define REPLAY_COMMON_OFFSET(field) require(layout.at("common_offsets").at(#field) == offsetof(nrd::CommonSettings, field), "CommonSettings ABI offset mismatch: " #field)
    REPLAY_COMMON_OFFSET(viewToClipMatrix); REPLAY_COMMON_OFFSET(viewToClipMatrixPrev);
    REPLAY_COMMON_OFFSET(worldToViewMatrix); REPLAY_COMMON_OFFSET(worldToViewMatrixPrev);
    REPLAY_COMMON_OFFSET(worldPrevToWorldMatrix); REPLAY_COMMON_OFFSET(motionVectorScale);
    REPLAY_COMMON_OFFSET(cameraJitter); REPLAY_COMMON_OFFSET(cameraJitterPrev);
    REPLAY_COMMON_OFFSET(resourceSize); REPLAY_COMMON_OFFSET(resourceSizePrev);
    REPLAY_COMMON_OFFSET(rectSize); REPLAY_COMMON_OFFSET(rectSizePrev);
    REPLAY_COMMON_OFFSET(viewZScale); REPLAY_COMMON_OFFSET(timeDeltaBetweenFrames);
    REPLAY_COMMON_OFFSET(denoisingRange); REPLAY_COMMON_OFFSET(disocclusionThreshold);
    REPLAY_COMMON_OFFSET(disocclusionThresholdAlternate); REPLAY_COMMON_OFFSET(cameraAttachedReflectionMaterialID);
    REPLAY_COMMON_OFFSET(strandMaterialID); REPLAY_COMMON_OFFSET(strandThickness);
    REPLAY_COMMON_OFFSET(splitScreen); REPLAY_COMMON_OFFSET(printfAt); REPLAY_COMMON_OFFSET(debug);
    REPLAY_COMMON_OFFSET(rectOrigin); REPLAY_COMMON_OFFSET(frameIndex); REPLAY_COMMON_OFFSET(accumulationMode);
    REPLAY_COMMON_OFFSET(isMotionVectorInWorldSpace); REPLAY_COMMON_OFFSET(isHistoryConfidenceAvailable);
    REPLAY_COMMON_OFFSET(isDisocclusionThresholdMixAvailable); REPLAY_COMMON_OFFSET(isBaseColorMetalnessAvailable);
    REPLAY_COMMON_OFFSET(enableValidation);
#undef REPLAY_COMMON_OFFSET
#define REPLAY_METHOD_OFFSET(field) require(layout.at("reblur_offsets").at(#field) == offsetof(nrd::ReblurSettings, field), "ReblurSettings ABI offset mismatch: " #field)
    REPLAY_METHOD_OFFSET(hitDistanceParameters); REPLAY_METHOD_OFFSET(antilagSettings);
    REPLAY_METHOD_OFFSET(maxAccumulatedFrameNum); REPLAY_METHOD_OFFSET(maxFastAccumulatedFrameNum);
    REPLAY_METHOD_OFFSET(maxStabilizedFrameNum); REPLAY_METHOD_OFFSET(historyFixFrameNum);
    REPLAY_METHOD_OFFSET(historyFixBasePixelStride); REPLAY_METHOD_OFFSET(diffusePrepassBlurRadius);
    REPLAY_METHOD_OFFSET(specularPrepassBlurRadius); REPLAY_METHOD_OFFSET(minHitDistanceWeight);
    REPLAY_METHOD_OFFSET(minBlurRadius); REPLAY_METHOD_OFFSET(maxBlurRadius);
    REPLAY_METHOD_OFFSET(lobeAngleFraction); REPLAY_METHOD_OFFSET(roughnessFraction);
    REPLAY_METHOD_OFFSET(responsiveAccumulationRoughnessThreshold); REPLAY_METHOD_OFFSET(planeDistanceSensitivity);
    REPLAY_METHOD_OFFSET(specularProbabilityThresholdsForMvModification); REPLAY_METHOD_OFFSET(fireflySuppressorMinRelativeScale);
    REPLAY_METHOD_OFFSET(checkerboardMode); REPLAY_METHOD_OFFSET(hitDistanceReconstructionMode);
    REPLAY_METHOD_OFFSET(enableAntiFirefly); REPLAY_METHOD_OFFSET(enablePerformanceMode);
    REPLAY_METHOD_OFFSET(minMaterialForDiffuse); REPLAY_METHOD_OFFSET(minMaterialForSpecular);
    REPLAY_METHOD_OFFSET(usePrepassOnlyForSpecularMotionEstimation);
#undef REPLAY_METHOD_OFFSET
    require(layout.at("hit_distance_parameters_sizeof") == sizeof(nrd::HitDistanceParameters) &&
        layout.at("antilag_settings_sizeof") == sizeof(nrd::ReblurAntilagSettings), "nested NRD settings ABI size mismatch");
#define REPLAY_NESTED_OFFSET(group, type, field) require(layout.at(group).at(#field) == offsetof(type, field), "nested NRD settings ABI offset mismatch: " #field)
    REPLAY_NESTED_OFFSET("hit_distance_parameters_offsets", nrd::HitDistanceParameters, A);
    REPLAY_NESTED_OFFSET("hit_distance_parameters_offsets", nrd::HitDistanceParameters, B);
    REPLAY_NESTED_OFFSET("hit_distance_parameters_offsets", nrd::HitDistanceParameters, C);
    REPLAY_NESTED_OFFSET("hit_distance_parameters_offsets", nrd::HitDistanceParameters, D);
    REPLAY_NESTED_OFFSET("antilag_settings_offsets", nrd::ReblurAntilagSettings, luminanceSigmaScale);
    REPLAY_NESTED_OFFSET("antilag_settings_offsets", nrd::ReblurAntilagSettings, luminanceSensitivity);
#undef REPLAY_NESTED_OFFSET
    const auto first = read_record<nrd::CommonSettings>(plane_path(root, 0u, 2u) / "common_settings.bin");
    capture.size = make_uint2(first.resourceSize[0], first.resourceSize[1]);
    require(capture.size.x && capture.size.y, "empty capture dimensions");
    return capture;
}
struct Frame {
    nrd::CommonSettings common{};
    nrd::ReblurSettings method{};
    std::shared_ptr<PackedNrdInputs> inputs{std::make_shared<PackedNrdInputs>()};
};
inline Frame read_frame(const Capture &capture, uint frame, uint layer, uint64_t &hash) {
    const auto directory = plane_path(capture.root, frame, layer);
    Frame result;
    const auto common_bytes = read_bytes(directory / "common_settings.bin", sizeof(result.common));
    const auto method_bytes = read_bytes(directory / "method_settings.bin", sizeof(result.method));
    std::memcpy(&result.common, common_bytes.data(), common_bytes.size());
    std::memcpy(&result.method, method_bytes.data(), method_bytes.size());
    const auto &common = result.common;
    require(common.frameIndex == frame && common.resourceSize[0] == capture.size.x &&
        common.resourceSize[1] == capture.size.y && common.rectSize[0] == capture.size.x &&
        common.rectSize[1] == capture.size.y && common.rectOrigin[0] == 0u && common.rectOrigin[1] == 0u,
        "frame/settings resolution or sequence mismatch");
    require(std::isfinite(common.denoisingRange) && common.denoisingRange > 0.f &&
        std::isfinite(common.viewZScale) && common.viewZScale > 0.f && common.timeDeltaBetweenFrames > 0.f,
        "invalid captured depth range/scale or frame delta");
    require(!common.enableValidation && !common.isHistoryConfidenceAvailable && !common.isBaseColorMetalnessAvailable,
        "capture requests optional textures absent from the six-input protocol");
    require(result.method.checkerboardMode == nrd::CheckerboardMode::OFF && !result.method.enablePerformanceMode,
        "capture is not full-resolution quality REBLUR");
    hash_bytes(hash, common_bytes.data(), common_bytes.size()); hash_bytes(hash, method_bytes.data(), method_bytes.size());
    const size_t pixels = size_t(capture.size.x) * capture.size.y;
    for (uint slot = 0u; slot != input_names.size(); ++slot) {
        const auto path = directory / (std::string(input_names[slot]) + ".bin");
        const auto meta = read_json(path.string() + ".json");
        require(meta.at("format") == input_formats[slot] && meta.at("width") == capture.size.x &&
            meta.at("height") == capture.size.y && meta.at("packed_row_pitch") == size_t(capture.size.x) * input_bytes[slot] &&
            meta.at("bytes") == pixels * input_bytes[slot], "texture layout mismatch: " + path.string());
        (*result.inputs)[slot] = read_bytes(path, pixels * input_bytes[slot]);
        hash_bytes(hash, (*result.inputs)[slot].data(), (*result.inputs)[slot].size());
    }
    return result;
}
inline void validate_capture(const std::filesystem::path &root) {
    const auto capture = open_capture(root);
    uint64_t hash = 14695981039346656037ull;
    for (uint frame = 0u; frame != capture.frames; ++frame)
        for (uint layer : {2u, 1u, 0u}) (void)read_frame(capture, frame, layer, hash);
    std::cout << "PASS: packed capture validated frames=" << capture.frames << " size=" << capture.size.x << 'x'
              << capture.size.y << " input_fnv64=" << hash_text(hash) << '\n';
}
inline float half_value(const std::byte *data, size_t component) {
    uint16_t value; std::memcpy(&value, data + component * 2u, 2u); return half_to_float(value);
}
inline std::array<float, 4> linear_pixel(const std::vector<std::byte> &bytes, size_t pixel) {
    const float y = half_value(bytes.data(), pixel * 4u), co = half_value(bytes.data(), pixel * 4u + 1u),
        cg = half_value(bytes.data(), pixel * 4u + 2u), hit = half_value(bytes.data(), pixel * 4u + 3u);
    return {std::max(y - cg + co, 0.f), std::max(y + cg, 0.f), std::max(y - cg - co, 0.f), hit};
}
struct Oracle {
    uint64_t valid_pixels{}, ignored_pixels{}, components{}, nonfinite_components{}, mismatch_components{};
    double max_abs{}, squared_error{};
    void compare(const Frame &frame, const std::vector<std::byte> &reference, const std::vector<std::byte> &actual) {
        const auto &depth = (*frame.inputs)[2];
        for (size_t pixel = 0u; pixel != depth.size() / 4u; ++pixel) {
            float z; std::memcpy(&z, depth.data() + pixel * 4u, 4u);
            z = std::abs(z * frame.common.viewZScale);
            if (!std::isfinite(z) || z > frame.common.denoisingRange) { ++ignored_pixels; continue; }
            ++valid_pixels;
            const auto a = linear_pixel(reference, pixel), b = linear_pixel(actual, pixel);
            for (uint c = 0u; c != 4u; ++c) {
                ++components;
                // Check the packed source components too: clamping during
                // YCoCg conversion must never hide a nonfinite valid pixel.
                if (!std::isfinite(half_value(reference.data(), pixel * 4u + c)) ||
                    !std::isfinite(half_value(actual.data(), pixel * 4u + c)) ||
                    !std::isfinite(a[c]) || !std::isfinite(b[c])) {
                    ++nonfinite_components; ++mismatch_components; continue;
                }
                const double error = std::abs(double(b[c]) - double(a[c]));
                max_abs = std::max(max_abs, error); squared_error += error * error;
                mismatch_components += uint64_t(error > .001 + .002 * std::abs(double(a[c])));
            }
        }
    }
    Json json() const {
        return {{"valid_pixels", valid_pixels}, {"ignored_sky_pixels", ignored_pixels}, {"components", components},
            {"nonfinite_components", nonfinite_components}, {"mismatch_components", mismatch_components},
            {"max_abs", max_abs}, {"rmse", components ? std::sqrt(squared_error / double(components)) : 0.}};
    }
};
inline void run(Device &device, Stream &stream, const std::string &backend, uint warmup, uint pause_ms) {
    const auto capture = open_capture(std::getenv("REBLUR_REPLAY_CAPTURE_DIR"));
    require(warmup < capture.frames, "warmup must leave at least one measured captured frame");
    for (const auto &[name, expected] : {std::pair{"REBLUR_REPLAY_WIDTH", capture.size.x},
        std::pair{"REBLUR_REPLAY_HEIGHT", capture.size.y}, std::pair{"REBLUR_REPLAY_FRAMES", capture.frames - warmup}}) {
        if (const char *value = std::getenv(name); value && *value) {
            char *end{}; const auto parsed = std::strtoull(value, &end, 10);
            require(value[0] != '-' && end && !*end && parsed == expected,
                std::string(name) + " disagrees with the recorded complete sequence");
        }
    }
    const char *output_path = std::getenv("REBLUR_REPLAY_OUTPUT_DIR");
    require(output_path && *output_path, "REBLUR_REPLAY_OUTPUT_DIR is required for packed replay evidence");
    const char *validation_option = std::getenv("REBLUR_REPLAY_VALIDATE_OUTPUTS");
    require(!validation_option || !*validation_option || std::strcmp(validation_option, "0") == 0 ||
        std::strcmp(validation_option, "1") == 0, "REBLUR_REPLAY_VALIDATE_OUTPUTS must be 0 or 1");
    const bool validate_all_frames = !validation_option || std::strcmp(validation_option, "0") != 0;
    const std::filesystem::path output = output_path; std::filesystem::create_directories(output);
    std::unique_ptr<NativeReblur> native;
    std::unique_ptr<EdslReblur> edsl;
    if (backend == "native") native = std::make_unique<NativeReblur>(device.context()->runtime_directory() / "reblur-native");
    else edsl = std::make_unique<EdslReblur>(device);
    std::cout << "REBLUR packed-input replay backend=" << backend << " size=" << capture.size.x << 'x' << capture.size.y
              << " layers=3 warmup=" << warmup << " measure_frames=" << capture.frames - warmup
              << " frame_pause_ms=" << pause_ms << '\n'
              << "Timing: NRD only; uploads/readbacks excluded; Pack/Unpack names are empty replay boundaries, not conversions.\n"
              << "Replay validation=" << (validate_all_frames ? "all_frames" : "final_frame_only")
              << " pacing=minimum_frame_start_interval_ms\n";
    uint64_t hash = 14695981039346656037ull;
    std::array<std::array<Oracle, 2>, 3> oracles;
    Json frame_records = Json::array();
    for (uint index = 0u; index != capture.frames; ++index) {
        const auto frame_start = std::chrono::steady_clock::now();
        std::array<Frame, 3> frames;
        for (uint layer : {2u, 1u, 0u}) {
            frames[layer] = read_frame(capture, index, layer, hash);
            if (native) stream << native->upload_replay_inputs(capture.size, layer, frames[layer].inputs);
            else stream << edsl->upload_replay_inputs(capture.size, layer, frames[layer].inputs);
        }
        stream << synchronize() << commit();
        for (uint layer : {2u, 1u, 0u}) {
            if (native) stream << native->replay_dispatch(capture.size, layer, frames[layer].common, frames[layer].method);
            else stream << edsl->replay_dispatch(capture.size, layer, frames[layer].common, frames[layer].method);
        }
        stream << synchronize() << commit();
        if (validate_all_frames || index + 1u == capture.frames) {
            std::array<std::shared_ptr<PackedNrdOutputs>, 3> results;
            for (uint layer : {2u, 1u, 0u}) {
                results[layer] = std::make_shared<PackedNrdOutputs>();
                if (native) stream << native->readback_replay_outputs(capture.size, layer, results[layer]);
                else stream << edsl->readback_replay_outputs(capture.size, layer, results[layer]);
            }
            stream << synchronize() << commit();
            for (uint layer : {2u, 1u, 0u}) {
                const auto destination = plane_path(output, index, layer); std::filesystem::create_directories(destination);
                for (uint signal = 0u; signal != 2u; ++signal) {
                    const auto &actual = (*results[layer])[signal];
                    const auto reference = read_bytes(plane_path(capture.root, index, layer) /
                        (std::string(output_names[signal]) + ".bin"), actual.size());
                    oracles[layer][signal].compare(frames[layer], reference, actual);
                    const auto path = destination / (std::string(output_names[signal]) + ".bin");
                    std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char *>(actual.data()), std::streamsize(actual.size()));
                    require(bool(file), "cannot write " + path.string());
                }
            }
        }
        frame_records.push_back({{"frame", index}, {"phase", index < warmup ? "warmup" : "measure"}, {"cumulative_input_fnv64", hash_text(hash)}});
        if (index % 16u == 0u || index + 1u == capture.frames) std::cout << "Replay frame " << index + 1u << '/' << capture.frames << '\n' << std::flush;
        if (pause_ms && index + 1u != capture.frames)
            std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(pause_ms));
    }
    native.reset(); edsl.reset();
    Json oracle = Json::array(); uint64_t mismatches = 0u, nonfinite = 0u;
    for (uint layer = 0u; layer != 3u; ++layer) for (uint signal = 0u; signal != 2u; ++signal) {
        auto record = oracles[layer][signal].json(); record["layer"] = layer; record["signal"] = output_names[signal];
        oracle.push_back(record); mismatches += oracles[layer][signal].mismatch_components;
        nonfinite += oracles[layer][signal].nonfinite_components;
    }
    Json report{{"schema", 1}, {"backend", backend}, {"capture", std::filesystem::absolute(capture.root).string()},
        {"size", {capture.size.x, capture.size.y}}, {"frames", capture.frames}, {"warmup", warmup}, {"layers", 3},
        {"plane_execution_order", {2, 1, 0}}, {"input_fnv64", hash_text(hash)}, {"frame_records", frame_records},
        {"validation_mode", validate_all_frames ? "all_frames" : "final_frame_only"},
        {"output_frame_count", validate_all_frames ? capture.frames : 1u},
        {"pacing", {{"mode", "minimum_frame_start_interval"}, {"milliseconds", pause_ms}}},
        {"timing_scope", "REBLUR-NRD only; packed uploads/readbacks and empty boundary dispatches excluded"},
        {"reference", "captured RTXPT D3D12 packed output decoded to linear RGB and normalized hit distance"},
        {"tolerance", {{"absolute", .001}, {"relative", .002}}}, {"oracles", oracle}, {"mismatch_components", mismatches}};
    std::ofstream metadata(output / "packed_replay.json"); metadata << report.dump(2) << '\n';
    require(bool(metadata), "cannot write packed replay metadata");
    require(nonfinite == 0u, "nonfinite output component at a valid NRD pixel; see packed_replay.json");
    std::cout << "Input combined_fnv64=" << hash_text(hash) << " order=frame0..N(plane2,1,0)(CommonSettings,ReblurSettings,MV,Normal,Z,Diffuse,Specular,Mix)\n"
              << "RTXPT oracle mismatch_components=" << mismatches << '\n'
              << "PASS: packed replay completed; three independent instances; no timed upload/readback\n";
}
} // namespace reblur_packed_replay
