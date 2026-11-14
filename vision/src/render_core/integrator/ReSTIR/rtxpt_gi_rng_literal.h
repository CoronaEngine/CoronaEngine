// Generated from the matched native RTXPT snapshot; original notices retained.
#pragma once
#include <string>
#include <string_view>
namespace vision::rtxpt_gi::native_rng {
inline constexpr std::string_view SourceParts[] = {
R"CRTXPTRNG(// GI-only bridge to matched native RTXPT stateless sample generators.
#ifndef CORONA_RTXPT_GI_RNG_LITERAL
#define CORONA_RTXPT_GI_RNG_LITERAL
#ifdef __SLANG__
#define CRTXPT_GI_RNG_MUTATING [mutating]
#else
#define CRTXPT_GI_RNG_MUTATING
#endif
namespace crtxpt_gi_rng_native {
/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef CORONA_RTXPT_GI_RNG_NATIVE_NOISE // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define CORONA_RTXPT_GI_RNG_NATIVE_NOISE


// must be power of 2! when using precomputed sobol, integration quality slowly deteriorates when sampling more than precomputed number, such that at 64 times the precomputed size it can drop below using pure pseudorandom
#define CRTXPT_GI_RNG_SOBOL_MAX_DIMENSIONS                                        5
#define CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_INDEX_COUNT                               65536
// don't forget to initialize storage, i.e.
// StructuredBuffer<uint> g_precomputedSobol : register( t42 );
// #define CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_BUFFER g_precomputedSobol


// *************************************************************************************************************************************
// Some of the noise and LD sampling functions in this file originate from:
// https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/Shaders/vaNoise.hlsl, 
// Original license provided below:
// *************************************************************************************************************************************
// MIT License
// 
// Copyright (C) 2016-2021, Intel Corporation 
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// *************************************************************************************************************************************
// Quasi-random sequence based on "Practical Hash-based Owen Scrambling", Brent Burley 2020, https://jcgt.org/published/0009/04/01/paper.pdf
// with shader implementation borrowing from Andrew Helmer's Shadertoy implementation (https://www.reddit.com/r/GraphicsProgramming/comments/l1go2r/owenscrambled_sobol_02_sequences_shadertoy/)
// *************************************************************************************************************************************


// *************************************************************************************************************************************
// Note: this will turn 0 into 0! if that's a problem do Hash32( x+constant ) - HashCombine does something similar already
inline uint Hash32( uint x )
{
// This little gem is from https://nullprogram.com/blog/2018/07/31/, "Prospecting for Hash Functions" by Chris Wellons
// There's also the inverse for the lowbias32, and a 64bit version.
// exact bias: 0.17353355999581582
// uint32_t lowbias32(uint32_t x)
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
//
inline uint Hash32Better( uint x )
{
// This little gem is from https://nullprogram.com/blog/2018/07/31/, "Prospecting for Hash Functions" by Chris Wellons
// slower, lower bias
// exact bias: 0.020888578919738908
// uint32_t triple32(uint32_t x)
    x ^= x >> 17;
    x *= uint(0xed5ad4bb);
    x ^= x >> 11;
    x *= uint(0xac4c1b51);
    x ^= x >> 15;
    x *= uint(0x31848bab);
    x ^= x >> 14;
    return x;
}
// popular hash_combine (boost, etc.)
inline uint Hash32Combine( const uint seed, const uint value )
{
     return seed ^ (Hash32(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));       
}
// same as Hash32Combine, just without re-hashing of input value
inline uint Hash32CombineSimple( const uint seed, const uint value )
{
    return seed ^ (value + (seed << 6) + (seed >> 2));
}
// converting the whole 32bit uint to [0, 1)
inline float Hash32ToFloat(uint hash)
{ 
    // a.) converting the upper 24bits to [0, 1) because the higher bits have better distribution in some hash algorithms (like sobol)
    // b.) this is a good way to guarantee [0, 1) since float32 mantissa is only 23 bits
    return (hash>>8) / float(1 << 24); // same as '/ 16777216.0'
}
inline float2 Hash32ToFloat(uint2 hash)
{
    return float2( Hash32ToFloat(hash.x), Hash32ToFloat(hash.y) );
}
inline float3 Hash32ToFloat(uint3 hash)
{
    return float3( Hash32ToFloat(hash.x), Hash32ToFloat(hash.y), Hash32ToFloat(hash.z) );
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if !defined(__cplusplus)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Quasi-random sequence - R*
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// http://extremelearning.com.au/unreasonable-effectiveness-of-quasirandom-sequences/
// https://www.shadertoy.com/view/4lVcRm, https://www.shadertoy.com/view/MtVBRw, 
// https://www.shadertoy.com/view/3ltSzS
float R1seq(int index, float offset)
{
    return frac(offset + float(index) * 0.618033988749894848204586834365641218413556121186522017520);
}
float2 R2seq(int index, float offset)
{
    return frac(offset.xx + float2(index.xx) * float2(0.754877666246692760049508896358532874940835564978799543103, 0.569840290998053265911399958119574964216147658520394151385));
}
float3 R3seq(int index, float offset)
{
    const float g = 1.22074408460575947536;
    float a1 = 1.0f / g, a2 = 1.0f / (g * g), a3 = 1.0f / (g * g * g);
    return float3(float(fmod((offset + a1 * float(index)), 1.0f)), float(fmod((offset + a2 * float(index)), 1.0)), float(fmod((offset + a3 * float(index)), 1.0)));
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Quasi-random sequence - Hash-based Owen Scrambled Sobol'
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Based on "Practical Hash-based Owen Scrambling", Brent Burley, Walt Disney Animation Studios
// With simplifications/optimizations taken out from https://www.shadertoy.com/view/wlyyDm# (relevant reddit thread:
// https://www.reddit.com/r/GraphicsProgramming/comments/l1go2r/owenscrambled_sobol_02_sequences_shadertoy/)
// This simplification uses Laine-Kerras permutation for the 1st dimension and Sobol' only for second and subsequent
// dimensions (good performance + multi-dimensional stratification).
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
uint bhos_sobol(uint index, uniform uint dimension)
{
#if 1 && defined(CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_BUFFER)
    return CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_BUFFER[ (index % CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_INDEX_COUNT) + CRTXPT_GI_RNG_SOBOL_PRECOMPUTED_INDEX_COUNT * dimension ];
#else
    const uint directions[CRTXPT_GI_RNG_SOBOL_MAX_DIMENSIONS][32] = {
        0x80000000, 0x40000000, 0x20000000, 0x10000000,
        0x08000000, 0x04000000, 0x02000000, 0x01000000,
        0x00800000, 0x00400000, 0x00200000, 0x00100000,
        0x00080000, 0x00040000, 0x00020000, 0x00010000,
        0x00008000, 0x00004000, 0x00002000, 0x00001000,
        0x00000800, 0x00000400, 0x00000200, 0x00000100,
        0x00000080, 0x00000040, 0x00000020, 0x00000010,
        0x00000008, 0x00000004, 0x00000002, 0x00000001,

        0x80000000, 0xc0000000, 0xa0000000, 0xf0000000,
        0x88000000, 0xcc000000, 0xaa000000, 0xff000000,
        0x80800000, 0xc0c00000, 0xa0a00000, 0xf0f00000,
        0x88880000, 0xcccc0000, 0xaaaa0000, 0xffff0000,
        0x80008000, 0xc000c000, 0xa000a000, 0xf000f000,
        0x88008800, 0xcc00cc00, 0xaa00aa00, 0xff00ff00,
        0x80808080, 0xc0c0c0c0, 0xa0a0a0a0, 0xf0f0f0f0,
        0x88888888, 0xcccccccc, 0xaaaaaaaa, 0xffffffff,

        0x80000000, 0xc0000000, 0x60000000, 0x90000000,
        0xe8000000, 0x5c000000, 0x8e000000, 0xc5000000,
        0x68800000, 0x9cc00000, 0xee600000, 0x55900000,
        0x80680000, 0xc09c0000, 0x60ee0000, 0x90550000,
        0xe8808000, 0x5cc0c000, 0x8e606000, 0xc5909000,
        0x6868e800, 0x9c9c5c00, 0xeeee8e00, 0x5555c500,
        0x8000e880, 0xc0005cc0, 0x60008e60, 0x9000c590,
        0xe8006868, 0x5c009c9c, 0x8e00eeee, 0xc5005555,

        0x80000000, 0xc0000000, 0x20000000, 0x50000000,
        0xf8000000, 0x74000000, 0xa2000000, 0x93000000,
        0xd8800000, 0x25400000, 0x59e00000, 0xe6d00000,
        0x78080000, 0xb40c0000, 0x82020000, 0xc3050000,
        0x208f8000, 0x51474000, 0xfbea2000, 0x75d93000,
        0xa0858800, 0x914e5400, 0xdbe79e00, 0x25db6d00,
        0x58800080, 0xe54000c0, 0x79e00020, 0xb6d00050,
        0x800800f8, 0xc00c0074, 0x200200a2, 0x50050093,

        0x80000000, 0x40000000, 0x20000000, 0xb0000000,
        0xf8000000, 0xdc000000, 0x7a000000, 0x9d000000,
        0x5a800000, 0x2fc00000, 0xa1600000, 0xf0b00000,
        0xda880000, 0x6fc40000, 0x81620000, 0x40bb0000,
        0x22878000, 0xb3c9c000, 0xfb65a000, 0xddb2d000,
        0x78022800, 0x9c0b3c00, 0x5a0fb600, 0x2d0ddb00,
        0xa2878080, 0xf3c9c040, 0xdb65a020, 0x6db2d0b0,
        0x800228f8, 0x400b3cdc, 0x200fb67a, 0xb00ddb9d,
    };

    uint X = 0u;
    [unroll]
    for (uint bit = 0; bit < 32; bit++) {
        uint mask = (index >> bit) & 1u;
        X ^= mask * directions[dimension][bit];
    }
    return X;
#endif
}
//
uint bhos_reverse_bits(uint x) 
{
#if 1
    return reversebits(x);  // hey we've got this in HLSL! awesome.
#else
    x = (((x & 0xaaaaaaaau) >> 1) | ((x & 0x55555555u) << 1));
    x = (((x & 0xccccccccu) >> 2) | ((x & 0x33333333u) << 2));
    x = (((x & 0xf0f0f0f0u) >> 4) | ((x & 0x0f0f0f0fu) << 4));
    x = (((x & 0xff00ff00u) >> 8) | ((x & 0x00ff00ffu) << 8));
    return ((x >> 16) | (x << 16));
#endif
}
//
uint bhos_owen_hash(uint x, uint seed) 
{
#if 0 // this is the original laine_karras_permutation from Burley2020
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
#else // this is from https://psychopath.io/post/2021_01_30_building_a_better_lk_hash (updated - thanks to https://github.com/CptLucky8 and https://pharr.org/matt/, see https://github.com/GameTechDev/XeGTAO/issues/7)
    x ^= x * 0x3d20adea;
    x += seed;
    x *= (seed >> 16) | 1;
    x ^= x * 0x05526c56;
    x ^= x * 0x53a22864;
#endif
    return x;
}
//
uint bhos_owen_scramble( uint x, uint seed ) )CRTXPTRNG",
R"CRTXPTRNG(// nested_uniform_scramble_base2
{
    x = bhos_reverse_bits(x);
    x = bhos_owen_hash(x, seed);
    x = bhos_reverse_bits(x);
    return x;
}
#endif // !defined(__cplusplus)
#endif // native noise guard
enum class SampleGeneratorEffectSeed : uint32_t {
    Base=0, ScatterBSDF=1, NextEventEstimation=2, NextEventEstimationLightSampler=3,
    NextEventEstimationFeedback=5, RussianRoulette=6
};
/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef CORONA_RTXPT_GI_RNG_NATIVE_STATELESS // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define CORONA_RTXPT_GI_RNG_NATIVE_STATELESS


// Each path tracing vertex initializes this once and spawns sample sequences using it
struct SampleGeneratorVertexBase
{
    uint    m_baseHash;
    uint    m_sampleIndex;
#ifdef HQ_UNIFORM_SAMPLE_SEQUENCE_GENERATOR_ENABLED
    uint2   m_pixelCoord;   
    uint    m_vertexIndex;  
#endif

    static SampleGeneratorVertexBase    make(uint2 pixelCoord, uint vertexIndex, uint sampleIndex)
    {
        SampleGeneratorVertexBase ret;
        ret.m_sampleIndex   = sampleIndex;
        ret.m_baseHash      = Hash32Combine(Hash32(vertexIndex + 0x035F9F29), (pixelCoord.x << 16) | pixelCoord.y);
#ifdef HQ_UNIFORM_SAMPLE_SEQUENCE_GENERATOR_ENABLED
        ret.m_pixelCoord    = pixelCoord;  
        ret.m_vertexIndex   = vertexIndex;
#endif
        return ret;
    }

    static SampleGeneratorVertexBase    make(uint baseHash, uint sampleIndex)
    {
        SampleGeneratorVertexBase ret;
        ret.m_sampleIndex = sampleIndex;
        ret.m_baseHash = Hash32(baseHash+0x035F9F29);
#ifdef HQ_UNIFORM_SAMPLE_SEQUENCE_GENERATOR_ENABLED
        ret.m_pixelCoord    = uint2(0,0);  
        ret.m_vertexIndex   = ret.m_baseHash;
#endif
        return ret;
    }
};

/** Inline quasi-random sample generator.

    Current implementation is based on "Practical Hash-based Owen Scrambling", Brent Burley 2020, https://jcgt.org/published/0009/04/01/paper.pdf
    Shader implementation borrows from Andrew Helmer's Shadertoy implementation (https://www.reddit.com/r/GraphicsProgramming/comments/l1go2r/owenscrambled_sobol_02_sequences_shadertoy/)

    It supports up to >6< dimensions and reverts back to pseudo-random for subsequent samples.

*/
struct SampleSequenceGenerator
{
    static const uint cLDDisabled                       = 0xFFFFFFFE;
    static const uint cLDDisabled_RanOutOfDimensions    = 0xFFFFFFFF;

    uint m_startingHash;
    uint m_currentHash;

    uint m_sampleIndex;
    uint m_dimension;   // cLDDisabled and cLDDisabled_RanOutOfDimensions mean it's in non-LD mode
    uint m_activeIndex; // in case subIndexing is used

    // Creates an effect-specific sequence sampler state that is decorrelated from other effects.
    // The state is also deterministic for the specific path vertex, regardless of shader code ordering / branching.
    // This either activates the non-low discrepancy path (faster) or a low-discrepancy (slower) sampler where
    // each subsequent 'Next()' call advances dimension and provide a low discrepancy sample for the index.
    // Once all available dimensions provided by the LD sampler are used up (4, 5, 6, etc.), it reverts to the faster non-low discrepancy path.
    //
    // Subindex-ing feature allows sampling multiple sequence samples in case of inner loops (i.e. sampling multiple lights in a loop per bounce).
    static SampleSequenceGenerator      make(const SampleGeneratorVertexBase base, const SampleGeneratorEffectSeed effectSeed = SampleGeneratorEffectSeed::Base, bool lowDiscrepancy = false, int subSampleCount = 1)
    {
        SampleSequenceGenerator ret;
        ret.m_sampleIndex = base.m_sampleIndex;
        ret.m_activeIndex = ret.m_sampleIndex * subSampleCount;
        ret.m_currentHash = Hash32Combine(base.m_baseHash, (uint)effectSeed);
        ret.m_startingHash = ret.m_currentHash;
        if (lowDiscrepancy)
        {
            ret.m_dimension = 0;    // set LD mode
        }
        else
        {
            ret.m_currentHash = Hash32Combine(ret.m_currentHash, ret.m_activeIndex); // unlike the StatelessUniformSampleGenerator, we combine the sampleIndex here in uniform mode; a bit slower but allows for LD mode that needs it not combined into m_baseHash
            ret.m_dimension = cLDDisabled; // set non-LD mode
        }
        return ret;
    }

    // Returns the next sample value. This function updates the state by advancing Dimension if possible, otherwise it just advances the uniform random sequence. 
    CRTXPT_GI_RNG_MUTATING uint Next()
    {
        const uint maxSupportedDimensionIndex = 5;
        
        if (m_dimension >= cLDDisabled)
        {
            m_currentHash = Hash32(m_currentHash);
            return m_currentHash;
        }
        else
        {
            uint shuffle_seed = Hash32Combine( m_currentHash, 0 );   // this might not need combine, just use raw and then remove '1+' from '1+m_dimension' below
            uint dim_seed = Hash32Combine( m_currentHash, 1+m_dimension );
            uint shuffled_index = bhos_owen_scramble(m_activeIndex, shuffle_seed);
            #if 1
            // Sobol' sequence is expensive but we can use Laine-Kerras permutation for the 1st dimension and Sobol' only for second and subsequent. See 'bhos_sobol' function comments for more detail.
            uint dim_sample;
            [branch] if (m_dimension==0) 
                dim_sample = bhos_reverse_bits(shuffled_index);
            else
                dim_sample = bhos_sobol(shuffled_index, m_dimension);
            #else
            uint dim_sample = bhos_sobol(shuffled_index, m_dimension);
            #endif
            dim_sample = bhos_owen_scramble(dim_sample, dim_seed);

            // step dimension for next sample!
            m_dimension++;
            
            // for all subsequent samples fall back to pseudo-random
            if(m_dimension >= maxSupportedDimensionIndex)
            {
                m_currentHash = Hash32Combine(m_currentHash, m_activeIndex);
                m_dimension = cLDDisabled_RanOutOfDimensions;    // set non-LD mode
            }
            return dim_sample;
        }
    }

    // Start next sample, reset the dimension to 0
    CRTXPT_GI_RNG_MUTATING void AdvanceSampleIndex()
    {
        if (m_dimension != cLDDisabled)  // if not uniform
        {
            m_dimension = 0;
            m_currentHash = m_startingHash;
            m_activeIndex++;
        }
    }

    // Note - this is limited to generating up to 4 values
    static float4 Generate(uint count, const SampleGeneratorVertexBase base, const SampleGeneratorEffectSeed effectSeed, const int subSampleIndex = 0, const int subSampleCount = 1)
    {
        count = min( count, 4 );
        float4 retVal;

        SampleSequenceGenerator context;
        context.m_sampleIndex = base.m_sampleIndex;
        context.m_activeIndex = context.m_sampleIndex * subSampleCount + subSampleIndex;
        context.m_currentHash = Hash32Combine(base.m_baseHash, (uint)effectSeed);
        context.m_startingHash = context.m_currentHash;
        
        [unroll]for( context.m_dimension = 0; context.m_dimension < count; context.m_dimension++ )
        {
            uint shuffle_seed = Hash32Combine( context.m_currentHash, 0 );   // this might not need combine, just use raw and then remove '1+' from '1+m_dimension' below
            uint dim_seed = Hash32Combine( context.m_currentHash, 1+context.m_dimension );
            uint shuffled_index = bhos_owen_scramble(context.m_activeIndex, shuffle_seed);
            
            uint dim_sample;
            // Sobol' sequence is expensive but we can use Laine-Kerras permutation for the 1st dimension and Sobol' only for second and subsequent. See 'bhos_sobol' function comments for more detail.
            if (context.m_dimension==0) 
                dim_sample = bhos_reverse_bits(shuffled_index);
            else
                dim_sample = bhos_sobol(shuffled_index, context.m_dimension);
            dim_sample = bhos_owen_scramble(dim_sample, dim_seed);
            retVal[context.m_dimension] = Hash32ToFloat(dim_sample);
        }
        return retVal;
    }

};

/** Inline uniform random sample generator.

    This generator has only 32 bit state and sub-optimal statistical properties, however it's 'mostly fine' for up to millions of samples.
    TODO: try using LCG
    TODO: read & try out: https://pharr.org/matt/blog/2022/03/05/sampling-fp-unit-interval
*/
struct UniformSampleSequenceGenerator
{
    uint m_currentHash;

    static UniformSampleSequenceGenerator make(const SampleGeneratorVertexBase base, const SampleGeneratorEffectSeed effectSeed = SampleGeneratorEffectSeed::Base, bool lowDiscrepancy = false, int subSampleCount = 1)
    {
        UniformSampleSequenceGenerator ret;

        uint activeIndex = base.m_sampleIndex * subSampleCount;
        ret.m_currentHash = Hash32Combine(base.m_baseHash, (uint)effectSeed);
        ret.m_currentHash = Hash32Combine(ret.m_currentHash, activeIndex); // haven't seen any instances where it's necessary
        return ret;
    }

    // Returns the next sample value. This function updates the state.
    CRTXPT_GI_RNG_MUTATING uint Next()
    {
        m_currentHash = Hash32(m_currentHash);
        return m_currentHash;
    }

    CRTXPT_GI_RNG_MUTATING void AdvanceSampleIndex()
    {
        // nothing to do here
    }

    // Note - this is limited to generating up to 4 values
    static float4 Generate(uint count, const SampleGeneratorVertexBase base, const SampleGeneratorEffectSeed effectSeed, const int subSampleIndex = 0, const int subSampleCount = 1)
    {
        count = min( count, 4 );
        float4 retVal;

        UniformSampleSequenceGenerator context;
        uint activeIndex = base.m_sampleIndex * subSampleCount + subSampleIndex;
        context.m_currentHash = Hash32Combine(base.m_baseHash, (uint)effectSeed);
        context.m_currentHash = Hash32Combine(context.m_currentHash, activeIndex); // haven't seen any instances where it's necessary
        
        [unroll]for( uint counter = 0; counter < count; counter++ )
        {
            context.m_currentHash = Hash32(context.m_currentHash);
            retVal[counter] = Hash32ToFloat(context.m_currentHash);
        }
        return retVal;
    }

};



#endif // native stateless guard

} // namespace crtxpt_gi_rng_native
float3 crtxpt_gi_bsdf_random(uint2 pixel, uint vertex, uint frame, uint diffuseBounces) {
    using namespace crtxpt_gi_rng_native;
    SampleGeneratorVertexBase base = SampleGeneratorVertexBase::make(pixel, vertex, frame);
    if(diffuseBounces < 1)
        return SampleSequenceGenerator::Generate(3, base, SampleGeneratorEffectSeed::ScatterBSDF).xyz;
    return UniformSampleSequenceGenerator::Generate(3, base, SampleGeneratorEffectSeed::ScatterBSDF).xyz;
}
float4 crtxpt_gi_nee_random(uint2 pixel, uint vertex, uint frame) {
    using namespace crtxpt_gi_rng_native;
    SampleGeneratorVertexBase base = SampleGeneratorVertexBase::make(pixel, vertex, frame);
    float3 light = UniformSampleSequenceGenerator::Generate(3, base, SampleGeneratorEffectSeed::NextEventEstimationLightSampler).xyz;
    float wrs = UniformSampleSequenceGenerator::Generate(1, base, SampleGeneratorEffectSeed::NextEventEstimation).x;
    return float4(light,wrs);
}
float crtxpt_gi_roulette_random(uint2 pixel, uint vertex, uint frame) {
    using namespace crtxpt_gi_rng_native;
    SampleGeneratorVertexBase base = SampleGeneratorVertexBase::make(pixel, vertex, frame);
    SampleSequenceGen)CRTXPTRNG",
R"CRTXPTRNG(erator rng = SampleSequenceGenerator::make(base, SampleGeneratorEffectSeed::RussianRoulette, false);
    uint bits = rng.Next();
    return (bits>>8) / float(1 << 24);
}

#undef CRTXPT_GI_RNG_MUTATING
#endif
)CRTXPTRNG"
};
inline const std::string &literal_source() {
    static const std::string source = [] {
        std::string value;
        for (const auto part : SourceParts) value.append(part);
        return value;
    }();
    return source;
}
}
