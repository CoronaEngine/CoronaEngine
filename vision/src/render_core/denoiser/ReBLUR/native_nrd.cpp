#include "native_nrd.h"
#include "backends/vulkan_compute/vulkan_interop.h"
#include <vulkan/vulkan.hpp>
#include <nvrhi/vulkan.h>
#include <nvrhi/utils.h>
#include <NRD.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

// This translation unit must use the same Vulkan-Hpp headers/dispatcher mode
// as the statically linked NVRHI Vulkan backend (see prototype build recipe).
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace vision::reblur {
namespace {

static_assert(NRD_VERSION_MAJOR == 4 && NRD_VERSION_MINOR == 14 && NRD_VERSION_BUILD == 3);

struct alignas(16) NativeReblurConstants {
    uint64_t guide{}, raw_diffuse{}, raw_specular{}, output_diffuse{}, output_specular{};
    uint32_t width{}, height{};
    float hit_distance_parameters[4]{3.f, .1f, 20.f, -25.f};
    float previous_view_depth_row[4]{};
    float denoising_range{500000.f};
};
static_assert(sizeof(NativeReblurConstants) == 96u);
static_assert(offsetof(NativeReblurConstants, width) == 40u);
static_assert(offsetof(NativeReblurConstants, hit_distance_parameters) == 48u);
static_assert(offsetof(NativeReblurConstants, previous_view_depth_row) == 64u);
static_assert(offsetof(NativeReblurConstants, denoising_range) == 80u);

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void nrd_check(nrd::Result result, const char *operation) {
    if (result != nrd::Result::SUCCESS)
        throw std::runtime_error(std::string("Native REBLUR: NRD failure in ") + operation);
}
template<typename T>
uint64_t buffer_address(const ocarina::BufferDesc<T> &buffer) {
    return buffer.head() + buffer.offset_in_byte();
}

std::vector<uint32_t> read_spirv(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Native REBLUR: cannot open " + path.string());
    const auto size = input.tellg();
    require(size >= std::streamoff(20) && size % std::streamoff(4) == 0,
            "Native REBLUR: invalid SPIR-V file size");
    std::vector<uint32_t> words(static_cast<size_t>(size) / sizeof(uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(words.data()), size);
    require(input.good() && words[0] == 0x07230203u, "Native REBLUR: invalid SPIR-V shader");
    return words;
}

nvrhi::Format nvrhi_format(nrd::Format format) {
    switch (format) {
#define NRD_NATIVE_FORMAT(NRD, NVRHI) case nrd::Format::NRD: return nvrhi::Format::NVRHI
        NRD_NATIVE_FORMAT(R8_UNORM, R8_UNORM); NRD_NATIVE_FORMAT(R8_SNORM, R8_SNORM);
        NRD_NATIVE_FORMAT(R8_UINT, R8_UINT); NRD_NATIVE_FORMAT(R8_SINT, R8_SINT);
        NRD_NATIVE_FORMAT(RG8_UNORM, RG8_UNORM); NRD_NATIVE_FORMAT(RG8_SNORM, RG8_SNORM);
        NRD_NATIVE_FORMAT(RG8_UINT, RG8_UINT); NRD_NATIVE_FORMAT(RG8_SINT, RG8_SINT);
        NRD_NATIVE_FORMAT(RGBA8_UNORM, RGBA8_UNORM); NRD_NATIVE_FORMAT(RGBA8_SNORM, RGBA8_SNORM);
        NRD_NATIVE_FORMAT(RGBA8_UINT, RGBA8_UINT); NRD_NATIVE_FORMAT(RGBA8_SINT, RGBA8_SINT);
        NRD_NATIVE_FORMAT(RGBA8_SRGB, SRGBA8_UNORM);
        NRD_NATIVE_FORMAT(R16_UNORM, R16_UNORM); NRD_NATIVE_FORMAT(R16_SNORM, R16_SNORM);
        NRD_NATIVE_FORMAT(R16_UINT, R16_UINT); NRD_NATIVE_FORMAT(R16_SINT, R16_SINT);
        NRD_NATIVE_FORMAT(R16_SFLOAT, R16_FLOAT);
        NRD_NATIVE_FORMAT(RG16_UNORM, RG16_UNORM); NRD_NATIVE_FORMAT(RG16_SNORM, RG16_SNORM);
        NRD_NATIVE_FORMAT(RG16_UINT, RG16_UINT); NRD_NATIVE_FORMAT(RG16_SINT, RG16_SINT);
        NRD_NATIVE_FORMAT(RG16_SFLOAT, RG16_FLOAT);
        NRD_NATIVE_FORMAT(RGBA16_UNORM, RGBA16_UNORM); NRD_NATIVE_FORMAT(RGBA16_SNORM, RGBA16_SNORM);
        NRD_NATIVE_FORMAT(RGBA16_UINT, RGBA16_UINT); NRD_NATIVE_FORMAT(RGBA16_SINT, RGBA16_SINT);
        NRD_NATIVE_FORMAT(RGBA16_SFLOAT, RGBA16_FLOAT);
        NRD_NATIVE_FORMAT(R32_UINT, R32_UINT); NRD_NATIVE_FORMAT(R32_SINT, R32_SINT);
        NRD_NATIVE_FORMAT(R32_SFLOAT, R32_FLOAT);
        NRD_NATIVE_FORMAT(RG32_UINT, RG32_UINT); NRD_NATIVE_FORMAT(RG32_SINT, RG32_SINT);
        NRD_NATIVE_FORMAT(RG32_SFLOAT, RG32_FLOAT);
        NRD_NATIVE_FORMAT(RGB32_UINT, RGB32_UINT); NRD_NATIVE_FORMAT(RGB32_SINT, RGB32_SINT);
        NRD_NATIVE_FORMAT(RGB32_SFLOAT, RGB32_FLOAT);
        NRD_NATIVE_FORMAT(RGBA32_UINT, RGBA32_UINT); NRD_NATIVE_FORMAT(RGBA32_SINT, RGBA32_SINT);
        NRD_NATIVE_FORMAT(RGBA32_SFLOAT, RGBA32_FLOAT);
        NRD_NATIVE_FORMAT(R10_G10_B10_A2_UNORM, R10G10B10A2_UNORM);
        NRD_NATIVE_FORMAT(R11_G11_B10_UFLOAT, R11G11B10_FLOAT);
#undef NRD_NATIVE_FORMAT
        default: return nvrhi::Format::UNKNOWN;
    }
}

nvrhi::VulkanBindingOffsets binding_offsets() {
    const auto &offsets = nrd::GetLibraryDesc().spirvBindingOffsets;
    nvrhi::VulkanBindingOffsets result;
    result.sampler = offsets.samplerOffset;
    result.shaderResource = offsets.textureOffset;
    result.constantBuffer = offsets.constantBufferOffset;
    result.unorderedAccess = offsets.storageTextureAndBufferOffset;
    return result;
}

nrd::ReblurSettings nrd_settings(const Settings &settings) {
    nrd::ReblurSettings result{};
    result.maxAccumulatedFrameNum = static_cast<uint32_t>(std::clamp(settings.max_history, 0.f, 63.f));
    result.maxFastAccumulatedFrameNum = static_cast<uint32_t>(std::clamp(settings.fast_history, 0.f, 63.f));
    result.maxStabilizedFrameNum = settings.stabilization <= 0.f ? 0u :
        static_cast<uint32_t>(std::clamp(std::round(settings.stabilization / std::max(1.f - settings.stabilization, 1e-6f)), 0.f, 63.f));
    result.historyFixFrameNum = static_cast<uint32_t>(std::clamp(settings.history_fix_frames, 0.f, 3.f));
    result.historyFixBasePixelStride = static_cast<uint32_t>(std::max(settings.history_fix_stride, 1.f));
    result.diffusePrepassBlurRadius = settings.diffuse_prepass;
    result.specularPrepassBlurRadius = settings.specular_prepass;
    result.minBlurRadius = settings.min_radius; result.maxBlurRadius = settings.max_radius;
    result.lobeAngleFraction = settings.lobe_fraction; result.roughnessFraction = settings.roughness_fraction;
    result.planeDistanceSensitivity = settings.plane_sensitivity;
    result.minHitDistanceWeight = settings.min_hit_weight;
    result.antilagSettings.luminanceSigmaScale = settings.antilag_sigma;
    result.antilagSettings.luminanceSensitivity = settings.antilag_sensitivity;
    result.enableAntiFirefly = settings.anti_firefly != 0u;
    result.hitDistanceReconstructionMode = static_cast<nrd::HitDistanceReconstructionMode>(settings.reconstruction);
    result.enablePerformanceMode = false;
    result.checkerboardMode = nrd::CheckerboardMode::OFF;
    return result;
}

class NativeMessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char *text) override {
        std::fprintf(stderr, "[Native REBLUR/NVRHI %u] %s\n", static_cast<unsigned>(severity), text);
    }
};

struct Pipeline {
    nvrhi::ShaderHandle shader;
    nvrhi::BindingLayoutHandle layout;
    nvrhi::ComputePipelineHandle pipeline;
};

struct NativeLayer {
    nrd::Instance *instance{};
    uint32_t width{}, height{};
    std::array<uint32_t, 3> frames{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    nvrhi::BufferHandle constants;
    std::vector<uint8_t> zero_constants;
    std::vector<Pipeline> pipelines;
    std::vector<nvrhi::SamplerHandle> samplers;
    std::vector<nvrhi::TextureHandle> permanent, transient;
    nvrhi::TextureHandle motion, normal, view_z, disocclusion_mix, input_diffuse, input_specular, output_diffuse, output_specular;
    nvrhi::TextureHandle ta_probe, ts_probe, hf_probe;
    nvrhi::BindingSetHandle pack_binding, unpack_binding;
    std::map<std::vector<uint32_t>, nvrhi::BindingSetHandle> binding_cache;
    ~NativeLayer() { if (instance) nrd::DestroyInstance(*instance); }
};

}// namespace

struct NativeReblur::Impl {
    std::filesystem::path shader_directory;
    std::filesystem::path test_ta_shader, test_ts_shader, test_hf_shader;
    NativeMessageCallback message_callback;
    nvrhi::vulkan::DeviceHandle device;
    VkDevice borrowed_device{};
    Pipeline pack, unpack;
    // One instance owns three independent denoisers. NRD aliases their
    // transient pool, and conversion inputs/outputs are reused in queue order.
    std::unique_ptr<NativeLayer> resources;
    nvrhi::CommandListHandle command_list;
    struct SamplingProbe {
        std::filesystem::path shader_path;
        Pipeline pipeline;
        nvrhi::TextureHandle texture;
        nvrhi::SamplerHandle sampler;
        nvrhi::BindingSetHandle binding;
    };
    std::unique_ptr<SamplingProbe> sampling;
    struct ProfileSample {
        nvrhi::TimerQueryHandle timer;
        nvrhi::EventQueryHandle completion;
        uint64_t serial{};
        uint32_t width{}, height{}, frame{}, layer{}, pass_index{};
        std::string name;
        bool per_pass{};
    };
    std::deque<ProfileSample> pending_profiles;
    std::string profile_path, pass_profile_path;
    std::filesystem::path snapshot_directory;
    bool snapshot_frame_enabled{};
    uint32_t snapshot_frame{};
    struct SnapshotSample {
        nvrhi::StagingTextureHandle staging;
        std::string file, constants, pass, shader;
        uint32_t frame{}, layer{}, ordinal{}, slot{}, resource_type{}, pool_index{};
        uint32_t width{}, height{}, constant_bytes{};
        nvrhi::Format format{};
    };
    uint64_t profile_serial{};

    explicit Impl(std::filesystem::path directory)
        : shader_directory(std::move(directory)) {
        if (const char *value = std::getenv("REBLUR_SNAPSHOT_FRAME")) {
            const char *end = value + std::strlen(value);
            const auto parsed = std::from_chars(value, end, snapshot_frame);
            if (value == end || parsed.ec != std::errc{} || parsed.ptr != end)
                throw std::invalid_argument("REBLUR_SNAPSHOT_FRAME must be an unsigned decimal uint32");
            snapshot_frame_enabled = true;
        }
        if (const char *path = std::getenv("REBLUR_GPU_PROFILE"); path && *path) profile_path = path;
        if (const char *path = std::getenv("REBLUR_NATIVE_PROFILE_PASSES"); path && *path) pass_profile_path = path;
        if (const char *path = std::getenv("REBLUR_NATIVE_SNAPSHOT_DIR"); path && *path) snapshot_directory = path;
        if (const char *path = std::getenv("REBLUR_NATIVE_TEST_HF_PROBE_SHADER"); path && *path) {
            test_hf_shader = path;
            require(test_hf_shader.is_absolute() && std::filesystem::is_regular_file(test_hf_shader),
                    "Native REBLUR: TEST HF probe shader must be an existing absolute file");
        }
        if (const char *path = std::getenv("REBLUR_NATIVE_TEST_TA_SHADER"); path && *path) {
            test_ta_shader = path;
            require(test_ta_shader.is_absolute() && std::filesystem::is_regular_file(test_ta_shader),
                    "Native REBLUR: TEST TA shader must be an existing absolute file");
        }
        if (const char *path = std::getenv("REBLUR_NATIVE_TEST_TS_SHADER"); path && *path) {
            test_ts_shader = path;
            require(test_ts_shader.is_absolute() && std::filesystem::is_regular_file(test_ts_shader),
                    "Native REBLUR: TEST TS shader must be an existing absolute file");
        }
        require(profile_path.empty() || pass_profile_path.empty() ||
                    std::filesystem::absolute(profile_path).lexically_normal() != std::filesystem::absolute(pass_profile_path).lexically_normal(),
                "Native REBLUR: aggregate and per-pass profiling require separate CSV files");
        require(snapshot_directory.empty() || (profile_path.empty() && pass_profile_path.empty()),
                "Native REBLUR: snapshots must be disabled during GPU profiling");
        if (!snapshot_directory.empty()) std::filesystem::create_directories(snapshot_directory);
    }
    ~Impl() {
        try { collect_profile_results(); }
        catch (const std::exception &error) { std::fprintf(stderr, "[Native REBLUR] profile flush: %s\n", error.what()); }
    }

    void collect_profile_results() {
        if (!device) return;
        static std::mutex output_mutex;
        // A recycled timestamp slot may still report its old available value
        // before the queued GPU reset executes. Gate reads on this submission's
        // queue event, not timestamp availability from a previous generation.
        while (!pending_profiles.empty() && device->pollEventQuery(pending_profiles.front().completion) &&
               device->pollTimerQuery(pending_profiles.front().timer)) {
            const auto sample = std::move(pending_profiles.front()); pending_profiles.pop_front();
            const double milliseconds = double(device->getTimerQueryTime(sample.timer)) * 1000.;
            std::lock_guard lock(output_mutex);
            const auto &path = sample.per_pass ? pass_profile_path : profile_path;
            // app controls writes; ate also positions tellp at the existing EOF
            // on open, so reopening for each ready sample cannot repeat headers.
            std::ofstream output(std::filesystem::path(path), std::ios::app | std::ios::ate);
            require(bool(output), "Native REBLUR: cannot open GPU profile output");
            if (output.tellp() == std::streampos(0)) output << (sample.per_pass ?
                "batch,frame,layer,pass_index,name,width,height,ms\n" : "batch,name,width,height,ms\n");
            output << sample.serial << ',';
            if (sample.per_pass) output << sample.frame << ',' << sample.layer << ',' << sample.pass_index << ',';
            if (sample.per_pass) {
                output << '"';
                for (char c : sample.name) { if (c == '"') output << '"'; output << c; }
                output << '"';
            } else output << sample.name;
            output << ',' << sample.width << ',' << sample.height << ',' << std::setprecision(12) << milliseconds << '\n';
        }
    }

    // Diagnostic capture only. Each copy is recorded immediately after its
    // producing dispatch, before any aliased pool resource can be overwritten.
    // The normal path never allocates staging resources, copies, maps or waits.
    std::string snapshot_stem(uint32_t frame, uint32_t layer, uint32_t ordinal) const {
        return "f" + std::to_string(frame) + "_l" + std::to_string(layer) + "_p" + std::to_string(ordinal);
    }

    void snapshot_constants(uint32_t frame, uint32_t layer, uint32_t ordinal,
                            const void *data, uint32_t size) const {
        if (snapshot_frame_enabled && frame != snapshot_frame) return;
        std::ofstream output(snapshot_directory / (snapshot_stem(frame, layer, ordinal) + ".constants.bin"),
                             std::ios::binary | std::ios::trunc);
        require(bool(output), "Native REBLUR: cannot open snapshot constants");
        if (size) output.write(static_cast<const char *>(data), size);
        output.close();
        require(bool(output), "Native REBLUR: cannot write snapshot constants");
    }

    void snapshot_texture(std::vector<SnapshotSample> &samples, nvrhi::ITexture *texture,
                          uint32_t frame, uint32_t layer, uint32_t ordinal, uint32_t slot,
                          nrd::ResourceType resource_type, uint32_t pool_index,
                          const char *pass_name, const char *shader_name, uint32_t constant_bytes) {
        if (snapshot_frame_enabled && frame != snapshot_frame) return;
        const auto &description = texture->getDesc();
        auto staging = device->createStagingTexture(description, nvrhi::CpuAccessMode::Read);
        require(bool(staging), "Native REBLUR: snapshot staging allocation failed");
        command_list->copyTexture(staging, nvrhi::TextureSlice{}, texture, nvrhi::TextureSlice{});
        const auto stem = snapshot_stem(frame, layer, ordinal);
        samples.push_back({staging, stem + "_u" + std::to_string(slot) + ".bin", stem + ".constants.bin",
                           pass_name ? pass_name : "", shader_name ? shader_name : "",
                           frame, layer, ordinal, slot, static_cast<uint32_t>(resource_type), pool_index,
                           description.width, description.height, constant_bytes, description.format});
    }

    void write_snapshots(const std::vector<SnapshotSample> &samples) const {
        std::ofstream metadata(snapshot_directory / "metadata.jsonl", std::ios::app);
        require(bool(metadata), "Native REBLUR: cannot open snapshot metadata");
        for (const auto &sample : samples) {
            const auto &format = nvrhi::getFormatInfo(sample.format);
            const size_t row_bytes = size_t(sample.width) * format.bytesPerBlock;
            size_t row_pitch{};
            const void *mapped = device->mapStagingTexture(sample.staging, nvrhi::TextureSlice{},
                                                          nvrhi::CpuAccessMode::Read, &row_pitch);
            require(mapped != nullptr, "Native REBLUR: cannot map snapshot texture");
            if (row_pitch < row_bytes) {
                device->unmapStagingTexture(sample.staging);
                throw std::runtime_error("Native REBLUR: invalid snapshot row pitch");
            }
            std::ofstream output(snapshot_directory / sample.file, std::ios::binary | std::ios::trunc);
            for (uint32_t row = 0u; row < sample.height; ++row)
                output.write(static_cast<const char *>(mapped) + size_t(row) * row_pitch,
                             static_cast<std::streamsize>(row_bytes));
            device->unmapStagingTexture(sample.staging);
            output.close();
            require(bool(output), "Native REBLUR: cannot write snapshot texture");
            metadata << "{\"path\":" << std::quoted(sample.file)
                     << ",\"frame\":" << sample.frame << ",\"layer\":" << sample.layer
                     << ",\"pass\":" << sample.ordinal << ",\"name\":" << std::quoted(sample.pass)
                     << ",\"shader\":" << std::quoted(sample.shader) << ",\"slot\":" << sample.slot
                     << ",\"resourceType\":" << sample.resource_type << ",\"indexInPool\":" << sample.pool_index
                     << ",\"descriptorType\":" << static_cast<uint32_t>(nrd::DescriptorType::STORAGE_TEXTURE)
                     << ",\"format\":" << std::quoted(format.name) << ",\"formatValue\":" << static_cast<uint32_t>(sample.format)
                     << ",\"width\":" << sample.width << ",\"height\":" << sample.height
                     << ",\"rowBytes\":" << row_bytes << ",\"constants\":" << std::quoted(sample.constants)
                     << ",\"constantBytes\":" << sample.constant_bytes << "}\n";
        }
        metadata.close();
        require(bool(metadata), "Native REBLUR: cannot write snapshot metadata");
    }

    Pipeline create_pipeline(const void *bytecode, size_t size, const char *name,
                             const nvrhi::BindingLayoutDesc &layout_desc, const char *entry = "main") {
        Pipeline result;
        require(bytecode && size >= 20u && size % sizeof(uint32_t) == 0u, "Native REBLUR: invalid embedded shader");
        std::vector<uint32_t> aligned_bytecode;
        if (reinterpret_cast<uintptr_t>(bytecode) % alignof(uint32_t)) {
            aligned_bytecode.resize(size / sizeof(uint32_t));
            std::memcpy(aligned_bytecode.data(), bytecode, size);
            bytecode = aligned_bytecode.data();
        }
        nvrhi::ShaderDesc shader_desc;
        shader_desc.shaderType = nvrhi::ShaderType::Compute; shader_desc.debugName = name; shader_desc.entryName = entry;
        result.shader = device->createShader(shader_desc, bytecode, size);
        require(bool(result.shader), "Native REBLUR: shader creation failed");
        result.layout = device->createBindingLayout(layout_desc);
        require(bool(result.layout), "Native REBLUR: binding layout creation failed");
        nvrhi::ComputePipelineDesc pipeline_desc;
        pipeline_desc.CS = result.shader; pipeline_desc.bindingLayouts = {result.layout};
        result.pipeline = device->createComputePipeline(pipeline_desc);
        require(bool(result.pipeline), "Native REBLUR: compute pipeline creation failed");
        return result;
    }

    void initialize(const ocarina::VulkanInteropContext &context) {
        if (device) {
            require(context.device == borrowed_device, "Native REBLUR: Vulkan device changed");
            return;
        }
        const auto &library = nrd::GetLibraryDesc();
        require(library.versionMajor == 4u && library.versionMinor == 14u && library.versionBuild == 3u &&
                    library.normalEncoding == nrd::NormalEncoding::R10_G10_B10_A2_UNORM &&
                    library.roughnessEncoding == nrd::RoughnessEncoding::LINEAR,
                "Native REBLUR: incompatible NRD runtime/packing configuration");
        const auto offsets = binding_offsets();
        require(offsets.sampler == 100u && offsets.shaderResource == 200u &&
                    offsets.constantBuffer == 300u && offsets.unorderedAccess == 400u,
                "Native REBLUR: conversion SPIR-V binding offsets do not match NRD");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(context.instance, context.get_instance_proc_addr, context.device);
        nvrhi::vulkan::DeviceDesc description{};
        description.errorCB = &message_callback;
        description.instance = context.instance; description.physicalDevice = context.physical_device;
        description.device = context.device;
        description.computeQueue = context.compute_queue; description.computeQueueIndex = int(context.queue_family);
        description.bufferDeviceAddressSupported = true;
        description.maxTimerQueries = 4096u;
        device = nvrhi::vulkan::createDevice(description);
        require(bool(device), "Native REBLUR: cannot wrap Vulkan device");
        borrowed_device = context.device;
        command_list = device->createCommandList(nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Compute));
        require(bool(command_list), "Native REBLUR: command list creation failed");
        nvrhi::BindingLayoutDesc pack_layout;
        pack_layout.visibility = nvrhi::ShaderType::Compute; pack_layout.bindingOffsets = offsets;
        pack_layout.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0u, sizeof(NativeReblurConstants)));
        for (uint32_t slot = 0u; slot < 6u; ++slot)
            pack_layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(slot));
        const auto pack_spirv = read_spirv(shader_directory / "NativeReblurPack.spv");
        pack = create_pipeline(pack_spirv.data(), pack_spirv.size() * sizeof(uint32_t), "Native REBLUR pack", pack_layout);
        nvrhi::BindingLayoutDesc unpack_layout;
        unpack_layout.visibility = nvrhi::ShaderType::Compute; unpack_layout.bindingOffsets = offsets;
        unpack_layout.bindings.push_back(nvrhi::BindingLayoutItem::PushConstants(0u, sizeof(NativeReblurConstants)));
        unpack_layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(0u));
        unpack_layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(1u));
        const auto unpack_spirv = read_spirv(shader_directory / "NativeReblurUnpack.spv");
        unpack = create_pipeline(unpack_spirv.data(), unpack_spirv.size() * sizeof(uint32_t), "Native REBLUR unpack", unpack_layout);
        std::fprintf(stderr, "[Native REBLUR] original NRD 4.14.3 shader backend enabled\n");
    }

    void external_buffer_barrier() {
        // NVRHI tracks descriptor resources, but raw device-address loads and
        // stores are invisible to its state tracker. Order those accesses at
        // conversion/NRD boundaries as well as the engine's interop boundary.
        command_list->commitBarriers();
        const VkCommandBuffer native = command_list->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
    }

    nvrhi::TextureHandle create_texture(uint32_t width, uint32_t height, nvrhi::Format format, const std::string &name) {
        require(format != nvrhi::Format::UNKNOWN, "Native REBLUR: unsupported NRD texture format");
        nvrhi::TextureDesc description;
        description.width = width; description.height = height; description.format = format;
        description.dimension = nvrhi::TextureDimension::Texture2D; description.mipLevels = 1u;
        description.initialState = nvrhi::ResourceStates::ShaderResource;
        description.keepInitialState = true; description.isUAV = true; description.debugName = name;
        auto result = device->createTexture(description);
        require(bool(result), "Native REBLUR: texture allocation failed");
        return result;
    }

    void run_sampling_probe(const ocarina::VulkanInteropContext &context,
                            const ocarina::BufferDesc<float4> &output,
                            const std::filesystem::path &probe_spirv) {
        initialize(context);
        device->runGarbageCollection();
        const uint64_t output_address = buffer_address(output);
        require(output_address != 0u && (output_address & 15u) == 0u &&
                    output.size_in_byte() >= 256u * sizeof(float4),
                "Native REBLUR: sampling probe requires 256 aligned float4 outputs");
        if (!sampling || sampling->shader_path != probe_spirv) {
            auto probe = std::make_unique<SamplingProbe>();
            probe->shader_path = probe_spirv;
            nvrhi::BindingLayoutDesc layout;
            layout.visibility = nvrhi::ShaderType::Compute;
            layout.bindingOffsets = binding_offsets();
            layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0u, sizeof(output_address)),
                               nvrhi::BindingLayoutItem::Texture_SRV(0u),
                               nvrhi::BindingLayoutItem::Sampler(0u)};
            const auto bytecode = read_spirv(probe_spirv);
            probe->pipeline = create_pipeline(bytecode.data(), bytecode.size() * sizeof(uint32_t),
                                               "Native REBLUR sampling contract", layout);
            probe->texture = create_texture(4u, 4u, nvrhi::Format::RGBA16_FLOAT,
                                            "Native REBLUR sampling contract pixels");
            probe->sampler = device->createSampler(nvrhi::SamplerDesc()
                .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(true));
            require(bool(probe->sampler), "Native REBLUR: sampling probe sampler creation failed");
            nvrhi::BindingSetDesc bindings;
            bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0u, sizeof(output_address)),
                                 nvrhi::BindingSetItem::Texture_SRV(0u, probe->texture),
                                 nvrhi::BindingSetItem::Sampler(0u, probe->sampler)};
            probe->binding = device->createBindingSet(bindings, probe->pipeline.layout);
            require(bool(probe->binding), "Native REBLUR: sampling probe binding creation failed");
            sampling = std::move(probe);
        }
        // All coordinates/products are small exactly representable integers.
        // Upload raw binary16 texels so no GPU store quantization is involved.
        constexpr std::array<uint16_t, 10> integers{
            0x0000u, 0x3c00u, 0x4000u, 0x4200u, 0x4400u,
            0x4500u, 0x4600u, 0x4700u, 0x4800u, 0x4880u};
        std::array<uint16_t, 4u * 4u * 4u> pixels{};
        for (uint32_t y = 0u; y != 4u; ++y) for (uint32_t x = 0u; x != 4u; ++x) {
            const auto offset = (y * 4u + x) * 4u;
            pixels[offset] = integers[x]; pixels[offset + 1u] = integers[y];
            pixels[offset + 2u] = integers[x * y]; pixels[offset + 3u] = integers[1u];
        }
        command_list->open();
        command_list->writeTexture(sampling->texture, 0u, 0u, pixels.data(), 4u * 4u * sizeof(uint16_t));
        nvrhi::ComputeState state;
        state.pipeline = sampling->pipeline.pipeline; state.bindings = {sampling->binding};
        command_list->setComputeState(state);
        command_list->setPushConstants(&output_address, sizeof(output_address));
        command_list->dispatch(16u, 1u);
        external_buffer_barrier();
        command_list->close();
        device->executeCommandList(command_list, nvrhi::CommandQueue::Compute);
    }

    void run_format_store_probe(const ocarina::VulkanInteropContext &context,
                                const std::vector<float4> &samples,
                                const std::filesystem::path &probe_spirv,
                                const std::filesystem::path &output_prefix) {
        initialize(context);
        device->runGarbageCollection();
        static_assert(sizeof(float4) == 4u * sizeof(float));
        VkPhysicalDeviceProperties properties{};
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties(context.physical_device, &properties);
        require(!samples.empty() && samples.size() <= properties.limits.maxImageDimension2D,
                "Native REBLUR: format store probe needs 1..maxImageDimension2D samples");
        const auto count = static_cast<uint32_t>(samples.size());
        const auto input_bytes = samples.size() * sizeof(float4);
        std::vector<uint32_t> encoded_samples(count);
        for (uint32_t i = 0u; i != count; ++i) {
            uint32_t bits = (i * 65521u) & 65535u;
            if ((bits & 0x7c00u) == 0x7c00u && (bits & 1023u) != 0u)
                bits = (bits & 0x8000u) | 0x7c00u;
            encoded_samples[i] = bits;
        }
        const auto encoded_bytes = encoded_samples.size() * sizeof(uint32_t);
        nvrhi::BindingLayoutDesc layout;
        layout.visibility = nvrhi::ShaderType::Compute;
        layout.bindingOffsets = binding_offsets();
        layout.bindings = {nvrhi::BindingLayoutItem::PushConstants(0u, sizeof(count)),
                           nvrhi::BindingLayoutItem::RawBuffer_SRV(0u),
                           nvrhi::BindingLayoutItem::RawBuffer_SRV(1u)};
        for (uint32_t slot = 0u; slot != 6u; ++slot)
            layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(slot));
        const auto bytecode = read_spirv(probe_spirv);
        auto pipeline = create_pipeline(bytecode.data(), bytecode.size() * sizeof(uint32_t),
                                         "Native REBLUR format store contract", layout);
        nvrhi::BufferDesc input_description;
        input_description.byteSize = input_bytes;
        input_description.canHaveRawViews = true;
        input_description.initialState = nvrhi::ResourceStates::ShaderResource;
        input_description.keepInitialState = true;
        input_description.debugName = "Native REBLUR format store source float32 bits";
        auto input = device->createBuffer(input_description);
        require(bool(input), "Native REBLUR: format store input allocation failed");
        auto encoded_description = input_description;
        encoded_description.byteSize = encoded_bytes;
        encoded_description.debugName = "Native REBLUR independent half decode source bits";
        auto encoded_input = device->createBuffer(encoded_description);
        require(bool(encoded_input), "Native REBLUR: half decode input allocation failed");
        struct Output {
            nvrhi::Format format;
            const char *suffix;
            nvrhi::TextureHandle texture;
            nvrhi::StagingTextureHandle staging;
        };
        std::array<Output, 6> outputs{{
            {nvrhi::Format::R10G10B10A2_UNORM, "rgb10a2", {}, {}},
            {nvrhi::Format::RGBA16_FLOAT, "rgba16f", {}, {}},
            {nvrhi::Format::R16_FLOAT, "r16f", {}, {}},
            {nvrhi::Format::R8_UNORM, "r8", {}, {}},
            {nvrhi::Format::R32_UINT, "f32tof16", {}, {}},
            {nvrhi::Format::R32_FLOAT, "f16tof32", {}, {}}}};
        nvrhi::BindingSetDesc bindings;
        bindings.bindings = {nvrhi::BindingSetItem::PushConstants(0u, sizeof(count)),
                             nvrhi::BindingSetItem::RawBuffer_SRV(0u, input),
                             nvrhi::BindingSetItem::RawBuffer_SRV(1u, encoded_input)};
        for (uint32_t slot = 0u; slot != outputs.size(); ++slot) {
            auto &output = outputs[slot];
            output.texture = create_texture(count, 1u, output.format,
                                             std::string("Native REBLUR format store ") + output.suffix);
            output.staging = device->createStagingTexture(output.texture->getDesc(), nvrhi::CpuAccessMode::Read);
            require(bool(output.staging), "Native REBLUR: format store staging allocation failed");
            bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(slot, output.texture));
        }
        auto binding = device->createBindingSet(bindings, pipeline.layout);
        require(bool(binding), "Native REBLUR: format store binding creation failed");
        auto completion = device->createEventQuery();
        require(bool(completion), "Native REBLUR: format store completion event allocation failed");
        command_list->open();
        command_list->writeBuffer(input, samples.data(), input_bytes);
        command_list->writeBuffer(encoded_input, encoded_samples.data(), encoded_bytes);
        nvrhi::ComputeState state;
        state.pipeline = pipeline.pipeline; state.bindings = {binding};
        command_list->setComputeState(state);
        command_list->setPushConstants(&count, sizeof(count));
        command_list->dispatch((count + 63u) / 64u, 1u);
        for (const auto &output : outputs)
            command_list->copyTexture(output.staging, nvrhi::TextureSlice{}, output.texture, nvrhi::TextureSlice{});
        command_list->close();
        device->executeCommandList(command_list, nvrhi::CommandQueue::Compute);
        device->setEventQuery(completion, nvrhi::CommandQueue::Compute);
        device->waitEventQuery(completion);

        if (output_prefix.has_parent_path()) std::filesystem::create_directories(output_prefix.parent_path());
        auto output_path = [&](const char *suffix) {
            auto path = output_prefix; path += suffix; return path;
        };
        std::ofstream source(output_path("-inputs.bin"), std::ios::binary | std::ios::trunc);
        source.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(input_bytes));
        source.close();
        require(bool(source), "Native REBLUR: cannot write format store probe inputs");
        std::ofstream encoded_source(output_path("-decode-inputs.bin"), std::ios::binary | std::ios::trunc);
        encoded_source.write(reinterpret_cast<const char *>(encoded_samples.data()), static_cast<std::streamsize>(encoded_bytes));
        encoded_source.close();
        require(bool(encoded_source), "Native REBLUR: cannot write half decode probe inputs");
        std::ofstream metadata(output_path("-metadata.jsonl"), std::ios::trunc);
        require(bool(metadata), "Native REBLUR: cannot open format store metadata");
        for (const auto &output : outputs) {
            const auto &format = nvrhi::getFormatInfo(output.format);
            const size_t row_bytes = size_t(count) * format.bytesPerBlock;
            size_t row_pitch{};
            const void *mapped = device->mapStagingTexture(output.staging, nvrhi::TextureSlice{},
                                                          nvrhi::CpuAccessMode::Read, &row_pitch);
            require(mapped != nullptr, "Native REBLUR: cannot map format store texture");
            if (row_pitch < row_bytes) {
                device->unmapStagingTexture(output.staging);
                throw std::runtime_error("Native REBLUR: invalid format store row pitch");
            }
            const auto path = output_path((std::string("-") + output.suffix + ".bin").c_str());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(static_cast<const char *>(mapped), static_cast<std::streamsize>(row_bytes));
            device->unmapStagingTexture(output.staging);
            file.close();
            require(bool(file), "Native REBLUR: cannot write format store texture");
            metadata << "{\"path\":" << std::quoted(path.filename().string())
                     << ",\"format\":" << std::quoted(format.name) << ",\"width\":" << count
                     << ",\"height\":1,\"rowBytes\":" << row_bytes
                     << ",\"inputs\":" << std::quoted(output_path("-inputs.bin").filename().string()) << "}\n";
        }
        metadata.close();
        require(bool(metadata), "Native REBLUR: cannot write format store metadata");
    }

    NativeLayer &prepare_layer(uint32_t index, uint32_t width, uint32_t height) {
        require(index < 3u, "Native REBLUR: invalid layer");
        if (resources && resources->width == width && resources->height == height) return *resources;
        auto layer = std::make_unique<NativeLayer>();
        layer->width = width; layer->height = height;
        const nrd::DenoiserDesc denoisers[]{
            {0u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}, {1u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
            {2u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}};
        nrd::InstanceCreationDesc creation{}; creation.denoisers = denoisers; creation.denoisersNum = 3u;
        nrd_check(nrd::CreateInstance(creation, layer->instance), "CreateInstance");
        const auto &description = nrd::GetInstanceDesc(*layer->instance);
        require(description.constantBufferSpaceIndex == 0u && description.samplersSpaceIndex == 0u && description.resourcesSpaceIndex == 0u,
                "Native REBLUR: only descriptor space zero is supported");
        layer->constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
            description.constantBufferMaxDataSize, "Native REBLUR constants", description.descriptorPoolDesc.setsMaxNum * 4u));
        require(bool(layer->constants), "Native REBLUR: constant buffer allocation failed");
        layer->zero_constants.resize(description.constantBufferMaxDataSize, 0u);
        if (!test_hf_shader.empty())
            layer->hf_probe = create_texture(80u, 1u, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR TEST HF spatial probe");
        if (!test_ta_shader.empty())
            layer->ta_probe = create_texture(63u, 1u, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR TEST TA probe");
        if (!test_ts_shader.empty())
            layer->ts_probe = create_texture(4u, 1u, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR TEST TS probe");
        for (uint32_t i = 0u; i < description.samplersNum; ++i) {
            const bool linear = description.samplers[i] == nrd::Sampler::LINEAR_CLAMP;
            require(linear || description.samplers[i] == nrd::Sampler::NEAREST_CLAMP, "Native REBLUR: unsupported sampler");
            auto sampler = device->createSampler(nvrhi::SamplerDesc().setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setAllFilters(linear));
            require(bool(sampler), "Native REBLUR: sampler allocation failed");
            layer->samplers.push_back(sampler);
        }
        for (uint32_t i = 0u; i < description.pipelinesNum; ++i) {
            const auto &source = description.pipelines[i];
            const bool hf_probe_pipeline = !test_hf_shader.empty() &&
                std::strcmp(source.shaderFileName, "REBLUR_DiffuseSpecular_HistoryFix.cs") == 0;
            const bool ta_probe_pipeline = !test_ta_shader.empty() &&
                std::strcmp(source.shaderFileName, "REBLUR_DiffuseSpecular_TemporalAccumulation.cs") == 0;
            const bool ts_probe_pipeline = !test_ts_shader.empty() &&
                std::strcmp(source.shaderFileName, "REBLUR_DiffuseSpecular_TemporalStabilization.cs") == 0;
            require(source.computeShaderSPIRV.bytecode && source.computeShaderSPIRV.size,
                    "Native REBLUR: NRD library has no matching SPIR-V shaders");
            nvrhi::BindingLayoutDesc layout;
            layout.visibility = nvrhi::ShaderType::Compute; layout.bindingOffsets = binding_offsets();
            layout.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(description.constantBufferRegisterIndex));
            for (uint32_t sampler = 0u; sampler < description.samplersNum; ++sampler)
                layout.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(description.samplersBaseRegisterIndex + sampler));
            for (uint32_t range_index = 0u; range_index < source.resourceRangesNum; ++range_index) {
                const auto &range = source.resourceRanges[range_index];
                for (uint32_t slot = 0u; slot < range.descriptorsNum; ++slot)
                    layout.bindings.push_back(range.descriptorType == nrd::DescriptorType::TEXTURE ?
                        nvrhi::BindingLayoutItem::Texture_SRV(range.baseRegisterIndex + slot) :
                        nvrhi::BindingLayoutItem::Texture_UAV(range.baseRegisterIndex + slot));
            }
            if (hf_probe_pipeline) layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(4u));
            if (ta_probe_pipeline) layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(7u));
            if (ts_probe_pipeline) layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(6u));
            const char *test_shader_directory = std::getenv("REBLUR_NATIVE_TEST_SHADER_DIR");
            if (hf_probe_pipeline) {
                const auto test_spirv = read_spirv(test_hf_shader);
                std::fprintf(stdout, "Native REBLUR TEST independent HF spatial probe shader: %s\n", test_hf_shader.string().c_str());
                layer->pipelines.push_back(create_pipeline(test_spirv.data(), test_spirv.size() * sizeof(uint32_t),
                    source.shaderFileName, layout, source.shaderEntryPointName));
            } else if (ta_probe_pipeline) {
                const auto test_spirv = read_spirv(test_ta_shader);
                std::fprintf(stdout, "Native REBLUR TEST temporal accumulation shader: %s\n", test_ta_shader.string().c_str());
                layer->pipelines.push_back(create_pipeline(test_spirv.data(), test_spirv.size() * sizeof(uint32_t),
                    source.shaderFileName, layout, source.shaderEntryPointName));
            } else if (ts_probe_pipeline) {
                const auto test_spirv = read_spirv(test_ts_shader);
                std::fprintf(stdout, "Native REBLUR TEST independent temporal stabilization probe shader: %s\n", test_ts_shader.string().c_str());
                layer->pipelines.push_back(create_pipeline(test_spirv.data(), test_spirv.size() * sizeof(uint32_t),
                    source.shaderFileName, layout, source.shaderEntryPointName));
            } else if (test_shader_directory && *test_shader_directory &&
                std::strcmp(source.shaderFileName, "REBLUR_DiffuseSpecular_TemporalStabilization.cs") == 0) {
                const auto test_path = std::filesystem::path(test_shader_directory) /
                    "REBLUR_DiffuseSpecular_TemporalStabilization.cs.spv";
                const auto test_spirv = read_spirv(test_path);
                std::fprintf(stdout, "Native REBLUR TEST temporal stabilization shader: %s\n", test_path.string().c_str());
                layer->pipelines.push_back(create_pipeline(test_spirv.data(), test_spirv.size() * sizeof(uint32_t),
                    source.shaderFileName, layout, source.shaderEntryPointName));
            } else {
                layer->pipelines.push_back(create_pipeline(source.computeShaderSPIRV.bytecode, size_t(source.computeShaderSPIRV.size),
                    source.shaderFileName, layout, source.shaderEntryPointName));
            }
        }
        auto allocate_pool = [&](const nrd::TextureDesc *source, uint32_t count, std::vector<nvrhi::TextureHandle> &pool, const char *name) {
            for (uint32_t i = 0u; i < count; ++i) {
                const uint32_t downsample = std::max(uint32_t(source[i].downsampleFactor), 1u);
                pool.push_back(create_texture(std::max((width + downsample - 1u) / downsample, 1u),
                    std::max((height + downsample - 1u) / downsample, 1u), nvrhi_format(source[i].format),
                    std::string("Native REBLUR ") + name + ' ' + std::to_string(i)));
            }
        };
        allocate_pool(description.permanentPool, description.permanentPoolSize, layer->permanent, "permanent");
        allocate_pool(description.transientPool, description.transientPoolSize, layer->transient, "transient");
        layer->motion = create_texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR motion");
        layer->normal = create_texture(width, height, nvrhi::Format::R10G10B10A2_UNORM, "Native REBLUR normal roughness");
        layer->view_z = create_texture(width, height, nvrhi::Format::R32_FLOAT, "Native REBLUR viewZ");
        layer->disocclusion_mix = create_texture(width, height, nvrhi::Format::R8_UNORM, "Native REBLUR disocclusion threshold mix");
        layer->input_diffuse = create_texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR diffuse input");
        layer->input_specular = create_texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR specular input");
        layer->output_diffuse = create_texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR diffuse output");
        layer->output_specular = create_texture(width, height, nvrhi::Format::RGBA16_FLOAT, "Native REBLUR specular output");
        nvrhi::BindingSetDesc pack_set;
        pack_set.bindings = {nvrhi::BindingSetItem::PushConstants(0u, sizeof(NativeReblurConstants)),
            nvrhi::BindingSetItem::Texture_UAV(0u, layer->input_diffuse), nvrhi::BindingSetItem::Texture_UAV(1u, layer->input_specular),
            nvrhi::BindingSetItem::Texture_UAV(2u, layer->normal), nvrhi::BindingSetItem::Texture_UAV(3u, layer->view_z),
            nvrhi::BindingSetItem::Texture_UAV(4u, layer->motion), nvrhi::BindingSetItem::Texture_UAV(5u, layer->disocclusion_mix)};
        layer->pack_binding = device->createBindingSet(pack_set, pack.layout);
        nvrhi::BindingSetDesc unpack_set;
        unpack_set.bindings = {nvrhi::BindingSetItem::PushConstants(0u, sizeof(NativeReblurConstants)),
            nvrhi::BindingSetItem::Texture_SRV(0u, layer->output_diffuse), nvrhi::BindingSetItem::Texture_SRV(1u, layer->output_specular)};
        layer->unpack_binding = device->createBindingSet(unpack_set, unpack.layout);
        require(bool(layer->pack_binding) && bool(layer->unpack_binding), "Native REBLUR: conversion binding set failed");
        resources = std::move(layer);
        return *resources;
    }

    nvrhi::ITexture *resolve_texture(NativeLayer &layer, const nrd::ResourceDesc &resource) {
        switch (resource.type) {
            case nrd::ResourceType::IN_MV: return layer.motion;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return layer.normal;
            case nrd::ResourceType::IN_VIEWZ: return layer.view_z;
            case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX: return layer.disocclusion_mix;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return layer.input_diffuse;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return layer.input_specular;
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return layer.output_diffuse;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return layer.output_specular;
            case nrd::ResourceType::PERMANENT_POOL: return layer.permanent.at(resource.indexInPool);
            case nrd::ResourceType::TRANSIENT_POOL: return layer.transient.at(resource.indexInPool);
            default: throw std::runtime_error("Native REBLUR: unexpected optional resource in dispatch");
        }
    }

    void run(const ocarina::VulkanInteropContext &context, const FilterParam &parameters,
             const NativeCameraParams &camera, uint32_t layer_index, const Settings &settings, bool reset) {
        initialize(context);
        collect_profile_results();
        device->runGarbageCollection();
        const auto width = parameters.size.x, height = parameters.size.y;
        require(width && height && width <= UINT16_MAX && height <= UINT16_MAX, "Native REBLUR: invalid resolution");
        require(std::isfinite(camera.denoising_range) && camera.denoising_range > 0.f && camera.denoising_range < 1e30f,
                "Native REBLUR: denoising range must be positive and finite");
        auto &layer = prepare_layer(layer_index, width, height);
        NativeReblurConstants conversion;
        conversion.guide = buffer_address(parameters.guide);
        conversion.raw_diffuse = buffer_address(parameters.raw_diffuse); conversion.raw_specular = buffer_address(parameters.raw_specular);
        conversion.output_diffuse = buffer_address(parameters.output_diffuse); conversion.output_specular = buffer_address(parameters.output_specular);
        conversion.width = width; conversion.height = height;
        conversion.denoising_range = camera.denoising_range;
        for (uint32_t component = 0u; component < 4u; ++component)
            conversion.previous_view_depth_row[component] = camera.world_to_view_prev[component * 4u + 2u];
        const bool continuous = parameters.history_valid && layer.frames[layer_index] != UINT32_MAX && layer.frames[layer_index] + 1u == parameters.frame;
        require(conversion.guide && conversion.raw_diffuse && conversion.raw_specular && conversion.output_diffuse && conversion.output_specular,
                "Native REBLUR: missing external buffer address");
        require(parameters.guide.size_in_byte() >= uint64_t(width) * height * sizeof(Guide), "Native REBLUR: guide buffer is too small");
        const uint64_t signal_bytes = uint64_t(width) * height * sizeof(float4);
        require(parameters.raw_diffuse.size_in_byte() >= signal_bytes && parameters.raw_specular.size_in_byte() >= signal_bytes &&
                    parameters.output_diffuse.size_in_byte() >= signal_bytes && parameters.output_specular.size_in_byte() >= signal_bytes,
                "Native REBLUR: signal buffer is too small");
        require(((conversion.guide | conversion.raw_diffuse | conversion.raw_specular |
                    conversion.output_diffuse | conversion.output_specular) & 15u) == 0u,
                "Native REBLUR: conversion buffers require 16-byte aligned addresses");
        auto method = nrd_settings(settings);
        nrd_check(nrd::SetDenoiserSettings(*layer.instance, layer_index, &method), "SetDenoiserSettings");
        nrd::CommonSettings common{};
        std::memcpy(common.worldToViewMatrix, camera.world_to_view.data(), sizeof(common.worldToViewMatrix));
        std::memcpy(common.worldToViewMatrixPrev, camera.world_to_view_prev.data(), sizeof(common.worldToViewMatrixPrev));
        std::memcpy(common.viewToClipMatrix, camera.view_to_clip.data(), sizeof(common.viewToClipMatrix));
        std::memcpy(common.viewToClipMatrixPrev, camera.view_to_clip_prev.data(), sizeof(common.viewToClipMatrixPrev));
        std::memcpy(common.cameraJitter, camera.jitter.data(), sizeof(common.cameraJitter));
        std::memcpy(common.cameraJitterPrev, camera.jitter_prev.data(), sizeof(common.cameraJitterPrev));
        common.motionVectorScale[0] = 1.f / float(width); common.motionVectorScale[1] = 1.f / float(height); common.motionVectorScale[2] = 1.f;
        common.isMotionVectorInWorldSpace = false;
        common.isDisocclusionThresholdMixAvailable = true;
        common.timeDeltaBetweenFrames = camera.frame_delta_ms;
        common.denoisingRange = camera.denoising_range;
        common.disocclusionThreshold = camera.disocclusion_threshold;
        common.disocclusionThresholdAlternate = camera.disocclusion_threshold_alternate;
        common.frameIndex = parameters.frame;
        common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = uint16_t(width);
        common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = uint16_t(height);
        // NRD filters injected clears by the requested identifier, so a reset
        // clears this denoiser's history while preserving the other layers.
        common.accumulationMode = (reset || !continuous) ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
        nrd_check(nrd::SetCommonSettings(*layer.instance, common), "SetCommonSettings");
        const nrd::Identifier identifier = layer_index;
        const nrd::DispatchDesc *dispatches{}; uint32_t dispatch_count{};
        nrd_check(nrd::GetComputeDispatches(*layer.instance, &identifier, 1u, dispatches, dispatch_count), "GetComputeDispatches");
        const auto &instance = nrd::GetInstanceDesc(*layer.instance);
        nvrhi::TimerQueryHandle timer, nrd_timer;
        const uint64_t serial = profile_serial++;
        std::vector<ProfileSample> pass_samples;
        std::vector<SnapshotSample> snapshots;
        uint32_t snapshot_ordinal{};
        uint32_t pass_index{};
        command_list->open();
        // A volatile buffer needs a version written in every opened command
        // list, even when reset/clear dispatches have no own constant data.
        command_list->writeBuffer(layer.constants, layer.zero_constants.data(), layer.zero_constants.size());
        if (!profile_path.empty()) { timer = device->createTimerQuery(); require(bool(timer), "Native REBLUR: timer allocation failed"); command_list->beginTimerQuery(timer); }
        auto begin_pass = [&](const char *name) {
            nvrhi::TimerQueryHandle query;
            if (!pass_profile_path.empty()) {
                query = device->createTimerQuery();
                require(bool(query), "Native REBLUR: per-pass timer allocation failed");
                pass_samples.push_back({query, {}, serial, width, height, parameters.frame, layer_index, pass_index, name ? name : "Unnamed", true});
                command_list->beginTimerQuery(query);
            }
            ++pass_index;
            return query;
        };
        auto end_pass = [&](const nvrhi::TimerQueryHandle &query) { if (query) command_list->endTimerQuery(query); };
        const auto pack_timer = begin_pass("Pack");
        nvrhi::ComputeState pack_state; pack_state.pipeline = pack.pipeline; pack_state.bindings = {layer.pack_binding};
        command_list->setComputeState(pack_state); command_list->setPushConstants(&conversion, sizeof(conversion));
        command_list->dispatch((width + 15u) / 16u, (height + 7u) / 8u);
        external_buffer_barrier();
        end_pass(pack_timer);
        if (!snapshot_directory.empty()) {
            snapshot_constants(parameters.frame, layer_index, 0u, &conversion, sizeof(conversion));
            snapshot_texture(snapshots, layer.input_diffuse, parameters.frame, layer_index, 0u, 0u,
                             nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
            snapshot_texture(snapshots, layer.input_specular, parameters.frame, layer_index, 0u, 1u,
                             nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
            snapshot_texture(snapshots, layer.normal, parameters.frame, layer_index, 0u, 2u,
                             nrd::ResourceType::IN_NORMAL_ROUGHNESS, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
            snapshot_texture(snapshots, layer.view_z, parameters.frame, layer_index, 0u, 3u,
                             nrd::ResourceType::IN_VIEWZ, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
            snapshot_texture(snapshots, layer.motion, parameters.frame, layer_index, 0u, 4u,
                             nrd::ResourceType::IN_MV, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
            snapshot_texture(snapshots, layer.disocclusion_mix, parameters.frame, layer_index, 0u, 5u,
                             nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX, 0u, "Pack", "NativeReblurPack.hlsl", sizeof(conversion));
        }
        if (timer) {
            nrd_timer = device->createTimerQuery();
            require(bool(nrd_timer), "Native REBLUR: NRD timer allocation failed");
            command_list->beginTimerQuery(nrd_timer);
        }
        for (uint32_t dispatch_index = 0u; dispatch_index < dispatch_count; ++dispatch_index) {
            const auto &dispatch = dispatches[dispatch_index];
            const auto &pipeline = layer.pipelines.at(dispatch.pipelineIndex);
            const auto &description = instance.pipelines[dispatch.pipelineIndex];
            const bool hf_probe_dispatch = bool(layer.hf_probe) &&
                std::strcmp(description.shaderFileName, "REBLUR_DiffuseSpecular_HistoryFix.cs") == 0;
            const bool ta_probe_dispatch = bool(layer.ta_probe) &&
                std::strcmp(description.shaderFileName, "REBLUR_DiffuseSpecular_TemporalAccumulation.cs") == 0;
            const bool ts_probe_dispatch = bool(layer.ts_probe) &&
                std::strcmp(description.shaderFileName, "REBLUR_DiffuseSpecular_TemporalStabilization.cs") == 0;
            const auto pass_timer = begin_pass(dispatch.name ? dispatch.name : description.shaderFileName);
            if (dispatch.name) command_list->beginMarker(dispatch.name);
            if (dispatch.constantBufferDataSize)
                command_list->writeBuffer(layer.constants, dispatch.constantBufferData, dispatch.constantBufferDataSize);
            std::vector<uint32_t> key{layer_index, dispatch.pipelineIndex};
            for (uint32_t i = 0u; i < dispatch.resourcesNum; ++i) {
                key.push_back(static_cast<uint32_t>(dispatch.resources[i].type)); key.push_back(dispatch.resources[i].indexInPool);
                key.push_back(static_cast<uint32_t>(dispatch.resources[i].descriptorType));
            }
            auto found = layer.binding_cache.find(key);
            if (found == layer.binding_cache.end()) {
                nvrhi::BindingSetDesc bindings;
                bindings.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(instance.constantBufferRegisterIndex, layer.constants));
                for (uint32_t sampler = 0u; sampler < instance.samplersNum; ++sampler)
                    bindings.bindings.push_back(nvrhi::BindingSetItem::Sampler(instance.samplersBaseRegisterIndex + sampler, layer.samplers[sampler]));
                uint32_t resource_index{};
                for (uint32_t range_index = 0u; range_index < description.resourceRangesNum; ++range_index) {
                    const auto &range = description.resourceRanges[range_index];
                    for (uint32_t offset = 0u; offset < range.descriptorsNum; ++offset) {
                        require(resource_index < dispatch.resourcesNum, "Native REBLUR: resource range overflow");
                        auto *texture = resolve_texture(layer, dispatch.resources[resource_index++]);
                        bindings.bindings.push_back(range.descriptorType == nrd::DescriptorType::TEXTURE ?
                            nvrhi::BindingSetItem::Texture_SRV(range.baseRegisterIndex + offset, texture) :
                            nvrhi::BindingSetItem::Texture_UAV(range.baseRegisterIndex + offset, texture));
                    }
                }
                require(resource_index == dispatch.resourcesNum, "Native REBLUR: resource range count mismatch");
                if (hf_probe_dispatch)
                    bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(4u, layer.hf_probe));
                if (ta_probe_dispatch)
                    bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(7u, layer.ta_probe));
                if (ts_probe_dispatch)
                    bindings.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(6u, layer.ts_probe));
                auto binding = device->createBindingSet(bindings, pipeline.layout);
                require(bool(binding), "Native REBLUR: denoiser binding set failed");
                found = layer.binding_cache.emplace(std::move(key), std::move(binding)).first;
            }
            if (hf_probe_dispatch)
                command_list->clearTextureFloat(layer.hf_probe, nvrhi::AllSubresources, nvrhi::Color(0.f));
            if (ta_probe_dispatch)
                command_list->clearTextureFloat(layer.ta_probe, nvrhi::AllSubresources, nvrhi::Color(0.f));
            if (ts_probe_dispatch)
                command_list->clearTextureFloat(layer.ts_probe, nvrhi::AllSubresources, nvrhi::Color(0.f));
            nvrhi::ComputeState state; state.pipeline = pipeline.pipeline; state.bindings = {found->second};
            command_list->setComputeState(state);
            command_list->dispatch(dispatch.gridWidth, dispatch.gridHeight);
            if (std::strncmp(description.shaderFileName, "Clear_", 6u) == 0) {
                // Reset clear dispatches need an explicit dependency before
                // another clear or denoising dispatch accesses the pool.
                command_list->commitBarriers();
                const VkCommandBuffer native = command_list->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
            }
            if (dispatch.name) command_list->endMarker();
            end_pass(pass_timer);
            if (!snapshot_directory.empty() && std::strncmp(description.shaderFileName, "Clear_", 6u) != 0) {
                ++snapshot_ordinal;
                snapshot_constants(parameters.frame, layer_index, snapshot_ordinal,
                                   dispatch.constantBufferData, dispatch.constantBufferDataSize);
                uint32_t resource_index{};
                for (uint32_t range_index = 0u; range_index < description.resourceRangesNum; ++range_index) {
                    const auto &range = description.resourceRanges[range_index];
                    for (uint32_t offset = 0u; offset < range.descriptorsNum; ++offset) {
                        const auto &resource = dispatch.resources[resource_index++];
                        if (range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE)
                            snapshot_texture(snapshots, resolve_texture(layer, resource), parameters.frame, layer_index,
                                             snapshot_ordinal, range.baseRegisterIndex + offset, resource.type,
                                             resource.indexInPool, dispatch.name, description.shaderFileName,
                                             dispatch.constantBufferDataSize);
                    }
                }
                if (hf_probe_dispatch)
                    snapshot_texture(snapshots, layer.hf_probe, parameters.frame, layer_index, snapshot_ordinal, 4u,
                                     nrd::ResourceType::MAX_NUM, UINT32_MAX, dispatch.name, description.shaderFileName,
                                     dispatch.constantBufferDataSize);
                if (ta_probe_dispatch)
                    snapshot_texture(snapshots, layer.ta_probe, parameters.frame, layer_index, snapshot_ordinal, 7u,
                                     nrd::ResourceType::MAX_NUM, UINT32_MAX, dispatch.name, description.shaderFileName,
                                     dispatch.constantBufferDataSize);
                if (ts_probe_dispatch)
                    snapshot_texture(snapshots, layer.ts_probe, parameters.frame, layer_index, snapshot_ordinal, 6u,
                                     nrd::ResourceType::MAX_NUM, UINT32_MAX, dispatch.name, description.shaderFileName,
                                     dispatch.constantBufferDataSize);
            }
        }
        if (nrd_timer) command_list->endTimerQuery(nrd_timer);
        const auto unpack_timer = begin_pass("Unpack");
        external_buffer_barrier();
        nvrhi::ComputeState unpack_state; unpack_state.pipeline = unpack.pipeline; unpack_state.bindings = {layer.unpack_binding};
        command_list->setComputeState(unpack_state); command_list->setPushConstants(&conversion, sizeof(conversion));
        command_list->dispatch((width + 15u) / 16u, (height + 7u) / 8u);
        external_buffer_barrier();
        end_pass(unpack_timer);
        if (timer) command_list->endTimerQuery(timer);
        command_list->close();
        nvrhi::EventQueryHandle completion;
        if (timer || !pass_samples.empty() || !snapshots.empty()) {
            completion = device->createEventQuery();
            require(bool(completion), "Native REBLUR: completion event allocation failed");
        }
        device->executeCommandList(command_list, nvrhi::CommandQueue::Compute);
        if (completion) device->setEventQuery(completion, nvrhi::CommandQueue::Compute);
        if (!snapshots.empty()) {
            device->waitEventQuery(completion);
            write_snapshots(snapshots);
        }
        if (timer) pending_profiles.push_back({timer, completion, serial, width, height, parameters.frame, layer_index, 0u, "REBLUR-Filter", false});
        if (nrd_timer) pending_profiles.push_back({nrd_timer, completion, serial, width, height, parameters.frame, layer_index, 0u, "REBLUR-NRD", false});
        for (auto &sample : pass_samples) {
            sample.completion = completion;
            pending_profiles.push_back(std::move(sample));
        }
        layer.frames[layer_index] = parameters.frame;
    }
};

NativeReblur::NativeReblur(std::filesystem::path directory)
    : impl_(std::make_shared<Impl>(std::move(directory))) {}
NativeReblur::~NativeReblur() = default;

bool NativeReblur::requested() noexcept {
    const char *value = std::getenv("REBLUR_NATIVE_NRD");
    return value && std::strcmp(value, "1") == 0;
}

ocarina::Command *NativeReblur::dispatch(FilterParam parameters, NativeCameraParams camera,
                                        uint layer, Settings settings, bool reset_history,
                                        std::shared_ptr<void> resource_owner) {
    return ocarina::native_callback([state = impl_, parameters, camera, layer, settings, reset_history,
                                     resource_owner = std::move(resource_owner)](const ocarina::NativeInteropContext &context) {
        (void)resource_owner; // Retained with the callback through its external GPU fence.
        require(context.backend == ocarina::NativeInteropBackend::VulkanCompute && context.native,
                "Native REBLUR: Vulkan compute interop is required");
        state->run(*static_cast<const ocarina::VulkanInteropContext *>(context.native), parameters, camera, layer, settings, reset_history);
    });
}

ocarina::Command *NativeReblur::sampling_probe(ocarina::BufferDesc<float4> output,
                                             std::filesystem::path probe_spirv) {
    // Retain the probe's pipeline/image/sampler through the native callback's
    // external GPU fence, just as for the ordinary native dispatch resources.
    return ocarina::native_callback([state = impl_, output, probe_spirv = std::move(probe_spirv)]
        (const ocarina::NativeInteropContext &context) {
        require(context.backend == ocarina::NativeInteropBackend::VulkanCompute && context.native,
                "Native REBLUR: Vulkan compute interop is required for the sampling probe");
        state->run_sampling_probe(*static_cast<const ocarina::VulkanInteropContext *>(context.native), output, probe_spirv);
    });
}

ocarina::Command *NativeReblur::format_store_probe(std::vector<float4> samples,
                                                 std::filesystem::path probe_spirv,
                                                 std::filesystem::path output_prefix) {
    return ocarina::native_callback([state = impl_, samples = std::move(samples),
                                     probe_spirv = std::move(probe_spirv), output_prefix = std::move(output_prefix)]
        (const ocarina::NativeInteropContext &context) {
        require(context.backend == ocarina::NativeInteropBackend::VulkanCompute && context.native,
                "Native REBLUR: Vulkan compute interop is required for the format store probe");
        state->run_format_store_probe(*static_cast<const ocarina::VulkanInteropContext *>(context.native),
                                      samples, probe_spirv, output_prefix);
    });
}

}// namespace vision::reblur
