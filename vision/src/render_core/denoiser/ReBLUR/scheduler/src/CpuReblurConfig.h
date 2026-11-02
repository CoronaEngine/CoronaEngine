#pragma once

// CPU-only constant-buffer layout, extracted from NRD 4.14.3. No GPU shader code.
#define REBLUR_ACCUMSPEED_BITS                                  6
#define REBLUR_MAX_ACCUM_FRAME_NUM                              ( ( 1 << REBLUR_ACCUMSPEED_BITS ) - 1 )

#define REBLUR_SHARED_CONSTANTS \
    NRD_CONSTANT( float4x4, gWorldToClip ) \
    NRD_CONSTANT( float4x4, gViewToClip ) \
    NRD_CONSTANT( float4x4, gViewToWorld ) \
    NRD_CONSTANT( float4x4, gWorldToViewPrev ) \
    NRD_CONSTANT( float4x4, gWorldToClipPrev ) \
    NRD_CONSTANT( float4x4, gWorldPrevToWorld ) \
    NRD_CONSTANT( float4, gRotatorPre ) \
    NRD_CONSTANT( float4, gRotator ) \
    NRD_CONSTANT( float4, gRotatorPost ) \
    NRD_CONSTANT( float4, gFrustum ) \
    NRD_CONSTANT( float4, gFrustumPrev ) \
    NRD_CONSTANT( float4, gCameraDelta ) \
    NRD_CONSTANT( float4, gHitDistParams ) \
    NRD_CONSTANT( float4, gViewVectorWorld ) \
    NRD_CONSTANT( float4, gViewVectorWorldPrev ) \
    NRD_CONSTANT( float4, gMvScale ) \
    NRD_CONSTANT( float2, gAntilagParams ) \
    NRD_CONSTANT( float2, gResourceSize ) \
    NRD_CONSTANT( float2, gResourceSizeInv ) \
    NRD_CONSTANT( float2, gResourceSizeInvPrev ) \
    NRD_CONSTANT( float2, gRectSize ) \
    NRD_CONSTANT( float2, gRectSizeInv ) \
    NRD_CONSTANT( float2, gRectSizePrev ) \
    NRD_CONSTANT( float2, gResolutionScale ) \
    NRD_CONSTANT( float2, gResolutionScalePrev ) \
    NRD_CONSTANT( float2, gRectOffset ) \
    NRD_CONSTANT( float2, gSpecProbabilityThresholdsForMvModification ) \
    NRD_CONSTANT( float2, gJitter ) \
    NRD_CONSTANT( uint2, gPrintfAt ) \
    NRD_CONSTANT( uint2, gRectOrigin ) \
    NRD_CONSTANT( int2, gRectSizeMinusOne ) \
    NRD_CONSTANT( float, gDisocclusionThreshold ) \
    NRD_CONSTANT( float, gDisocclusionThresholdAlternate ) \
    NRD_CONSTANT( float, gCameraAttachedReflectionMaterialID ) \
    NRD_CONSTANT( float, gStrandMaterialID ) \
    NRD_CONSTANT( float, gStrandThickness ) \
    NRD_CONSTANT( float, gStabilizationStrength ) \
    NRD_CONSTANT( float, gDebug ) \
    NRD_CONSTANT( float, gOrthoMode ) \
    NRD_CONSTANT( float, gUnproject ) \
    NRD_CONSTANT( float, gDenoisingRange ) \
    NRD_CONSTANT( float, gPlaneDistSensitivity ) \
    NRD_CONSTANT( float, gFramerateScale ) \
    NRD_CONSTANT( float, gMinBlurRadius ) \
    NRD_CONSTANT( float, gMaxBlurRadius ) \
    NRD_CONSTANT( float, gDiffPrepassBlurRadius ) \
    NRD_CONSTANT( float, gSpecPrepassBlurRadius ) \
    NRD_CONSTANT( float, gMaxAccumulatedFrameNum ) \
    NRD_CONSTANT( float, gMaxFastAccumulatedFrameNum ) \
    NRD_CONSTANT( float, gAntiFirefly ) \
    NRD_CONSTANT( float, gLobeAngleFraction ) \
    NRD_CONSTANT( float, gRoughnessFraction ) \
    NRD_CONSTANT( float, gResponsiveAccumulationRoughnessThreshold ) \
    NRD_CONSTANT( float, gHistoryFixFrameNum ) \
    NRD_CONSTANT( float, gHistoryFixBasePixelStride ) \
    NRD_CONSTANT( float, gMinRectDimMulUnproject ) \
    NRD_CONSTANT( float, gUsePrepassNotOnlyForSpecularMotionEstimation ) \
    NRD_CONSTANT( float, gSplitScreen ) \
    NRD_CONSTANT( float, gSplitScreenPrev ) \
    NRD_CONSTANT( float, gCheckerboardResolveAccumSpeed ) \
    NRD_CONSTANT( float, gViewZScale ) \
    NRD_CONSTANT( float, gFireflySuppressorMinRelativeScale ) \
    NRD_CONSTANT( float, gMinHitDistanceWeight ) \
    NRD_CONSTANT( float, gDiffMinMaterial ) \
    NRD_CONSTANT( float, gSpecMinMaterial ) \
    NRD_CONSTANT( uint, gHasHistoryConfidence ) \
    NRD_CONSTANT( uint, gHasDisocclusionThresholdMix ) \
    NRD_CONSTANT( uint, gDiffCheckerboard ) \
    NRD_CONSTANT( uint, gSpecCheckerboard ) \
    NRD_CONSTANT( uint, gFrameIndex ) \
    NRD_CONSTANT( uint, gIsRectChanged ) \
    NRD_CONSTANT( uint, gResetHistory )

