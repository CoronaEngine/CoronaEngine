#pragma once
#include "CpuReblurConfig.h"

// Only C++ constant records and group sizes; resource binding graphs remain in the CPU denoiser.

NRD_CONSTANTS_START( Clear_FloatConstants )
    NRD_CONSTANT( float, gDebug ) // only for availability in Common.hlsl
    NRD_CONSTANT( float, gViewZScale ) // only for availability in Common.hlsl
NRD_CONSTANTS_END
#define Clear_FloatGroupX 16
#define Clear_FloatGroupY 16

NRD_CONSTANTS_START( Clear_UintConstants )
    NRD_CONSTANT( float, gDebug ) // only for availability in Common.hlsl
    NRD_CONSTANT( float, gViewZScale ) // only for availability in Common.hlsl
NRD_CONSTANTS_END
#define Clear_UintGroupX 16
#define Clear_UintGroupY 16

NRD_CONSTANTS_START( REBLUR_BlurConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_BlurGroupX 8
#define REBLUR_BlurGroupY 16

NRD_CONSTANTS_START( REBLUR_ClassifyTilesConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_ClassifyTilesGroupX 16
#define REBLUR_ClassifyTilesGroupY 16

NRD_CONSTANTS_START( REBLUR_HistoryFixConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_HistoryFixGroupX 8
#define REBLUR_HistoryFixGroupY 16

NRD_CONSTANTS_START( REBLUR_HitDistReconstructionConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_HitDistReconstructionGroupX 8
#define REBLUR_HitDistReconstructionGroupY 16

NRD_CONSTANTS_START( REBLUR_PostBlurConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_PostBlurGroupX 8
#define REBLUR_PostBlurGroupY 16

NRD_CONSTANTS_START( REBLUR_PrePassConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_PrePassGroupX 8
#define REBLUR_PrePassGroupY 16

NRD_CONSTANTS_START( REBLUR_SplitScreenConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_SplitScreenGroupX 8
#define REBLUR_SplitScreenGroupY 16

NRD_CONSTANTS_START( REBLUR_TemporalAccumulationConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_TemporalAccumulationGroupX 8
#define REBLUR_TemporalAccumulationGroupY 16

NRD_CONSTANTS_START( REBLUR_TemporalStabilizationConstants )
    REBLUR_SHARED_CONSTANTS
NRD_CONSTANTS_END
#define REBLUR_TemporalStabilizationGroupX 8
#define REBLUR_TemporalStabilizationGroupY 16

NRD_CONSTANTS_START( REBLUR_ValidationConstants )
    REBLUR_SHARED_CONSTANTS
    NRD_CONSTANT( uint, gHasDiffuse )
    NRD_CONSTANT( uint, gHasSpecular )
NRD_CONSTANTS_END
#define REBLUR_ValidationGroupX 8
#define REBLUR_ValidationGroupY 16

static_assert(sizeof(REBLUR_TemporalAccumulationConstants) == 832,
    "REBLUR 4.14.3 CPU constants must match the EDSL and independent oracle layout");
