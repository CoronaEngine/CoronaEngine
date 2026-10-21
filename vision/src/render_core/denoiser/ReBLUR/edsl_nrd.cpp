#include "edsl_nrd.h"
#include "edsl_nrd_params.h"
#include <NRD.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace vision::reblur {
namespace {
void require_edsl(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(std::string("EDSL REBLUR: ") + message);
}
void check_edsl(nrd::Result result, const char *operation) {
    require_edsl(result == nrd::Result::SUCCESS, operation);
}
nrd::ReblurSettings method_settings(const Settings &settings) {
    nrd::ReblurSettings result{};
    result.maxAccumulatedFrameNum = static_cast<uint>(std::clamp(settings.max_history, 0.f, 63.f));
    result.maxFastAccumulatedFrameNum = static_cast<uint>(std::clamp(settings.fast_history, 0.f, 63.f));
    result.maxStabilizedFrameNum = settings.stabilization <= 0.f ? 0u :
        static_cast<uint>(std::clamp(std::round(settings.stabilization / std::max(1.f - settings.stabilization, 1e-6f)), 0.f, 63.f));
    result.historyFixFrameNum = static_cast<uint>(std::clamp(settings.history_fix_frames, 0.f, 3.f));
    result.historyFixBasePixelStride = static_cast<uint>(std::max(settings.history_fix_stride, 1.f));
    result.diffusePrepassBlurRadius = settings.diffuse_prepass; result.specularPrepassBlurRadius = settings.specular_prepass;
    result.minBlurRadius = settings.min_radius; result.maxBlurRadius = settings.max_radius;
    result.lobeAngleFraction = settings.lobe_fraction; result.roughnessFraction = settings.roughness_fraction;
    result.planeDistanceSensitivity = settings.plane_sensitivity; result.minHitDistanceWeight = settings.min_hit_weight;
    result.antilagSettings.luminanceSigmaScale = settings.antilag_sigma;
    result.antilagSettings.luminanceSensitivity = settings.antilag_sensitivity;
    result.enableAntiFirefly = settings.anti_firefly != 0u;
    result.hitDistanceReconstructionMode = static_cast<nrd::HitDistanceReconstructionMode>(settings.reconstruction);
    result.enablePerformanceMode = false; result.checkerboardMode = nrd::CheckerboardMode::OFF;
    return result;
}
uint storage_bytes(nrd::Format format) {
    switch (format) {
        case nrd::Format::R8_UNORM: return 1u;
        case nrd::Format::RG8_UNORM: case nrd::Format::R16_SFLOAT: case nrd::Format::R16_UINT: return 2u;
        case nrd::Format::R32_SFLOAT: case nrd::Format::R32_UINT: case nrd::Format::R10_G10_B10_A2_UNORM: return 4u;
        case nrd::Format::RGBA16_SFLOAT: return 8u;
        default: throw std::runtime_error("EDSL REBLUR: unsupported pool format");
    }
}
struct Storage {
    Texture2D texture;
    uint2 size{};
    nrd::Format format{};
    [[nodiscard]] edsl::ImageDesc descriptor() const { return {texture, size}; }
};
struct Resources {
    nrd::Instance *instance{};
    uint2 size{};
    std::array<uint, 3> frames{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<Storage> permanent, transient;
    Storage motion, normal, view_z, disocclusion, input_d, input_s, output_d, output_s;
    Storage ta_probe, ts_probe, hf_probe;
    std::vector<std::byte> zeros;
    ~Resources() { if (instance) nrd::DestroyInstance(*instance); }
};
}

struct EdslReblur::Impl {
    Device &device;
    edsl::PassShader pack, unpack, classify, reconstruct3, reconstruct5, prepass, temporal, history_fix, blur, post, post_no_ts, stabilization;
    std::shared_ptr<Resources> resources;
    std::filesystem::path snapshot_directory;
    bool snapshot_frame_enabled{};
    uint32_t snapshot_frame{};
    bool ta_probe_enabled{}, ts_probe_enabled{}, hf_probe_enabled{};
    explicit Impl(Device &d) : device(d) {
        if (const char *value = std::getenv("REBLUR_SNAPSHOT_FRAME")) {
            const char *end = value + std::strlen(value);
            const auto parsed = std::from_chars(value, end, snapshot_frame);
            if (value == end || parsed.ec != std::errc{} || parsed.ptr != end)
                throw std::invalid_argument("REBLUR_SNAPSHOT_FRAME must be an unsigned decimal uint32");
            snapshot_frame_enabled = true;
        }
        if (const char *value = std::getenv("REBLUR_EDSL_HF_PROBE")) hf_probe_enabled = std::strcmp(value, "1") == 0;
        if (const char *value = std::getenv("REBLUR_EDSL_TA_PROBE")) ta_probe_enabled = std::strcmp(value, "1") == 0;
        if (const char *value = std::getenv("REBLUR_EDSL_EDGE_PROBE")) ts_probe_enabled = std::strcmp(value, "2") == 0;
        if (const char *path = std::getenv("REBLUR_EDSL_SNAPSHOT_DIR"); path && *path) {
            require_edsl(!std::getenv("REBLUR_GPU_PROFILE"), "snapshots and performance profiling cannot run together");
            snapshot_directory = path; std::filesystem::create_directories(snapshot_directory);
        }
        const auto &library = nrd::GetLibraryDesc();
        require_edsl(library.versionMajor == 4u && library.versionMinor == 14u && library.versionBuild == 3u &&
            library.normalEncoding == nrd::NormalEncoding::R10_G10_B10_A2_UNORM && library.roughnessEncoding == nrd::RoughnessEncoding::LINEAR,
            "incompatible NRD CPU runtime");
        pack = edsl::compile_pack(device); unpack = edsl::compile_unpack(device);
        classify = edsl::compile_classify(device);
        reconstruct3 = edsl::compile_reconstruction(device, false); reconstruct5 = edsl::compile_reconstruction(device, true);
        prepass = edsl::compile_prepass(device); temporal = edsl::compile_temporal(device);
        history_fix = edsl::compile_history_fix(device); blur = edsl::compile_blur(device, false);
        post = edsl::compile_blur(device, true); post_no_ts = edsl::compile_blur(device, true, false);
        stabilization = edsl::compile_stabilization(device);
    }
    Storage allocate(uint2 size, nrd::Format format) {
        PixelStorage storage;
        switch (format) {
            case nrd::Format::R8_UNORM: storage=PixelStorage::UNORM1; break;
            case nrd::Format::RG8_UNORM: storage=PixelStorage::UNORM2; break;
            case nrd::Format::R16_SFLOAT: storage=PixelStorage::HALF1; break;
            case nrd::Format::R16_UINT: storage=PixelStorage::USHORT1; break;
            case nrd::Format::R32_SFLOAT: storage=PixelStorage::FLOAT1; break;
            case nrd::Format::R32_UINT: storage=PixelStorage::UINT1; break;
            case nrd::Format::RGBA16_SFLOAT: storage=PixelStorage::HALF4; break;
            case nrd::Format::R10_G10_B10_A2_UNORM: storage=PixelStorage::RGB10A2; break;
            default: throw std::runtime_error("EDSL REBLUR: unsupported image format");
        }
        return {device.create_texture2d(size,storage,"REBLUR EDSL NRD resource"),size,format};
    }
    std::string snapshot_prefix(uint frame, uint layer, uint pass) const {
        return "f" + std::to_string(frame) + "_l" + std::to_string(layer) + "_p" + std::to_string(pass);
    }
    void snapshot(CommandBatch &commands, const Storage &storage, uint frame, uint layer, uint pass, uint slot, std::string_view shader_name) {
        if (snapshot_directory.empty() || (snapshot_frame_enabled && frame != snapshot_frame)) return;
        const std::string name = snapshot_prefix(frame, layer, pass) + "_u" + std::to_string(slot) + ".bin";
        const auto path = snapshot_directory / name;
        const size_t logical_bytes = size_t(storage.size.x) * storage.size.y * storage_bytes(storage.format);
        auto bytes = std::make_shared<std::vector<std::byte>>(logical_bytes);
        commands << storage.texture.download(bytes->data()) << synchronize();
        commands << [bytes, path, logical_bytes] {
            std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char *>(bytes->data()), logical_bytes);
            require_edsl(bool(file), "snapshot write failed");
        };
        std::ofstream meta(snapshot_directory / "metadata.jsonl", std::ios::app);
        meta << "{\"path\":\"" << name << "\",\"frame\":" << frame << ",\"layer\":" << layer
             << ",\"pass\":" << pass << ",\"slot\":" << slot << ",\"shader\":\"" << shader_name
             << "\",\"formatValue\":" << uint(storage.format) << ",\"width\":" << storage.size.x
             << ",\"height\":" << storage.size.y << ",\"rowBytes\":" << storage.size.x * storage_bytes(storage.format) << "}\n";
    }
    Resources &prepare(uint2 size) {
        if (resources && resources->size.x == size.x && resources->size.y == size.y) return *resources;
        auto r = std::make_shared<Resources>(); r->size = size;
        r->zeros.resize(size_t(size.x)*size.y*8u);
        const nrd::DenoiserDesc denoisers[]{{0u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR},
            {1u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}, {2u, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}};
        nrd::InstanceCreationDesc creation{}; creation.denoisers = denoisers; creation.denoisersNum = 3u;
        check_edsl(nrd::CreateInstance(creation, r->instance), "CreateInstance");
        const auto &desc = nrd::GetInstanceDesc(*r->instance);
        auto pool = [&](const nrd::TextureDesc *textures, uint count, std::vector<Storage> &result) {
            for (uint i = 0u; i != count; ++i) {
                uint factor = std::max(uint(textures[i].downsampleFactor), 1u);
                result.push_back(allocate(make_uint2((size.x + factor - 1u) / factor, (size.y + factor - 1u) / factor), textures[i].format));
            }
        };
        pool(desc.permanentPool, desc.permanentPoolSize, r->permanent);
        pool(desc.transientPool, desc.transientPoolSize, r->transient);
        r->motion = allocate(size, nrd::Format::RGBA16_SFLOAT); r->normal = allocate(size, nrd::Format::R10_G10_B10_A2_UNORM);
        r->view_z = allocate(size, nrd::Format::R32_SFLOAT); r->disocclusion = allocate(size, nrd::Format::R8_UNORM);
        r->input_d = allocate(size, nrd::Format::RGBA16_SFLOAT); r->input_s = allocate(size, nrd::Format::RGBA16_SFLOAT);
        r->output_d = allocate(size, nrd::Format::RGBA16_SFLOAT); r->output_s = allocate(size, nrd::Format::RGBA16_SFLOAT);
        if (ta_probe_enabled) {
            r->ta_probe = allocate(make_uint2(63u, 1u), nrd::Format::RGBA16_SFLOAT);
            r->zeros.resize(std::max(r->zeros.size(), size_t(63u * 8u)));
        }
        if (ts_probe_enabled) {
            r->ts_probe = allocate(make_uint2(4u, 1u), nrd::Format::RGBA16_SFLOAT);
            r->zeros.resize(std::max(r->zeros.size(), size_t(4u * 8u)));
        }
        if (hf_probe_enabled) {
            r->hf_probe = allocate(make_uint2(80u, 1u), nrd::Format::RGBA16_SFLOAT);
            r->zeros.resize(std::max(r->zeros.size(), size_t(80u * 8u)));
        }
        resources = std::move(r); return *resources;
    }
    static Storage &resolve(Resources &r, const nrd::ResourceDesc &resource) {
        switch (resource.type) {
            case nrd::ResourceType::IN_MV: return r.motion;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS: return r.normal;
            case nrd::ResourceType::IN_VIEWZ: return r.view_z;
            case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX: return r.disocclusion;
            case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST: return r.input_d;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: return r.input_s;
            case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST: return r.output_d;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: return r.output_s;
            case nrd::ResourceType::PERMANENT_POOL: return r.permanent.at(resource.indexInPool);
            case nrd::ResourceType::TRANSIENT_POOL: return r.transient.at(resource.indexInPool);
            default: throw std::runtime_error("EDSL REBLUR: unexpected optional resource");
        }
    }
    edsl::PassShader &shader(std::string_view file) {
        if (file.find("ClassifyTiles") != file.npos) return classify;
        if (file.find("HitDistReconstruction") != file.npos) return file.find("5x5") != file.npos ? reconstruct5 : reconstruct3;
        if (file.find("PrePass") != file.npos) return prepass;
        if (file.find("TemporalAccumulation") != file.npos) return temporal;
        if (file.find("HistoryFix") != file.npos) return history_fix;
        if (file.find("PostBlur") != file.npos) return file.find("NoTemporalStabilization") != file.npos ? post_no_ts : post;
        if (file.find("Blur") != file.npos) return blur;
        if (file.find("TemporalStabilization") != file.npos) return stabilization;
        throw std::runtime_error("EDSL REBLUR: unsupported scheduled pass: " + std::string(file));
    }
    CommandBatch dispatch(FilterParam p, const NativeCameraParams &camera, uint layer, const Settings &settings, bool reset) {
        require_edsl(layer < 3u && p.size.x && p.size.y && p.size.x <= UINT16_MAX && p.size.y <= UINT16_MAX, "invalid layer/resolution");
        require_edsl(std::isfinite(camera.denoising_range) && camera.denoising_range > 0.f, "invalid denoising range");
        auto &r = prepare(p.size);
        auto method = method_settings(settings);
        check_edsl(nrd::SetDenoiserSettings(*r.instance, layer, &method), "SetDenoiserSettings");
        nrd::CommonSettings common{};
        std::memcpy(common.worldToViewMatrix, camera.world_to_view.data(), sizeof(common.worldToViewMatrix));
        std::memcpy(common.worldToViewMatrixPrev, camera.world_to_view_prev.data(), sizeof(common.worldToViewMatrixPrev));
        std::memcpy(common.viewToClipMatrix, camera.view_to_clip.data(), sizeof(common.viewToClipMatrix));
        std::memcpy(common.viewToClipMatrixPrev, camera.view_to_clip_prev.data(), sizeof(common.viewToClipMatrixPrev));
        std::memcpy(common.cameraJitter, camera.jitter.data(), sizeof(common.cameraJitter));
        std::memcpy(common.cameraJitterPrev, camera.jitter_prev.data(), sizeof(common.cameraJitterPrev));
        common.motionVectorScale[0] = 1.f / float(p.size.x); common.motionVectorScale[1] = 1.f / float(p.size.y); common.motionVectorScale[2] = 1.f;
        common.isMotionVectorInWorldSpace = false; common.isDisocclusionThresholdMixAvailable = true;
        common.timeDeltaBetweenFrames = camera.frame_delta_ms; common.denoisingRange = camera.denoising_range;
        common.disocclusionThreshold = camera.disocclusion_threshold; common.disocclusionThresholdAlternate = camera.disocclusion_threshold_alternate;
        common.frameIndex = p.frame;
        common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = uint16_t(p.size.x);
        common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = uint16_t(p.size.y);
        const bool continuous = p.history_valid && r.frames[layer] != UINT32_MAX && r.frames[layer] + 1u == p.frame;
        common.accumulationMode = (reset || !continuous) ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
        check_edsl(nrd::SetCommonSettings(*r.instance, common), "SetCommonSettings");
        const nrd::Identifier identifier = layer;
        const nrd::DispatchDesc *dispatches{}; uint count{};
        check_edsl(nrd::GetComputeDispatches(*r.instance, &identifier, 1u, dispatches, count), "GetComputeDispatches");
        edsl::Dispatch parameters{}; parameters.external = p;
        parameters.constants.gDenoisingRange = camera.denoising_range;
        parameters.constants.gHitDistParams = make_float4(3.f, .1f, 20.f, -25.f);
        for (uint c = 0u; c != 4u; ++c) parameters.previous_view_depth_row[c] = camera.world_to_view_prev[c * 4u + 2u];
        auto alias = [](const auto &a, const auto &b) { return a.head() + a.offset_in_byte() == b.head() + b.offset_in_byte(); };
        parameters.output_alias_mask = uint(alias(p.raw_diffuse, p.output_diffuse)) | (uint(alias(p.raw_specular, p.output_specular)) << 1u);
        parameters.outputs[0] = r.input_d.descriptor(); parameters.outputs[1] = r.input_s.descriptor();
        parameters.outputs[2] = r.normal.descriptor(); parameters.outputs[3] = r.view_z.descriptor();
        parameters.outputs[4] = r.motion.descriptor(); parameters.outputs[5] = r.disocclusion.descriptor();
        CommandBatch commands;
        // ImageDesc contains borrowed bindless slots. Keep this resource
        // generation alive even if another resolution is queued before commit.
        // Native callback captures survive through the following GPU batch's
        // fence; the callback itself submits no work or host completion wait.
        commands << ocarina::native_callback([hold = resources](const ocarina::NativeInteropContext &) { (void)hold; });
        commands << pack(parameters).dispatch(p.size);
        const Storage *packed[]{&r.input_d, &r.input_s, &r.normal, &r.view_z, &r.motion, &r.disocclusion};
        for (uint slot=0u; slot!=6u; ++slot) snapshot(commands,*packed[slot],p.frame,layer,0u,slot,"Pack");
        const auto &instance = nrd::GetInstanceDesc(*r.instance);
        uint ordinal = 0u;
        for (uint i = 0u; i != count; ++i) {
            const auto &dispatch = dispatches[i];
            const std::string_view file(instance.pipelines[dispatch.pipelineIndex].shaderFileName);
            if (file.find("Clear") != file.npos) {
                for (uint slot = 0; slot != dispatch.resourcesNum; ++slot) {
                    require_edsl(dispatch.resources[slot].descriptorType == nrd::DescriptorType::STORAGE_TEXTURE, "clear bound a read-only resource");
                    commands << resolve(r, dispatch.resources[slot]).texture.upload(r.zeros.data());
                }
                continue;
            }
            require_edsl(dispatch.constantBufferDataSize == sizeof(edsl::SharedConstants), "shared constant layout mismatch");
            std::memcpy(&parameters.constants, dispatch.constantBufferData, sizeof(parameters.constants));
            const auto &pipeline = instance.pipelines[dispatch.pipelineIndex];
            uint resource = 0;
            for (uint range_index = 0; range_index != pipeline.resourceRangesNum; ++range_index) {
                const auto &range = pipeline.resourceRanges[range_index];
                for (uint offset = 0; offset != range.descriptorsNum; ++offset) {
                    require_edsl(resource < dispatch.resourcesNum, "resource range overflow");
                    auto image = resolve(r, dispatch.resources[resource++]).descriptor();
                    const auto slot = range.baseRegisterIndex + offset;
                    if (range.descriptorType == nrd::DescriptorType::TEXTURE) {
                        require_edsl(slot < parameters.inputs.size(), "input slot overflow"); parameters.inputs[slot] = image;
                    } else {
                        require_edsl(slot < parameters.outputs.size(), "output slot overflow"); parameters.outputs[slot] = image;
                    }
                }
            }
            require_edsl(resource == dispatch.resourcesNum, "resource range mismatch");
            parameters.temporal_stabilization = method.maxStabilizedFrameNum != 0u;
            parameters.reconstruction_5x5 = settings.reconstruction == HitDistanceReconstructionMode::AREA_5X5;
            const uint block_y = file.find("ClassifyTiles") != file.npos ? 4u : 16u;
            const bool capture_hf = hf_probe_enabled && file == "REBLUR_DiffuseSpecular_HistoryFix.cs";
            const bool capture_ta = ta_probe_enabled && file == "REBLUR_DiffuseSpecular_TemporalAccumulation.cs";
            const bool capture_ts = ts_probe_enabled && file == "REBLUR_DiffuseSpecular_TemporalStabilization.cs";
            if (capture_hf) {
                commands << r.hf_probe.texture.upload(r.zeros.data());
                parameters.outputs.t4 = r.hf_probe.descriptor();
            }
            if (capture_ta) {
                commands << r.ta_probe.texture.upload(r.zeros.data());
                parameters.outputs.t7 = r.ta_probe.descriptor();
            }
            if (capture_ts) {
                commands << r.ts_probe.texture.upload(r.zeros.data());
                parameters.outputs.t6 = r.ts_probe.descriptor();
            }
            commands << shader(file)(parameters).dispatch(make_uint2(dispatch.gridWidth * 8u, dispatch.gridHeight * block_y));
            ++ordinal;
            if (!snapshot_directory.empty() && (!snapshot_frame_enabled || p.frame == snapshot_frame)) {
                std::ofstream constants(snapshot_directory / (snapshot_prefix(p.frame,layer,ordinal) + ".constants.bin"), std::ios::binary);
                constants.write(reinterpret_cast<const char *>(dispatch.constantBufferData), dispatch.constantBufferDataSize);
                uint output_slot=0u;
                for (uint range_index=0u, resource_index=0u; range_index!=pipeline.resourceRangesNum; ++range_index) {
                    const auto &range=pipeline.resourceRanges[range_index];
                    for (uint offset=0u; offset!=range.descriptorsNum; ++offset,++resource_index) {
                        if (range.descriptorType==nrd::DescriptorType::STORAGE_TEXTURE) {
                            output_slot=range.baseRegisterIndex+offset;
                            snapshot(commands,resolve(r,dispatch.resources[resource_index]),p.frame,layer,ordinal,output_slot,file);
                        }
                    }
                }
            }
            if (capture_hf) snapshot(commands, r.hf_probe, p.frame, layer, ordinal, 4u, file);
            if (capture_ta) snapshot(commands, r.ta_probe, p.frame, layer, ordinal, 7u, file);
            if (capture_ts) snapshot(commands, r.ts_probe, p.frame, layer, ordinal, 6u, file);
        }
        parameters.inputs[0] = r.output_d.descriptor(); parameters.inputs[1] = r.output_s.descriptor();
        commands << unpack(parameters).dispatch(p.size);
        r.frames[layer] = p.frame;
        return commands;
    }
};
EdslReblur::EdslReblur(Device &device) : impl_(std::make_unique<Impl>(device)) {}
EdslReblur::~EdslReblur() = default;
CommandBatch EdslReblur::dispatch(FilterParam p, const NativeCameraParams &camera, uint layer, const Settings &settings, bool reset) {
    return impl_->dispatch(p, camera, layer, settings, reset);
}
}
