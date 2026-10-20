#pragma once

#include "horizon.h"

#include "Codegen/BuiltinVariate.h"
#include "Codegen/ControlFlows.h"
#include "Codegen/CustomLibrary.h"
#include "Codegen/TypeAlias.h"
#include "native_frame_throttle.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// clang-format off
#include GLSL(shaders/visibility.vert.glsl)
#include GLSL(shaders/visibility.frag.glsl)
#include GLSL(shaders/shadow.vert.glsl)
#include GLSL(shaders/shadow.frag.glsl)
#include GLSL(shaders/lighting.comp.glsl)
#include GLSL(shaders/sky.comp.glsl)
#include GLSL(shaders/sky_sh_project.comp.glsl)
#include GLSL(shaders/tonemap.comp.glsl)
#include GLSL(shaders/actor_pick.comp.glsl)
#include GLSL(shaders/image_readback.comp.glsl)
#include GLSL(shaders/optics_overlay.comp.glsl)
#include GLSL(shaders/optics_gizmo.comp.glsl)
#include GLSL(shaders/optics_cursor.comp.glsl)
#include GLSL(shaders/optics_ui_warp.comp.glsl)
#include GLSL(shaders/optics_composite.comp.glsl)
#include GLSL(shaders/vision_resolve.comp.glsl)
// clang-format on

// ============================================================================
// FramePlaceBufferPool — 消除 V-buffer 共享 buffer 的跨帧/跨相机覆盖竞争
// ============================================================================
// 背景：V-buffer 延迟着色第 2 趟（lighting/ssao/debug_resolve/overlay compute）
// 在 GPU 执行时才从 SSBO 回读 vp / instance / material / ubo / shadow 表，重投影
// 三角形算重心坐标→UV。这些表原本是 Hardware 的单例，CPU 每相机/每帧立即 memcpy
// 覆写、commit 又异步不等待——于是前一相机的 compute 还在飞，其读取的 buffer 已被
// 下一相机的 CPU 写覆盖，导致纹理错位（错误 VP）与 LOD 闪烁（错误 instance 表）。
// Vulkan barrier 只能串行化 GPU-GPU，管不了 CPU memcpy，故必须换缓冲。
//
// 本池按 per-submission 租用：acquire() 返回一份当前无 GPU 在用的 buffer；调用方写
// 它、取 storeDescriptor() 进 push constant，并在 commit 前 `stream << keep_alive(busy)`。
// GPU 完成后 executor retire() 会 drop 该 busy 哨兵（device_manager.cpp:332），使
// busy.use_count() 落回 1，下次 acquire 即可复用。每次 commit 内 Queue::acquire()
// 都先 retire_completed()，故完成即回收，池稳态大小 = 实际在飞 submission 数。
//
// 为何用哨兵 shared_ptr 而非直接查 buffer：HardwareBuffer 的引用计数是 private，
// 外部不可查；horizon 也无公开的 completion-poll。哨兵是唯一可观测的在飞标记。
class FramePlaceBufferPool {
public:
    struct Lease {
        Corona::Horizon::HardwareBuffer* buffer{nullptr};
        std::uint64_t capacity{0};
        std::shared_ptr<int> busy;  // 传给 stream << keep_alive(busy)
    };

    // factory: 无空闲槽时用它新建底层 buffer（尺寸/类型由调用方闭包决定）。
    template <typename Factory>
    Lease acquire(Factory&& factory) {
        return acquire(0, std::forward<Factory>(factory));
    }

    template <typename Factory>
    Lease acquire(std::uint64_t required_capacity, Factory&& factory) {
        for (auto& slot : slots_) {
            // busy 为空（从未租出）或仅池自己持有（use_count()==1，GPU 已完成并
            // retire 掉哨兵）→ 该槽空闲，可复用其已建好的 buffer。
            if (!slot.busy || slot.busy.use_count() == 1) {
                if (required_capacity > slot.capacity || !slot.buffer) {
                    slot.buffer = factory();
                    slot.capacity = required_capacity;
                }
                slot.busy = std::make_shared<int>(0);
                return Lease{&slot.buffer, slot.capacity, slot.busy};
            }
        }
        // 全部在飞 → 新增一槽（稳态极少触发；池大小自适应收敛到在飞数）。
        Slot slot;
        slot.buffer = factory();
        slot.capacity = required_capacity;
        slot.busy = std::make_shared<int>(0);
        slots_.push_back(std::move(slot));
        return Lease{&slots_.back().buffer, slots_.back().capacity, slots_.back().busy};
    }

    // Horizon 移除了 HardwareBuffer::storeDescriptor()（只剩 HardwareImage::store_descriptor()）。
    // 池返回的是原始 buffer，故提供个 store_descriptor() 包装转发。
    static uint32_t store_descriptor(Corona::Horizon::HardwareBuffer& buffer) {
        return buffer.store_descriptor();
    }

private:
    struct Slot {
        Corona::Horizon::HardwareBuffer buffer;
        std::uint64_t capacity{0};
        std::shared_ptr<int> busy;  // nullptr=从未用；use_count()>1=GPU 在飞
    };
    std::deque<Slot> slots_;
};

struct Hardware {
    Corona::Systems::OpticsDetail::NativeFrameThrottle native_frame_throttle;
    // === Visibility Buffer (replaces GBuffer rasterization output) ===
    Corona::Horizon::HardwareImage visibilityImage;  // RG32_UINT: R=instanceID, G=primitiveID
    Corona::Horizon::HardwareImage depthImage;       // D32_FLOAT: depth (kept from GBuffer)
    Corona::Horizon::HardwareImage uiVisibilityImage;  // Pass 2 visibility, isolated from scene pass
    Corona::Horizon::HardwareImage uiDepthImage;        // Pass 2 depth, isolated from scene pass
    std::array<Corona::Horizon::HardwareImage, 4> shadowCascadeImages;

    // === Final composited output ===
    Corona::Horizon::HardwareImage finalOutputImage;
    Corona::Horizon::HardwareImage cursorIconImage;
    bool cursorIconLoadAttempted = false;
    std::array<Corona::Horizon::HardwareImage, 3> gizmoAxisImages;
    bool gizmoAxisLoadAttempted = false;
    Corona::Horizon::HardwareExecutor executor;
    // Horizon 移除了 HardwareExecutor::last_receipt()，Hardware 自己记住最后一次提交。
    Corona::Horizon::SubmitReceipt last_receipt;

    // === Uniform buffers ===
    Corona::Horizon::HardwareBuffer uniformBuffer;
    Corona::Horizon::HardwareBuffer vpUniformBuffer;  // renamed: view-projection matrix
    Corona::Horizon::HardwareBuffer uiVpUniformBuffer;  // Pass 2 orthographic view-projection matrix

    // === Instance & Material tables (uploaded per frame) ===
    Corona::Horizon::HardwareBuffer instanceInfoBuffer;
    Corona::Horizon::HardwareBuffer materialTableBuffer;
    Corona::Horizon::HardwareBuffer uiInstanceInfoBuffer;
    Corona::Horizon::HardwareBuffer uiMaterialTableBuffer;
    std::uint64_t instanceInfoCapacity = 0;
    std::uint64_t materialTableCapacity = 0;
    std::uint64_t uiInstanceInfoCapacity = 0;
    std::uint64_t uiMaterialTableCapacity = 0;
    Corona::Horizon::HardwareBuffer skyIrradianceSHBuffer;  // 9 vec3 SH coeffs (sky-driven ambient)
    Corona::Horizon::HardwareBuffer shadowInfoBuffer;

    // === Per-submission buffer 池（消除 V-buffer 共享 buffer 跨帧/跨相机覆盖竞争）===
    // 上面这些 *Buffer 单例的语义已改为"每相机从对应池租一份写入"，见 optics_system.cpp
    // 场景/overlay pass。单例本身仅保留给 screenshot 等冷路径。
    FramePlaceBufferPool vpUniformBufferPool;
    FramePlaceBufferPool uniformBufferPool;
    FramePlaceBufferPool shadowInfoBufferPool;
    FramePlaceBufferPool instanceInfoBufferPool;
    FramePlaceBufferPool materialTableBufferPool;
    FramePlaceBufferPool uiVpUniformBufferPool;
    FramePlaceBufferPool uiInstanceInfoBufferPool;
    FramePlaceBufferPool uiMaterialTableBufferPool;

    // === Shader pipelines ===
    bool shaderHasInit = false;
    std::optional<Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>> visibilityPipeline;
    std::optional<Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>> uiVisibilityPipeline;
    std::array<std::optional<Corona::Horizon::RasterizerPipeline<shadow_vert_glsl_t, shadow_frag_glsl_t>>, 4> shadowPipelines;
    std::optional<Corona::Horizon::ComputePipeline<lighting_comp_glsl_t>> lightingPipeline;
    std::optional<Corona::Horizon::ComputePipeline<sky_comp_glsl_t>> skyPipeline;
    std::optional<Corona::Horizon::ComputePipeline<sky_sh_project_comp_glsl_t>> skySHProjectPipeline;
    std::optional<Corona::Horizon::ComputePipeline<tonemap_comp_glsl_t>> tonemapPipeline;
    std::optional<Corona::Horizon::ComputePipeline<actor_pick_comp_glsl_t>> actorPickPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_overlay_comp_glsl_t>> opticsOverlayPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_gizmo_comp_glsl_t>> opticsGizmoPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_cursor_comp_glsl_t>> opticsCursorPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_ui_warp_comp_glsl_t>> opticsUiWarpPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_composite_comp_glsl_t>> opticsCompositePipeline;
    std::optional<Corona::Horizon::ComputePipeline<vision_resolve_comp_glsl_t>> visionResolvePipeline;

    // === CPU-side uniform data ===
    struct UniformBufferObject {
        // Light data (for shadow mapping, etc.)
        ktm::fvec4 lightPosition;
        ktm::fmat4x4 lightViewMatrix;
        ktm::fmat4x4 lightProjMatrix;

        // Eye/Camera data
        ktm::fvec4 eyePosition;
        ktm::fvec4 eyeDir;
        ktm::fmat4x4 eyeViewMatrix;
        ktm::fmat4x4 eyeProjMatrix;
        ktm::fmat4x4 eyeInvProjMatrix;
    } uniformBufferObjects{};

    struct VPUniformBufferObject {
        ktm::fmat4x4 viewProjMatrix;
    } vpUniformBufferObjects{};

    struct ShadowInfoBufferObject {
        std::array<ktm::fmat4x4, 4> lightViewProj;
        ktm::fvec4 cascadeSplits;
        std::array<uint32_t, 4> shadowMapDescriptors{};
        float shadowMapSize = 1024.0f;
        float shadowBias = 0.0015f;
        uint32_t shadowEnabled = 0;
        uint32_t padding0 = 0;
    } shadowInfoBufferObjects{};

    // === GPU-side instance info table (matches GLSL InstanceInfo layout) ===
    // 96 bytes = 24 uints per entry:
    //   [0..15]  mat4 modelMatrix
    //   [16]     vertexBufferIndex
    //   [17]     indexBufferIndex
    //   [18]     materialID
    //   [19]     objectID
    //   [20]     indexCount
    //   [21]     vertexCount
    //   [22]     maxIndex
    //   [23]     flags
    struct InstanceInfo {
        ktm::fmat4x4 modelMatrix;
        uint32_t vertexBufferIndex;
        uint32_t indexBufferIndex;
        uint32_t materialID;
        uint32_t objectID;
        uint32_t indexCount;
        uint32_t vertexCount;
        uint32_t maxIndex;
        uint32_t flags;
    };
    static_assert(sizeof(InstanceInfo) == 24u * sizeof(uint32_t),
                  "InstanceInfo must match the 24-uint shader layout");
    static_assert(offsetof(InstanceInfo, vertexBufferIndex) == 16u * sizeof(uint32_t),
                  "InstanceInfo vertexBufferIndex offset must match shaders");
    static_assert(offsetof(InstanceInfo, flags) == 23u * sizeof(uint32_t),
                  "InstanceInfo flags offset must match shaders");

    // === GPU-side material table (matches GLSL MaterialInfo layout) ===
    // 128 bytes = 32 uints per entry. The first 16 retain the legacy layout;
    // the remaining words carry model texture bindings and linear factors.
    //   [0]      textureDescriptor
    //   [1]      metallic  (as float bits)
    //   [2]      roughness (as float bits)
    //   [3]      subsurface
    //   [4]      specular
    //   [5]      specularTint
    //   [6]      anisotropic
    //   [7]      sheen
    //   [8]      sheenTint
    //   [9]      clearcoat
    //   [10]     clearcoatGloss
    //   [11]     lightingEnabled (原 padding0，1.0=受光, 0.0=不受光)
    //   [12..15] materialColor (vec4)
    struct MaterialInfo {
        uint32_t textureDescriptor;
        float metallic;
        float roughness;
        float subsurface;
        float specular;
        float specularTint;
        float anisotropic;
        float sheen;
        float sheenTint; // MR material (flags bit 5): specular weight; sheen is disabled
        float clearcoat;
        float clearcoatGloss;
        float lightingEnabled;  // 光照开关：1.0=接收光照, 0.0=不受光（始终使用基础颜色）
        ktm::fvec4 materialColor;
        uint32_t normalDescriptor{};
        uint32_t metallicDescriptor{};
        uint32_t roughnessDescriptor{};
        uint32_t occlusionDescriptor{};
        uint32_t emissiveDescriptor{};
        uint32_t specularDescriptor{};
        uint32_t specularColorDescriptor{};
        uint32_t flags{}; // metal channel[1:0], rough channel[3:2], double-sided[4], imported MR[5]
        std::array<float, 3> emissive{0.0f, 0.0f, 0.0f};
        float normalScale{1.0f};
        std::array<float, 3> specularColor{1.0f, 1.0f, 1.0f};
        float occlusionStrength{1.0f};
    };
    static_assert(sizeof(MaterialInfo) == 32u * sizeof(uint32_t),
                  "MaterialInfo must match the 32-uint shader layout");
    static_assert(offsetof(MaterialInfo, materialColor) == 12u * sizeof(uint32_t),
                  "MaterialInfo materialColor offset must match shaders");
    static_assert(offsetof(MaterialInfo, emissive) == 24u * sizeof(uint32_t));
    static_assert(offsetof(MaterialInfo, specularColor) == 28u * sizeof(uint32_t));

    // === Render dimensions ===
    ktm::uvec2 gbufferSize{};
};
