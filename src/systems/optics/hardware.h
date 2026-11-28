#pragma once

#include "horizon.h"

#include "Codegen/BuiltinVariate.h"
#include "Codegen/ControlFlows.h"
#include "Codegen/CustomLibrary.h"
#include "Codegen/TypeAlias.h"

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
#include GLSL(shaders/image_readback.comp.glsl)
#include GLSL(shaders/optics_overlay.comp.glsl)
#include GLSL(shaders/optics_ui_warp.comp.glsl)
#include GLSL(shaders/optics_composite.comp.glsl)
// clang-format on

// ============================================================================
// FramePlaceBufferPool — 消除 V-buffer 共享 buffer 的跨帧/跨相机覆盖竞争
// ============================================================================
// Each UI submission holds its lease until the GPU is done reading its tables.
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

private:
    struct Slot {
        Corona::Horizon::HardwareBuffer buffer;
        std::uint64_t capacity{0};
        std::shared_ptr<int> busy;  // nullptr=从未用；use_count()>1=GPU 在飞
    };
    std::deque<Slot> slots_;
};

struct Hardware {
    Corona::Horizon::HardwareImage uiVisibilityImage;  // Pass 2 visibility, isolated from scene pass
    Corona::Horizon::HardwareImage uiDepthImage;        // Pass 2 depth, isolated from scene pass
    Corona::Horizon::HardwareExecutor executor;
    // Horizon 移除了 HardwareExecutor::last_receipt()，Hardware 自己记住最后一次提交。
    Corona::Horizon::SubmitReceipt last_receipt;

    FramePlaceBufferPool uiVpUniformBufferPool;
    FramePlaceBufferPool uiInstanceInfoBufferPool;
    FramePlaceBufferPool uiMaterialTableBufferPool;

    // === Shader pipelines ===
    bool shaderHasInit = false;
    std::optional<Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>> uiVisibilityPipeline;
    std::optional<Corona::Horizon::ComputePipeline<image_readback_comp_glsl_t>> screenshotReadbackPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_overlay_comp_glsl_t>> opticsOverlayPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_ui_warp_comp_glsl_t>> opticsUiWarpPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_composite_comp_glsl_t>> opticsCompositePipeline;
    EmbeddedShader::Array<ktm::fvec4> visionDisplayPixels;
    EmbeddedShader::Texture2D<ktm::fvec4> visionDisplayImage;
    EmbeddedShader::Uint2 visionDisplayExtent;
    std::optional<Corona::Horizon::ComputePipeline<>> visionDisplayPipeline;

    // === CPU-side uniform data ===
    struct VPUniformBufferObject {
        ktm::fmat4x4 viewProjMatrix;
    };

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

    // Word layout read by optics_overlay.comp.glsl.
    struct MaterialInfo {
        uint32_t textureDescriptor;
        std::array<float, 4> materialColor;
    };
    static_assert(sizeof(MaterialInfo) == 5 * sizeof(uint32_t));
    static_assert(offsetof(MaterialInfo, materialColor) == sizeof(uint32_t));

};
