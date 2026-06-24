#pragma once

#include "horizon.h"
#include <corona/shader_include.h>

#include "Codegen/BuiltinVariate.h"
#include "Codegen/ControlFlows.h"
#include "Codegen/CustomLibrary.h"
#include "Codegen/TypeAlias.h"

#include <optional>

// Helicon codegen translates GLSL `uint` to C++ `uint` (not `unsigned int`)
// in SSBO struct members. Provide the missing typedef.
using uint = uint32_t;

// Generated shader headers currently emit bare `Array` for SSBO pools.
struct Array {};

// clang-format off
#include GLSL(../../../assets/shaders/visibility.vert.glsl)
#include GLSL(../../../assets/shaders/visibility.frag.glsl)
#include GLSL(../../../assets/shaders/lighting.comp.glsl)
#include GLSL(../../../assets/shaders/sky.comp.glsl)
#include GLSL(../../../assets/shaders/sky_sh_project.comp.glsl)
#include GLSL(../../../assets/shaders/tonemap.comp.glsl)
#include GLSL(../../../assets/shaders/debug_resolve.comp.glsl)
#include GLSL(../../../assets/shaders/visibility_debug_resolve.comp.glsl)
#include GLSL(../../../assets/shaders/actor_pick.comp.glsl)
#include GLSL(../../../assets/shaders/optics_overlay.comp.glsl)
#include GLSL(../../../assets/shaders/optics_cursor.comp.glsl)
#include GLSL(../../../assets/shaders/optics_ui_warp.comp.glsl)
#include GLSL(../../../assets/shaders/optics_composite.comp.glsl)
#ifdef CORONA_ENABLE_VISION
#include GLSL(../../../assets/shaders/vision_resolve.comp.glsl)
#endif
// clang-format on

struct Hardware {
    // === Visibility Buffer (replaces GBuffer rasterization output) ===
    Corona::Horizon::HardwareImage visibilityImage;  // RGBA32_UINT: R=instanceID, G=primitiveID
    Corona::Horizon::HardwareImage depthImage;       // D32_FLOAT: depth (kept from GBuffer)
    Corona::Horizon::HardwareImage uiVisibilityImage;  // Pass 2 visibility, isolated from scene pass
    Corona::Horizon::HardwareImage uiDepthImage;        // Pass 2 depth, isolated from scene pass

    // === Final composited output ===
    Corona::Horizon::HardwareImage finalOutputImage;
    Corona::Horizon::HardwareImage cursorIconImage;
    bool cursorIconLoadAttempted = false;
    Corona::Horizon::HardwareExecutor executor;

    // === Uniform buffers ===
    Corona::Horizon::HardwareBuffer uniformBuffer;
    Corona::Horizon::HardwareBuffer vpUniformBuffer;  // renamed: view-projection matrix
    Corona::Horizon::HardwareBuffer uiVpUniformBuffer;  // Pass 2 orthographic view-projection matrix

    // === Instance & Material tables (uploaded per frame) ===
    Corona::Horizon::HardwareBuffer instanceInfoBuffer;
    Corona::Horizon::HardwareBuffer materialTableBuffer;
    Corona::Horizon::HardwareBuffer uiInstanceInfoBuffer;
    Corona::Horizon::HardwareBuffer uiMaterialTableBuffer;
    Corona::Horizon::HardwareBuffer actorPickBuffer;
    Corona::Horizon::HardwareBuffer skyIrradianceSHBuffer;  // 9 vec3 SH coeffs (sky-driven ambient)

    // === Shader pipelines ===
    bool shaderHasInit = false;
    std::optional<Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>> visibilityPipeline;
    std::optional<Corona::Horizon::RasterizerPipeline<visibility_vert_glsl_t, visibility_frag_glsl_t>> uiVisibilityPipeline;
    std::optional<Corona::Horizon::ComputePipeline<lighting_comp_glsl_t>> lightingPipeline;
    std::optional<Corona::Horizon::ComputePipeline<sky_comp_glsl_t>> skyPipeline;
    std::optional<Corona::Horizon::ComputePipeline<sky_sh_project_comp_glsl_t>> skySHProjectPipeline;
    std::optional<Corona::Horizon::ComputePipeline<tonemap_comp_glsl_t>> tonemapPipeline;
    std::optional<Corona::Horizon::ComputePipeline<debug_resolve_comp_glsl_t>> debugResolvePipeline;
    std::optional<Corona::Horizon::ComputePipeline<visibility_debug_resolve_comp_glsl_t>> visibilityDebugResolvePipeline;
    std::optional<Corona::Horizon::ComputePipeline<actor_pick_comp_glsl_t>> actorPickPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_overlay_comp_glsl_t>> opticsOverlayPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_cursor_comp_glsl_t>> opticsCursorPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_ui_warp_comp_glsl_t>> opticsUiWarpPipeline;
    std::optional<Corona::Horizon::ComputePipeline<optics_composite_comp_glsl_t>> opticsCompositePipeline;
#ifdef CORONA_ENABLE_VISION
    std::optional<Corona::Horizon::ComputePipeline<vision_resolve_comp_glsl_t>> visionResolvePipeline;
#endif

    // === CPU-side uniform data ===
    struct UniformBufferObject {
        // Light data (for shadow mapping, etc.)
        ktm::fvec3 lightPosition;
        float padding0;
        ktm::fmat4x4 lightViewMatrix;
        ktm::fmat4x4 lightProjMatrix;

        // Eye/Camera data
        ktm::fvec3 eyePosition;
        float padding1;
        ktm::fvec3 eyeDir;
        float padding2;
        ktm::fmat4x4 eyeViewMatrix;
        ktm::fmat4x4 eyeProjMatrix;
    } uniformBufferObjects{};

    struct VPUniformBufferObject {
        ktm::fmat4x4 viewProjMatrix;
    } vpUniformBufferObjects{};

    // === GPU-side instance info table (matches GLSL InstanceInfo layout) ===
    // 80 bytes = 20 uints per entry:
    //   [0..15]  mat4 modelMatrix
    //   [16]     vertexBufferIndex
    //   [17]     indexBufferIndex
    //   [18]     materialID
    //   [19]     objectID
    struct InstanceInfo {
        ktm::fmat4x4 modelMatrix;
        uint32_t vertexBufferIndex;
        uint32_t indexBufferIndex;
        uint32_t materialID;
        uint32_t objectID;
    };

    // === GPU-side material table (matches GLSL MaterialInfo layout) ===
    // 64 bytes = 16 uints per entry:
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
        float sheenTint;
        float clearcoat;
        float clearcoatGloss;
        float lightingEnabled;  // 光照开关：1.0=接收光照, 0.0=不受光（始终使用基础颜色）
        ktm::fvec4 materialColor;
    };

    // === Render dimensions ===
    ktm::uvec2 gbufferSize{};
};
