#pragma once
#include "filter.h"
#include "dsl/dsl.h"
#include <stdexcept>

namespace vision::reblur::edsl {
using namespace ocarina;
// Same field order and scalar widths as upstream REBLUR_SHARED_CONSTANTS.
struct alignas(16) SharedConstants {
    float4x4 gWorldToClip{};
    float4x4 gViewToClip{};
    float4x4 gViewToWorld{};
    float4x4 gWorldToViewPrev{};
    float4x4 gWorldToClipPrev{};
    float4x4 gWorldPrevToWorld{};
    float4 gRotatorPre{};
    float4 gRotator{};
    float4 gRotatorPost{};
    float4 gFrustum{};
    float4 gFrustumPrev{};
    float4 gCameraDelta{};
    float4 gHitDistParams{};
    float4 gViewVectorWorld{};
    float4 gViewVectorWorldPrev{};
    float4 gMvScale{};
    float2 gAntilagParams{};
    float2 gResourceSize{};
    float2 gResourceSizeInv{};
    float2 gResourceSizeInvPrev{};
    float2 gRectSize{};
    float2 gRectSizeInv{};
    float2 gRectSizePrev{};
    float2 gResolutionScale{};
    float2 gResolutionScalePrev{};
    float2 gRectOffset{};
    float2 gSpecProbabilityThresholdsForMvModification{};
    float2 gJitter{};
    uint2 gPrintfAt{};
    uint2 gRectOrigin{};
    int2 gRectSizeMinusOne{};
    float gDisocclusionThreshold{};
    float gDisocclusionThresholdAlternate{};
    float gCameraAttachedReflectionMaterialID{};
    float gStrandMaterialID{};
    float gStrandThickness{};
    float gStabilizationStrength{};
    float gDebug{};
    float gOrthoMode{};
    float gUnproject{};
    float gDenoisingRange{};
    float gPlaneDistSensitivity{};
    float gFramerateScale{};
    float gMinBlurRadius{};
    float gMaxBlurRadius{};
    float gDiffPrepassBlurRadius{};
    float gSpecPrepassBlurRadius{};
    float gMaxAccumulatedFrameNum{};
    float gMaxFastAccumulatedFrameNum{};
    float gAntiFirefly{};
    float gLobeAngleFraction{};
    float gRoughnessFraction{};
    float gResponsiveAccumulationRoughnessThreshold{};
    float gHistoryFixFrameNum{};
    float gHistoryFixBasePixelStride{};
    float gMinRectDimMulUnproject{};
    float gUsePrepassNotOnlyForSpecularMotionEstimation{};
    float gSplitScreen{};
    float gSplitScreenPrev{};
    float gCheckerboardResolveAccumSpeed{};
    float gViewZScale{};
    float gFireflySuppressorMinRelativeScale{};
    float gMinHitDistanceWeight{};
    float gDiffMinMaterial{};
    float gSpecMinMaterial{};
    uint gHasHistoryConfidence{};
    uint gHasDisocclusionThresholdMix{};
    uint gDiffCheckerboard{};
    uint gSpecCheckerboard{};
    uint gFrameIndex{};
    uint gIsRectChanged{};
    uint gResetHistory{};
};
struct ImageDesc {
    ulong texture{}, surface{};
    uint2 size{};
    ImageDesc() = default;
    ImageDesc(const Texture2D &image, uint2 extent)
        : texture(image->descriptor().texture), surface(image->descriptor().surface), size(extent) {}
};
struct InputImages {
    ImageDesc t0; ImageDesc t1; ImageDesc t2; ImageDesc t3; ImageDesc t4; ImageDesc t5; ImageDesc t6; ImageDesc t7; ImageDesc t8; ImageDesc t9; ImageDesc t10; ImageDesc t11; ImageDesc t12; ImageDesc t13; ImageDesc t14; ImageDesc t15; ImageDesc t16; ImageDesc t17; ImageDesc t18; ImageDesc t19;
    static constexpr uint size() { return 20u; }
    ImageDesc &operator[](uint slot) {
        switch (slot) {
            case 0u: return t0;
            case 1u: return t1;
            case 2u: return t2;
            case 3u: return t3;
            case 4u: return t4;
            case 5u: return t5;
            case 6u: return t6;
            case 7u: return t7;
            case 8u: return t8;
            case 9u: return t9;
            case 10u: return t10;
            case 11u: return t11;
            case 12u: return t12;
            case 13u: return t13;
            case 14u: return t14;
            case 15u: return t15;
            case 16u: return t16;
            case 17u: return t17;
            case 18u: return t18;
            case 19u: return t19;
            default: throw std::out_of_range("REBLUR image slot");
        }
    }
};
struct OutputImages {
    ImageDesc t0; ImageDesc t1; ImageDesc t2; ImageDesc t3; ImageDesc t4; ImageDesc t5; ImageDesc t6; ImageDesc t7;
    static constexpr uint size() { return 8u; }
    ImageDesc &operator[](uint slot) {
        switch (slot) {
            case 0u: return t0;
            case 1u: return t1;
            case 2u: return t2;
            case 3u: return t3;
            case 4u: return t4;
            case 5u: return t5;
            case 6u: return t6;
            case 7u: return t7;
            default: throw std::out_of_range("REBLUR image slot");
        }
    }
};
struct ComposeParam {
    BufferDesc<float4> diffuse, specular, bypass, stable_radiance, output;
    uint layer_count{1u}, layer_index{}, layered{};
    float direct_factor{1.f};
};
struct BatchUnpackParam {
    ImageDesc diffuse0, specular0, diffuse1, specular1, diffuse2, specular2;
    ComposeParam composition;
};
struct Dispatch {
    SharedConstants constants;
    InputImages inputs;
    OutputImages outputs;
    FilterParam external;
    float4 previous_view_depth_row{};
    uint reconstruction_5x5{}, temporal_stabilization{1u};
    uint output_alias_mask{}; // bit 0: diffuse output aliases raw; bit 1: specular
    uint defer_unpack{}; // Raw D/S retire into per-layer compact guides after Pack.
    uint inputs_precleared{}; // Sparse layers inherit exact invalid input texels.
};
}
OC_STRUCT(vision::reblur::edsl, SharedConstants, gWorldToClip, gViewToClip, gViewToWorld, gWorldToViewPrev, gWorldToClipPrev, gWorldPrevToWorld, gRotatorPre, gRotator, gRotatorPost, gFrustum, gFrustumPrev, gCameraDelta, gHitDistParams, gViewVectorWorld, gViewVectorWorldPrev, gMvScale, gAntilagParams, gResourceSize, gResourceSizeInv, gResourceSizeInvPrev, gRectSize, gRectSizeInv, gRectSizePrev, gResolutionScale, gResolutionScalePrev, gRectOffset, gSpecProbabilityThresholdsForMvModification, gJitter, gPrintfAt, gRectOrigin, gRectSizeMinusOne, gDisocclusionThreshold, gDisocclusionThresholdAlternate, gCameraAttachedReflectionMaterialID, gStrandMaterialID, gStrandThickness, gStabilizationStrength, gDebug, gOrthoMode, gUnproject, gDenoisingRange, gPlaneDistSensitivity, gFramerateScale, gMinBlurRadius, gMaxBlurRadius, gDiffPrepassBlurRadius, gSpecPrepassBlurRadius, gMaxAccumulatedFrameNum, gMaxFastAccumulatedFrameNum, gAntiFirefly, gLobeAngleFraction, gRoughnessFraction, gResponsiveAccumulationRoughnessThreshold, gHistoryFixFrameNum, gHistoryFixBasePixelStride, gMinRectDimMulUnproject, gUsePrepassNotOnlyForSpecularMotionEstimation, gSplitScreen, gSplitScreenPrev, gCheckerboardResolveAccumSpeed, gViewZScale, gFireflySuppressorMinRelativeScale, gMinHitDistanceWeight, gDiffMinMaterial, gSpecMinMaterial, gHasHistoryConfidence, gHasDisocclusionThresholdMix, gDiffCheckerboard, gSpecCheckerboard, gFrameIndex, gIsRectChanged, gResetHistory) {};
OC_PARAM_STRUCT(vision::reblur::edsl, ImageDesc, texture, surface, size) {};
OC_PARAM_STRUCT(vision::reblur::edsl, InputImages, t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15, t16, t17, t18, t19) {};
OC_PARAM_STRUCT(vision::reblur::edsl, OutputImages, t0, t1, t2, t3, t4, t5, t6, t7) {};
OC_PARAM_STRUCT(vision::reblur::edsl, ComposeParam, diffuse, specular, bypass, stable_radiance, output, layer_count, layer_index, layered, direct_factor) {};
OC_PARAM_STRUCT(vision::reblur::edsl, BatchUnpackParam, diffuse0, specular0, diffuse1, specular1, diffuse2, specular2, composition) {};
OC_PARAM_STRUCT(vision::reblur::edsl, Dispatch, constants, inputs, outputs, external, previous_view_depth_row, reconstruction_5x5, temporal_stabilization, output_alias_mask, defer_unpack, inputs_precleared) {};

namespace vision::reblur::edsl {
using PassShader = Shader<void(Dispatch)>;
using ComposingUnpackShader = Shader<void(Dispatch, ComposeParam)>;
using BatchedUnpackShader = Shader<void(Dispatch, BatchUnpackParam)>;
// Emit the unchanged format conversion while scene guides are still in registers.
void emit_pack_pixel(const Var<Dispatch> &parameters, const Var<Guide> &guide, const Float2 &repaired_hits);
PassShader compile_pack(Device &device);
PassShader compile_unpack(Device &device, bool compact_guide = false);
ComposingUnpackShader compile_unpack_and_compose(Device &device, bool compact_guide = true);
BatchedUnpackShader compile_unpack_batch(Device &device);
PassShader compile_classify(Device &device);
PassShader compile_reconstruction(Device &device, bool is_5x5);
PassShader compile_prepass(Device &device);
PassShader compile_blur(Device &device, bool post, bool temporal_stabilization = true);
PassShader compile_temporal(Device &device);
PassShader compile_history_fix(Device &device);
PassShader compile_stabilization(Device &device);
}
