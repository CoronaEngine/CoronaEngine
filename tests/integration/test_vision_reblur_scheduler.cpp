// CPU-only REBLUR scheduler contract. The optional official oracle is loaded
// explicitly at runtime; ordinary builds/tests need no external NRD SDK or DLL.
#include <NRD.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Json = nlohmann::json;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
void success(nrd::Result result, const char *operation) {
    require(result == nrd::Result::SUCCESS, std::string(operation) + " result=" + std::to_string(uint32_t(result)));
}

struct Api {
    decltype(&nrd::CreateInstance) create = &nrd::CreateInstance;
    decltype(&nrd::DestroyInstance) destroy = &nrd::DestroyInstance;
    decltype(&nrd::GetLibraryDesc) library = &nrd::GetLibraryDesc;
    decltype(&nrd::GetInstanceDesc) description = &nrd::GetInstanceDesc;
    decltype(&nrd::SetCommonSettings) common = &nrd::SetCommonSettings;
    decltype(&nrd::SetDenoiserSettings) settings = &nrd::SetDenoiserSettings;
    decltype(&nrd::GetComputeDispatches) dispatches = &nrd::GetComputeDispatches;
#ifdef _WIN32
    HMODULE module{};
    explicit Api(const std::filesystem::path &path) {
        module = LoadLibraryW(path.c_str());
        require(module != nullptr, "cannot load explicit NRD oracle: " + path.string());
        auto bind = [&](auto &function, const char *name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
            require(function != nullptr, std::string("NRD oracle is missing ") + name);
        };
        bind(create, "CreateInstance"); bind(destroy, "DestroyInstance");
        bind(library, "GetLibraryDesc"); bind(description, "GetInstanceDesc");
        bind(common, "SetCommonSettings"); bind(settings, "SetDenoiserSettings");
        bind(dispatches, "GetComputeDispatches");
    }
    ~Api() { if (module) FreeLibrary(module); }
#else
    explicit Api(const std::filesystem::path &) {
        throw std::runtime_error("direct NRD DLL oracle requires Windows; use --fixture on this platform");
    }
#endif
    Api() = default;
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;
};

std::string bytes_hex(const uint8_t *bytes, uint32_t size) {
    static constexpr char hex[] = "0123456789abcdef";
    require(size == 0u || bytes != nullptr, "constant data pointer is null");
    std::string result(size_t(size) * 2u, '0');
    for (uint32_t i = 0u; i < size; ++i) {
        result[size_t(i) * 2u] = hex[bytes[i] >> 4u];
        result[size_t(i) * 2u + 1u] = hex[bytes[i] & 15u];
    }
    return result;
}

Json describe(const nrd::InstanceDesc &instance) {
    Json result = {{"constant_bytes", instance.constantBufferMaxDataSize},
        {"constant_space", instance.constantBufferSpaceIndex}, {"constant_register", instance.constantBufferRegisterIndex},
        {"sampler_space", instance.samplersSpaceIndex}, {"sampler_register", instance.samplersBaseRegisterIndex},
        {"resource_space", instance.resourcesSpaceIndex}, {"samplers", Json::array()},
        {"pipelines", Json::array()}, {"permanent", Json::array()}, {"transient", Json::array()}};
    for (uint32_t i = 0u; i < instance.samplersNum; ++i) result["samplers"].push_back(uint32_t(instance.samplers[i]));
    for (uint32_t i = 0u; i < instance.pipelinesNum; ++i) {
        const auto &pipeline = instance.pipelines[i];
        Json ranges = Json::array();
        for (uint32_t j = 0u; j < pipeline.resourceRangesNum; ++j) {
            const auto &range = pipeline.resourceRanges[j];
            ranges.push_back({uint32_t(range.descriptorType), range.baseRegisterIndex, range.descriptorsNum});
        }
        // Bytecode is intentionally absent from the repository EDSL scheduler.
        result["pipelines"].push_back({{"shader", pipeline.shaderFileName}, {"entry", pipeline.shaderEntryPointName},
            {"constants", pipeline.hasConstantData}, {"ranges", ranges}});
    }
    auto pool = [&](const char *name, const nrd::TextureDesc *textures, uint32_t count) {
        for (uint32_t i = 0u; i < count; ++i)
            result[name].push_back({uint32_t(textures[i].format), textures[i].downsampleFactor});
    };
    pool("permanent", instance.permanentPool, instance.permanentPoolSize);
    pool("transient", instance.transientPool, instance.transientPoolSize);
    const auto &limits = instance.descriptorPoolDesc;
    result["descriptor_limits"] = {limits.setsMaxNum, limits.constantBuffersMaxNum, limits.samplersMaxNum,
        limits.texturesMaxNum, limits.storageTexturesMaxNum};
    return result;
}

nrd::CommonSettings camera(uint16_t width, uint16_t height, uint32_t frame, uint32_t kind = 0u) {
    nrd::CommonSettings result{};
    auto identity = [](float *matrix) {
        std::fill_n(matrix, 16u, 0.f);
        for (uint32_t i = 0u; i < 4u; ++i) matrix[i * 5u] = 1.f;
    };
    identity(result.worldToViewMatrix); identity(result.worldToViewMatrixPrev);
    const float aspect = float(width) / float(height), depth = 1000.f / 999.9f;
    const std::array<float, 16u> projection{1.f / aspect, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, depth, 1.f, 0.f, 0.f, -.1f * depth, 0.f};
    std::copy(projection.begin(), projection.end(), result.viewToClipMatrix);
    std::copy(projection.begin(), projection.end(), result.viewToClipMatrixPrev);
    for (uint32_t i = 0u; i < 2u; ++i) {
        result.resourceSize[i] = result.resourceSizePrev[i] = result.rectSize[i] = result.rectSizePrev[i] = i ? height : width;
    }
    result.frameIndex = frame;
    result.timeDeltaBetweenFrames = 1000.f / 60.f; // Never use the oracle's wall clock.
    result.denoisingRange = 100000.f;
    result.disocclusionThreshold = .01f; result.disocclusionThresholdAlternate = .1f;
    result.motionVectorScale[0] = 1.f / float(width); result.motionVectorScale[1] = 1.f / float(height);
    result.motionVectorScale[2] = 1.f;
    result.isDisocclusionThresholdMixAvailable = true;
    result.accumulationMode = frame == 0u ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    if (kind == 1u) {
        const float angle = .37f, cosine = std::cos(angle), sine = std::sin(angle);
        result.worldToViewMatrix[0] = result.worldToViewMatrix[10] = cosine;
        result.worldToViewMatrix[2] = -sine; result.worldToViewMatrix[8] = sine;
        result.worldToViewMatrix[12] = .3f; result.worldToViewMatrix[13] = -.2f; result.worldToViewMatrix[14] = .4f;
        result.cameraJitter[0] = .25f; result.cameraJitter[1] = -.375f;
        result.cameraJitterPrev[0] = -.125f; result.cameraJitterPrev[1] = .125f;
    } else if (kind == 2u) {
        identity(result.viewToClipMatrix); identity(result.viewToClipMatrixPrev);
        result.viewToClipMatrix[0] = result.viewToClipMatrixPrev[0] = 1.f / aspect;
        result.viewToClipMatrix[10] = result.viewToClipMatrixPrev[10] = 1.f / 999.9f;
        result.viewToClipMatrix[14] = result.viewToClipMatrixPrev[14] = -.1f / 999.9f;
        result.isMotionVectorInWorldSpace = true;
        result.motionVectorScale[0] = result.motionVectorScale[1] = result.motionVectorScale[2] = 1.f;
    } else if (kind == 3u) {
        result.timeDeltaBetweenFrames = 1000.f / 24.f;
        result.rectSize[0] = std::max(uint16_t(1u), uint16_t(width / 2u));
        result.rectSize[1] = std::max(uint16_t(1u), uint16_t(height / 2u));
        result.rectOrigin[0] = 1u; result.rectOrigin[1] = 2u;
        result.viewZScale = 2.f;
        result.isDisocclusionThresholdMixAvailable = false;
    }
    return result;
}

nrd::ReblurSettings settings(uint32_t kind = 0u) {
    nrd::ReblurSettings result{};
    result.maxAccumulatedFrameNum = 50u; result.maxFastAccumulatedFrameNum = 6u;
    result.maxStabilizedFrameNum = 63u;
    result.diffusePrepassBlurRadius = 15.f; result.specularPrepassBlurRadius = 40.f;
    result.enableAntiFirefly = true;
    result.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_5X5;
    if (kind == 1u) result.enableAntiFirefly = false;
    if (kind == 2u) result.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
    if (kind == 3u) result.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
    if (kind == 4u) result.maxStabilizedFrameNum = 0u;
    if (kind == 5u) result.diffusePrepassBlurRadius = result.specularPrepassBlurRadius = 0.f;
    if (kind == 6u) {
        result.maxAccumulatedFrameNum = result.maxFastAccumulatedFrameNum = result.maxStabilizedFrameNum = 0u;
        result.historyFixFrameNum = 0u; result.historyFixBasePixelStride = 1u;
    }
    if (kind == 7u) {
        result.maxAccumulatedFrameNum = result.maxFastAccumulatedFrameNum = result.maxStabilizedFrameNum = 63u;
        result.historyFixFrameNum = 3u; result.historyFixBasePixelStride = 32u;
        result.minBlurRadius = .5f; result.maxBlurRadius = 64.f;
        result.lobeAngleFraction = .1f; result.roughnessFraction = .2f;
        result.antilagSettings.luminanceSigmaScale = 2.f; result.antilagSettings.luminanceSensitivity = 5.f;
    }
    return result;
}

Json exercise(Api &api) {
    const auto &library = api.library();
    require(library.versionMajor == 4u && library.versionMinor == 14u && library.versionBuild == 3u,
        "scheduler oracle requires exactly NRD 4.14.3");
    Json result = {{"schema", 1}, {"algorithm", "NRD 4.14.3 REBLUR_DIFFUSE_SPECULAR"},
        {"normal_encoding", uint32_t(library.normalEncoding)}, {"roughness_encoding", uint32_t(library.roughnessEncoding)},
        {"instances", Json::array()}};
    for (uint32_t layers : {1u, 3u}) {
        const nrd::DenoiserDesc denoisers[]{{0u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
            {1u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}, {2u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}};
        nrd::InstanceCreationDesc creation{}; creation.denoisers = denoisers; creation.denoisersNum = layers;
        nrd::Instance *instance{};
        success(api.create(creation, instance), "CreateInstance");
        const auto destroy = [&](nrd::Instance *value) { if (value) api.destroy(*value); };
        std::unique_ptr<nrd::Instance, decltype(destroy)> owner(instance, destroy);
        const auto &description = api.description(*instance);
        require(description.permanentPoolSize > 0u && description.transientPoolSize > 0u && description.pipelinesNum > 0u,
            "REBLUR pools and pipeline table must be present");
        Json sequence = {{"layers", layers}, {"description", describe(description)}, {"queries", Json::array()}};
        auto query = [&](const char *name, nrd::CommonSettings common, nrd::ReblurSettings method,
                         const std::vector<nrd::Identifier> &identifiers) {
            success(api.common(*instance, common), "SetCommonSettings");
            for (auto identifier : identifiers) success(api.settings(*instance, identifier, &method), "SetDenoiserSettings");
            const nrd::DispatchDesc *dispatches{}; uint32_t count{};
            success(api.dispatches(*instance, identifiers.data(), uint32_t(identifiers.size()), dispatches, count), "GetComputeDispatches");
            require(count > 0u && dispatches != nullptr, "a requested REBLUR layer must produce dispatches");
            Json passes = Json::array();
            bool temporal = false, stabilization = false;
            for (uint32_t i = 0u; i < count; ++i) {
                const auto &dispatch = dispatches[i];
                require(dispatch.pipelineIndex < description.pipelinesNum, "dispatch pipeline index exceeds the pipeline table");
                require(dispatch.gridWidth > 0u && dispatch.gridHeight > 0u, "dispatch grid must be nonempty");
                const std::string shader = description.pipelines[dispatch.pipelineIndex].shaderFileName;
                const bool clear = shader.find("Clear") != std::string::npos;
                require(clear ? dispatch.constantBufferDataSize == 0u : dispatch.constantBufferDataSize == 832u,
                    "REBLUR core constants must retain the 832-byte shared ABI");
                temporal |= shader.find("TemporalAccumulation") != std::string::npos;
                stabilization |= shader.find("_TemporalStabilization") != std::string::npos;
                Json resources = Json::array();
                for (uint32_t j = 0u; j < dispatch.resourcesNum; ++j) {
                    const auto &resource = dispatch.resources[j];
                    if (resource.type == nrd::ResourceType::PERMANENT_POOL)
                        require(resource.indexInPool < description.permanentPoolSize, "permanent resource index exceeds its pool");
                    if (resource.type == nrd::ResourceType::TRANSIENT_POOL)
                        require(resource.indexInPool < description.transientPoolSize, "transient resource index exceeds its pool");
                    resources.push_back({uint32_t(resource.descriptorType), uint32_t(resource.type), resource.indexInPool});
                }
                passes.push_back({{"name", dispatch.name}, {"identifier", dispatch.identifier}, {"pipeline", dispatch.pipelineIndex},
                    {"grid", {dispatch.gridWidth, dispatch.gridHeight}}, {"resources", resources},
                    {"constant_reuse", dispatch.constantBufferDataMatchesPreviousDispatch},
                    {"constants", bytes_hex(dispatch.constantBufferData, dispatch.constantBufferDataSize)}});
            }
            require(temporal && (stabilization == (method.maxStabilizedFrameNum != 0u)),
                std::string("scheduled temporal passes disagree: ") + name + " frame=" + std::to_string(common.frameIndex) +
                " temporal=" + std::to_string(temporal) + " stabilization=" + std::to_string(stabilization) +
                " requested=" + std::to_string(method.maxStabilizedFrameNum));
            sequence["queries"].push_back({{"case", name}, {"frame", common.frameIndex}, {"identifiers", identifiers},
                {"size", {common.resourceSize[0], common.resourceSize[1]}}, {"passes", passes}});
        };
        for (uint32_t frame = 0u; frame < 68u; ++frame)
            for (uint32_t layer = 0u; layer < layers; ++layer)
                query("stable_history", camera(127u, 73u, frame), settings(), {layer});
        for (uint32_t kind = 0u; kind < 8u; ++kind) {
            query("settings_reset", camera(31u, 19u, 0u), settings(kind), {0u});
            query("settings_continue", camera(31u, 19u, 1u), settings(kind), {0u});
        }
        for (uint32_t kind = 0u; kind < 4u; ++kind)
            query("camera_motion", camera(127u, 73u, kind + 2u, kind), settings(), {0u});
        uint32_t resize_frame = 12u;
        for (const auto size : {std::array<uint16_t, 2u>{1u, 1u}, {13u, 9u}, {17u, 9u}, {19u, 11u}, {1920u, 1080u}, {65535u, 65535u}})
            query("resize", camera(size[0], size[1], resize_frame++), settings(), {0u});
        auto restart = camera(13u, 9u, 3u); restart.accumulationMode = nrd::AccumulationMode::RESTART;
        query("explicit_restart", restart, settings(), {0u});
        restart.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
        query("explicit_clear_same_frame", restart, settings(), {0u});
        if (layers == 3u) {
            query("identifier_subset_reverse", camera(13u, 9u, 4u), settings(), {2u, 0u});
            query("all_identifiers_reverse", camera(13u, 9u, 5u), settings(), {2u, 1u, 0u});
        }
        result["instances"].push_back(std::move(sequence));
    }
    return result;
}

std::string fnv64(const Json &value) {
    // json uses sorted object keys. Snapshot values contain no JSON floats:
    // shader constants remain exact bytes encoded as lowercase hexadecimal.
    const std::string canonical = value.dump();
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : canonical) hash = (hash ^ byte) * 1099511628211ull;
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(16u, '0');
    for (uint32_t i = 0u; i < 16u; ++i) result[15u - i] = hex[(hash >> (i * 4u)) & 15u];
    return result;
}

Json digest(const Json &snapshot) {
    Json result = {{"algorithm", snapshot.at("algorithm")}, {"normal_encoding", snapshot.at("normal_encoding")},
        {"roughness_encoding", snapshot.at("roughness_encoding")}, {"instances", Json::array()}};
    for (const auto &instance : snapshot.at("instances")) {
        Json queries = Json::array();
        uint32_t index = 0u;
        for (const auto &query : instance.at("queries")) {
            std::string identifiers;
            for (const auto &identifier : query.at("identifiers")) {
                if (!identifiers.empty()) identifiers += ',';
                identifiers += std::to_string(identifier.get<uint32_t>());
            }
            const std::string label = "q" + std::to_string(index++) + ':' + query.at("case").get<std::string>() +
                ":f" + std::to_string(query.at("frame").get<uint32_t>()) + ":ids" + identifiers + ':' +
                std::to_string(query.at("size")[0u].get<uint32_t>()) + 'x' +
                std::to_string(query.at("size")[1u].get<uint32_t>());
            queries.push_back({{"label", label}, {"fnv64", fnv64(query)}});
        }
        result["instances"].push_back({{"layers", instance.at("layers")},
            {"description_fnv64", fnv64(instance.at("description"))}, {"queries", queries}});
    }
    return result;
}

void compare(const Json &actual, const Json &expected) {
    const auto difference = Json::diff(expected, actual);
    if (difference.empty()) return;
    const auto &first = difference.front();
    const std::string path = first.at("path").get<std::string>();
    std::string detail;
    if (path.ends_with("/constants") && first.contains("value")) {
        const auto &a = actual.at(Json::json_pointer(path)).get_ref<const std::string &>();
        const auto &b = expected.at(Json::json_pointer(path)).get_ref<const std::string &>();
        size_t byte = 0u;
        while (byte < a.size() && byte < b.size() && a[byte] == b[byte]) ++byte;
        detail = " first differing constant byte=" + std::to_string(byte / 2u);
        const size_t word = byte / 8u * 8u;
        if (word + 8u <= a.size() && word + 8u <= b.size()) {
            auto decode = [word](const std::string &value) {
                uint32_t bits = 0u;
                for (size_t i = 0u; i < 4u; ++i)
                    bits |= uint32_t(std::stoul(value.substr(word + i * 2u, 2u), nullptr, 16)) << (i * 8u);
                float number; std::memcpy(&number, &bits, sizeof(number));
                return std::to_string(number) + " (bits=" + std::to_string(bits) + ")";
            };
            detail += " owned=" + decode(a) + " oracle=" + decode(b);
        }
    } else if (path.ends_with("/fnv64")) {
        const auto parent = Json::json_pointer(path.substr(0u, path.rfind('/')));
        detail = " query=" + actual.at(parent).at("label").get<std::string>();
    }
    throw std::runtime_error("scheduler oracle mismatch at " + path + detail);
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::filesystem::path oracle, capture, fixture, digest_fixture;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            require(i + 1 < argc, "options require a value: --oracle-dll/--capture/--fixture/--digest-fixture");
            if (option == "--oracle-dll") oracle = argv[++i];
            else if (option == "--capture") capture = argv[++i];
            else if (option == "--fixture") fixture = argv[++i];
            else if (option == "--digest-fixture") digest_fixture = argv[++i];
            else throw std::runtime_error("unknown scheduler test option: " + option);
        }
        require(capture.empty() || !oracle.empty(), "--capture requires an explicit official --oracle-dll");
        Api implementation;
        const auto actual = exercise(implementation);
        if (!oracle.empty()) {
            Api reference(oracle);
            const auto expected = exercise(reference);
            if (!capture.empty()) {
                std::ofstream output(capture);
                require(bool(output), "cannot write scheduler oracle fixture");
                output << expected.dump();
                require(bool(output), "cannot complete scheduler oracle fixture");
            }
            compare(actual, expected);
        }
        if (!fixture.empty()) {
            std::ifstream input(fixture);
            require(bool(input), "cannot read scheduler oracle fixture");
            Json expected; input >> expected; compare(actual, expected);
        }
        if (!digest_fixture.empty()) {
            std::ifstream input(digest_fixture);
            require(bool(input), "cannot read scheduler golden digest fixture");
            Json expected; input >> expected;
            require(expected.at("schema").get<uint32_t>() == 1u && expected.at("origin").at("version") == "4.14.3",
                "scheduler golden digest fixture schema/version mismatch");
            compare(digest(actual), expected.at("digest"));
        }
        std::cout << "PASS: CPU REBLUR scheduler reset/history/layer/resize/settings/camera/resource contracts"
                  << (!oracle.empty() || !fixture.empty() ? "; official 4.14.3 pass/constants/pool comparison exact" : "")
                  << (!digest_fixture.empty() ? "; official 4.14.3 golden digests match" : "") << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
