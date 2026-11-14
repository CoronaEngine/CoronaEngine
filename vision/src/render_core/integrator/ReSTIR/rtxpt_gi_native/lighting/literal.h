#pragma once
#include <string>
#include <string_view>
namespace vision::rtxpt_gi::native_lighting {
inline constexpr std::string_view LiteralSourceParts[] = {
R"RTXPTLIGHT(
#ifndef CORONA_RTXPT_GI_NATIVE_LIGHTING_LITERAL
#define CORONA_RTXPT_GI_NATIVE_LIGHTING_LITERAL
/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/


namespace crtxpt_gi_lighting_native {
static const float K_PI=3.14159265358979323846;
static const float K_2_PI=0.63661977236758134308;
static const float K_PI_4=0.78539816339744830962;
float sq(float v){return v*v;}

float Luminance(float3 c){return dot(c,float3(.2126,.7152,.0722));}

static const uint kPolymorphicLightTypeShift = 24;
static const uint kPolymorphicLightTypeMask = 0xf;
static const uint kPolymorphicLightShapingEnableBit = 1 << 28;
static const uint kPolymorphicLightIesProfileEnableBit = 1 << 29;
static const float kPolymorphicLightMinLog2Radiance = -8.f;
static const float kPolymorphicLightMaxLog2Radiance = 40.f;


enum PolymorphicLightType { kSphere=0,kTriangle=1,kDirectional=2,kEnvironment=3,kPoint=4,kEnvironmentQuad=5 };
struct PolymorphicLightInfo { float3 Center; uint ColorTypeAndFlags; uint Direction1; uint Direction2; uint Scalars; uint LogRadiance; };
struct PolymorphicLightInfoEx { uint IesProfileIndex, PrimaryAxis,CosConeAngleAndSoftness,UniqueID; static PolymorphicLightInfoEx empty(){return (PolymorphicLightInfoEx)0;} };
struct PolymorphicLightInfoFull { PolymorphicLightInfo Base; PolymorphicLightInfoEx Extended; };
struct PolymorphicLightSample {float3 Position,Normal,Radiance;float SolidAnglePdf;bool LightSampleableByBSDF;};

uint Pack_R8_UFLOAT(float r,float d=.5f){const uint mask=(1U<<8)-1U;return (uint)floor(r*mask+d)&mask;}
float Unpack_R8_UFLOAT(uint r){const uint mask=(1U<<8)-1U;return (float)(r&mask)/(float)mask;}

uint Pack_R8G8B8_UFLOAT(float3 rgb, float3 d = float3(0.5f, 0.5f, 0.5f))
{
    uint r = Pack_R8_UFLOAT(rgb.r, d.r);
    uint g = Pack_R8_UFLOAT(rgb.g, d.g) << 8;
    uint b = Pack_R8_UFLOAT(rgb.b, d.b) << 16;
    return r | g | b;
}
float3 Unpack_R8G8B8_UFLOAT(uint rgb)
{
    float r = Unpack_R8_UFLOAT(rgb);
    float g = Unpack_R8_UFLOAT(rgb >> 8);
    float b = Unpack_R8_UFLOAT(rgb >> 16);
    return float3(r, g, b);
}



struct PolymorphicLight {
static float UnpackRadiance(uint logRadiance)
{
    return (logRadiance == 0) ? 0 : exp2((float(logRadiance - 1) / 65534.0) * (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance) + kPolymorphicLightMinLog2Radiance);
}
static float3 UnpackColor(PolymorphicLightInfo lightInfoBase)
{
    float3 color = Unpack_R8G8B8_UFLOAT(lightInfoBase.ColorTypeAndFlags);
    float radiance = UnpackRadiance(lightInfoBase.LogRadiance & 0xffff);
    return color * radiance.xxx;
}
static void PackColor(float3 radiance, inout PolymorphicLightInfo lightInfoBase)
{   
    float intensity = max(radiance.r, max(radiance.g, radiance.b));

    if (intensity > 0.0)
    {
        float logRadiance = saturate((log2(intensity) - kPolymorphicLightMinLog2Radiance) 
            / (kPolymorphicLightMaxLog2Radiance - kPolymorphicLightMinLog2Radiance));
        uint packedRadiance = min(uint32_t(ceil(logRadiance * 65534.0)) + 1, 0xffffu);
        float unpackedRadiance = UnpackRadiance(packedRadiance);

        float3 normalizedRadiance = saturate(radiance.rgb / unpackedRadiance.xxx);

        lightInfoBase.LogRadiance |= packedRadiance;
        lightInfoBase.ColorTypeAndFlags |= Pack_R8G8B8_UFLOAT(normalizedRadiance);
    }
}
};

float2 ndir_to_oct_equal_area_unorm(float3 n)
{
    // Use atan2 to avoid explicit div-by-zero check in atan(y/x).
    float r = sqrt(1.f - abs(n.z));
    float phi = atan2(abs(n.y), abs(n.x));

    // Compute p = (u,v) in the first quadrant.
    float2 p;
    p.y = r * phi * K_2_PI;
    p.x = r - p.y;

    // Reflect p over the diagonals, and move to the correct quadrant.
    if (n.z < 0.f) p = 1.f - p.yx;
    p *= sign(n.xy);

    return saturate(p * 0.5f + 0.5f);
}
float3 oct_to_ndir_equal_area_unorm(float2 p)
{
    p = p * 2.f - 1.f;

    // Compute radius r without branching. The radius r=0 at +z (center) and at -z (corners).
    float d = 1.f - (abs(p.x) + abs(p.y));
    float r = 1.f - abs(d);

    // Compute phi in [0,pi/2] (first quadrant) and sin/cos without branching.
    // TODO: Analyze fp32 precision, do we need a small epsilon instead of 0.0 here?
    float phi = (r > 0.f) ? ((abs(p.y) - abs(p.x)) / r + 1.f) * K_PI_4 : 0.f;

    // Convert to Cartesian coordinates. Note that sign(x)=0 for x=0, but that's fine here.
    float f = r * sqrt(2.f - r*r);
    float x = f * sign(p.x) * cos(phi);
    float y = f * sign(p.y) * sin(phi);
    float z = sign(d) * (1.f - r*r);

    return float3(x, y, z);
}
float3 SampleTriangleUniform(float2 rndSample)
{
    float sqrtx = sqrt(rndSample.x);

    return float3(
        1 - sqrtx,
        sqrtx * (1 - rndSample.y),
        sqrtx * rndSample.y);
}
float pdfAtoW(float pdfA, float distance_, float cosTheta)
{
    return pdfA * sq(distance_) / max(cosTheta, 2e-9f); //< TODO: take this epsilon out as a global const
}
float3 ComputeRayOrigin(float3 worldPosition, float3 faceNormal)  // expects triangle faceNormal pointing towards the intended ray direction
{
    const float origin = 1.f / 16.f;
    const float fScale = 3.f / 65536.f;
    const float iScale = 3 * 256.f;

    // Per-component integer offset to bit representation of fp32 position.
    int3 iOff = int3(faceNormal * iScale);
    float3 iPos = asfloat(asint(worldPosition) + select(worldPosition < 0.f, -iOff, iOff));

    // Select per-component between small fixed offset or above variable offset depending on distance to origin.
    float3 fOff = faceNormal * fScale;
    return select(abs(worldPosition) < origin, worldPosition + fOff, iPos);
}
float3 CubemapGetDirectionFor(uint face, float2 uv)
{
    // map [0, dim] to [-1,1] with (-1,-1) at bottom left
    float cx = (uv.x * 2.0) - 1;
    float cy = 1 - (uv.y * 2.0);    // <- not entirely sure about this bit

    float3 dir;
    const float l = sqrt(cx * cx + cy * cy + 1);
    switch (face) 
    {
        case 0:  dir = float3(   1, cy, -cx ); break;  // PX
        case 1:  dir = float3(  -1, cy,  cx ); break;  // NX
        case 2:  dir = float3(  cx,  1, -cy ); break;  // PY
        case 3:  dir = float3(  cx, -1,  cy ); break;  // NY
        case 4:  dir = float3(  cx, cy,   1 ); break;  // PZ
        case 5:  dir = float3( -cx, cy,  -1 ); break;  // NZ
        default: dir = 0.0.xxx; break;
    }
    return dir * (1 / l);
}
static inline float SphereQuadrantArea(float x, float y) 
{
    return atan2( x*y, sqrt( x*x + y*y + 1 ) );
}
float CubemapTexelSolidAngle(float cubeDim, uint2 coord) 
{
    const float iDim = 1.0f / cubeDim;
    float s = ((coord.x + 0.5f) * 2 * iDim) - 1;
    float t = ((coord.y + 0.5f) * 2 * iDim) - 1;
    const float x0 = s - iDim;
    const float y0 = t - iDim;
    const float x1 = s + iDim;
    const float y1 = t + iDim;
    float solidAngle =  SphereQuadrantArea( x0, y0 ) -
                        SphereQuadrantArea( x0, y1 ) -
                        SphereQuadrantArea( x1, y0 ) +
                        SphereQuadrantArea( x1, y1 );
    return max( 1e-6, abs(solidAngle) );
}
#define FLT_EPSILON_MINI (2e-9f)
#define MAX_SOLID_ANGLE_PDF (1e10f)

#define RTXPT_INVALID_LIGHT_INDEX 0xFFFFFFFFu

struct LightFeedbackReservoir
{
    uint        CandidateIndex;
    float       CandidateWeight;
    float       TotalWeight;

    static LightFeedbackReservoir make()
    {
        LightFeedbackReservoir ret;
        ret.CandidateIndex  = RTXPT_INVALID_LIGHT_INDEX;
        ret.CandidateWeight = 0;
        ret.TotalWeight     = 0;
        return ret;
    }
    
    static LightFeedbackReservoir Unpack12Byte( uint3 packed )
    {
        LightFeedbackReservoir ret;
        ret.CandidateIndex  = packed.x;
        ret.CandidateWeight = asfloat(packed.y);
        ret.TotalWeight     = asfloat(packed.z);
        return ret;
    }

    uint3 Pack12Byte()
    { 
        return uint3(CandidateIndex, asuint(CandidateWeight), asuint(TotalWeight));
    }

    static LightFeedbackReservoir Unpack8Byte(uint2 packed)
    {
        LightFeedbackReservoir ret;
        ret.CandidateIndex    = packed.x;
        ret.CandidateWeight   = f16tof32(packed.y & 0xFFFF);
        ret.TotalWeight    = f16tof32(packed.y >> 16);
        return ret;
    }

    uint2 Pack8Byte()
    {
        return uint2(CandidateIndex, f32tof16(CandidateWeight) | ( f32tof16(TotalWeight) << 16 ) ); 
    }

    [mutating] void Add( const float randomValue, const uint candidateIndex, const float candidateWeight )
    {
        TotalWeight += candidateWeight;
        float threshold = saturate(candidateWeight / TotalWeight);
        if (randomValue < threshold)
        {
            CandidateIndex = candidateIndex;
            CandidateWeight = candidateWeight;
        }
    }

    [mutating] void Merge( const float randomValue, const LightFeedbackReservoir other )
    {
        if( other.TotalWeight > 0 )
            Add( randomValue, other.CandidateIndex, other.TotalWeight );
    }

    void Retrieve( out uint candidateIndex, out float candidatePdf )
    {
        candidateIndex  = CandidateIndex;
        candidatePdf    = /*TotalCandidates **/ CandidateWeight / TotalWeight;
    }

    [mutating] void Scale(float factor)
    {
        CandidateWeight *= factor;
        TotalWeight *= factor;
    }

    //void Merge( )
};
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
inline uint Hash32Combine( const uint seed, const uint value )
{
     return seed ^ (Hash32(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));       
}
struct TriangleLight {float3 base,edge1,edge2,radiance,normal;float surfaceArea;

PolymorphicLightSample CalcSample(in const float2 random, in const float3 viewerPosition)
    {
        // consider https://www.graphics.cornell.edu/pubs/1995/Arv95c.pdf for ugprade - thanks to Johannes 

        PolymorphicLightSample result = (PolymorphicLightSample)0;

        float3 bary = SampleTriangleUniform(random);
        result.Position = base + edge1 * bary.y + edge2 * bary.z;
        result.Position = ComputeRayOrigin(result.Position, normal);
        result.Normal = normal;

        const float3 toLight = result.Position - viewerPosition;
        const float distSqr = max(FLT_EPSILON_MINI, dot(toLight, toLight));
        const float distance = sqrt(distSqr);
        const float3 dir = toLight / distance;
        float cosTheta = dot(normal, -dir);
        
        result.SolidAnglePdf = 0.f;
        result.Radiance = 0.f;
        if (cosTheta <= 0.f) return result;

        const float areaPdf = max(FLT_EPSILON_MINI, 1.0 / surfaceArea);
        result.SolidAnglePdf = min( MAX_SOLID_ANGLE_PDF, pdfAtoW(areaPdf, distance, cosTheta) );
        result.Radiance = radiance;
        result.LightSampleableByBSDF = true;

        #if 0 && defined(DEBUG_PRINT_DEFINED)
        float solidAnglePdfTest = CalcSolidAnglePdfForMIS(viewerPosition, result.Position);
        if( !RelativelyEqual(result.SolidAnglePdf, solidAnglePdfTest ))
            DebugPrint( "ERROR: solidAngle {0} solidAngleTest {1}", result.SolidAnglePdf, solidAnglePdfTest );
        #endif

        return result;
    }
float CalcSolidAnglePdfForMIS(in const float3 viewerPosition, in const float3 lightSamplePosition)
    {
        const float3 toLight = lightSamplePosition - viewerPosition;
)RTXPTLIGHT",
R"RTXPTLIGHT(        const float distSqr = max(FLT_EPSILON_MINI, dot(toLight, toLight));
        const float distance = sqrt(distSqr);
        const float3 dir = toLight / distance;
        float cosTheta = dot(normal, -dir);

        const float areaPdf = max(FLT_EPSILON_MINI, 1.0 / surfaceArea);
        return min( MAX_SOLID_ANGLE_PDF, pdfAtoW(areaPdf, distance, cosTheta) );
    }
float GetPower()
    {
        return surfaceArea * K_PI * Luminance(radiance);
    }
static TriangleLight Create(in const PolymorphicLightInfoFull lightInfo)
    {
        TriangleLight triLight;

        triLight.edge1 = f16tof32(uint3(lightInfo.Base.Direction1, lightInfo.Base.Direction2, lightInfo.Base.Scalars) & 0xffff);
        triLight.edge2 = f16tof32(uint3(lightInfo.Base.Direction1, lightInfo.Base.Direction2, lightInfo.Base.Scalars) >> 16);
        triLight.base = lightInfo.Base.Center - ((triLight.edge1 + triLight.edge2) / 3.0);
        triLight.radiance = PolymorphicLight::UnpackColor(lightInfo.Base);

        float3 lightNormal = cross(triLight.edge1, triLight.edge2);
        float lightNormalLength = length(lightNormal);
        
        //Check for tiny triangles
        if(lightNormalLength > 0.0)
        {
            triLight.surfaceArea = 0.5 * lightNormalLength;
            triLight.normal = lightNormal / lightNormalLength;
        }
        else
        {
           triLight.surfaceArea = 0.0;
           triLight.normal = 0.0; 
        }

        return triLight;
    }
PolymorphicLightInfoFull Store(uint uniqueID)
    {
        PolymorphicLightInfoFull lightInfo = (PolymorphicLightInfoFull)0;

        PolymorphicLight::PackColor(radiance, lightInfo.Base);
        lightInfo.Base.Center = base + ((edge1 + edge2) / 3.0); 
        float3 edges = (f32tof16(edge1) & 0xffff) | (f32tof16(edge2) << 16);
        lightInfo.Base.Direction1 = edges.x;
        lightInfo.Base.Direction2 = edges.y;
        lightInfo.Base.Scalars = edges.z;
        lightInfo.Base.ColorTypeAndFlags |= uint(PolymorphicLightType::kTriangle) << kPolymorphicLightTypeShift;
        //lightInfo.Base.logRadiance |= f32tof16((uint) empty slot) << 16; //unused
        lightInfo.Extended = PolymorphicLightInfoEx::empty();
        lightInfo.Extended.UniqueID = uniqueID;
        return lightInfo;
    }
};
}


float3 crtxpt_light_oct_decode(float2 uv){return crtxpt_gi_lighting_native::oct_to_ndir_equal_area_unorm(uv);}
float2 crtxpt_light_oct_encode(float3 d){return crtxpt_gi_lighting_native::ndir_to_oct_equal_area_unorm(d);}
float3 crtxpt_light_cube_dir(uint face,float2 uv){return crtxpt_gi_lighting_native::CubemapGetDirectionFor(face,uv);}
float crtxpt_light_cube_area(float dim,uint2 xy){return crtxpt_gi_lighting_native::CubemapTexelSolidAngle(dim,xy);}
uint2 crtxpt_light_half_pack(float4 v){uint4 h=f32tof16(v);return uint2(h.x|(h.y<<16),h.z|(h.w<<16));}
float4 crtxpt_light_half_unpack(uint2 h){return f16tof32(uint4(h.x&65535,h.x>>16,h.y&65535,h.y>>16));}
float4 crtxpt_light_half_round(float4 v){return f16tof32(f32tof16(v));}
float3 crtxpt_light_cube_coord(float3 d){
 float3 a=abs(d);uint f;float2 q;
 if(a.x>=a.y&&a.x>=a.z){f=d.x>=0?0:1;q=float2(d.x>=0?-d.z:d.z,d.y)/a.x;}
 else if(a.y>=a.z){f=d.y>=0?2:3;q=float2(d.x,d.y>=0?-d.z:d.z)/a.y;}
 else{f=d.z>=0?4:5;q=float2(d.z>=0?d.x:-d.x,d.y)/a.z;}
 return float3(q.x*.5+.5,.5-q.y*.5,float(f));
}
crtxpt_gi_lighting_native::PolymorphicLightInfo crtxpt_light_info(uint4 lo,uint4 hi){
 crtxpt_gi_lighting_native::PolymorphicLightInfo p;p.Center=asfloat(lo.xyz);p.ColorTypeAndFlags=lo.w;
 p.Direction1=hi.x;p.Direction2=hi.y;p.Scalars=hi.z;p.LogRadiance=hi.w;return p;
}
float4x4 crtxpt_light_pack_triangle(float3 a,float3 b,float3 c,float3 radiance){
 crtxpt_gi_lighting_native::TriangleLight t;t.base=a;t.edge1=b-a;t.edge2=c-a;t.radiance=radiance;
 crtxpt_gi_lighting_native::PolymorphicLightInfo p=t.Store(0).Base;
 return float4x4(float4(p.Center,asfloat(p.ColorTypeAndFlags)),asfloat(uint4(p.Direction1,p.Direction2,p.Scalars,p.LogRadiance)),0.0.xxxx,0.0.xxxx);
}
float4x4 crtxpt_light_pack_environment(uint3 node,float3 radiance,float weight){
 crtxpt_gi_lighting_native::PolymorphicLightInfo p=(crtxpt_gi_lighting_native::PolymorphicLightInfo)0;
 crtxpt_gi_lighting_native::PolymorphicLight::PackColor(radiance,p);
 p.ColorTypeAndFlags|=5u<<24;p.Direction1=(node.x<<16)|node.y;p.Direction2=node.z<<16;p.Scalars=asuint(weight);
 return float4x4(float4(0,0,0,asfloat(p.ColorTypeAndFlags)),asfloat(uint4(p.Direction1,p.Direction2,p.Scalars,p.LogRadiance)),0.0.xxxx,0.0.xxxx);
}
float4x4 crtxpt_light_sample_triangle(uint4 lo,uint4 hi,float2 u,float3 receiver){
 crtxpt_gi_lighting_native::PolymorphicLightInfoFull p;p.Base=crtxpt_light_info(lo,hi);p.Extended=crtxpt_gi_lighting_native::PolymorphicLightInfoEx::empty();
 crtxpt_gi_lighting_native::TriangleLight t=crtxpt_gi_lighting_native::TriangleLight::Create(p);
 crtxpt_gi_lighting_native::PolymorphicLightSample s=t.CalcSample(u,receiver);
 return float4x4(float4(s.Position,s.SolidAnglePdf),float4(s.Normal,0),float4(s.Radiance,s.LightSampleableByBSDF?1:0),0.0.xxxx);
}
float crtxpt_light_triangle_pdf(uint4 lo,uint4 hi,float3 receiver,float3 position){
 crtxpt_gi_lighting_native::PolymorphicLightInfoFull p;p.Base=crtxpt_light_info(lo,hi);p.Extended=crtxpt_gi_lighting_native::PolymorphicLightInfoEx::empty();
 return crtxpt_gi_lighting_native::TriangleLight::Create(p).CalcSolidAnglePdfForMIS(receiver,position);
}
float3 crtxpt_light_radiance(uint4 lo,uint4 hi){return crtxpt_gi_lighting_native::PolymorphicLight::UnpackColor(crtxpt_light_info(lo,hi));}
float crtxpt_light_power(uint4 lo,uint4 hi){
 if(((lo.w>>24)&15)==5)return asfloat(hi.z);
 crtxpt_gi_lighting_native::PolymorphicLightInfoFull p;p.Base=crtxpt_light_info(lo,hi);p.Extended=crtxpt_gi_lighting_native::PolymorphicLightInfoEx::empty();
 return crtxpt_gi_lighting_native::TriangleLight::Create(p).GetPower();
}
uint2 crtxpt_light_feedback_add(uint2 packed,uint light,float weight,float random){
 crtxpt_gi_lighting_native::LightFeedbackReservoir r=crtxpt_gi_lighting_native::LightFeedbackReservoir::Unpack8Byte(packed);
 r.Add(random,light,weight);return r.Pack8Byte();
}
uint crtxpt_light_feedback_flood(uint2 center,uint2 left,uint2 right,uint2 up,uint2 down,uint2 pixel,uint update){
 crtxpt_gi_lighting_native::LightFeedbackReservoir r=crtxpt_gi_lighting_native::LightFeedbackReservoir::Unpack8Byte(center);
 uint base=crtxpt_gi_lighting_native::Hash32Combine(crtxpt_gi_lighting_native::Hash32(0x035F9F29u),(pixel.x<<16)|pixel.y);
 uint state=crtxpt_gi_lighting_native::Hash32Combine(crtxpt_gi_lighting_native::Hash32Combine(base,0u),update);
 if(r.CandidateIndex==0xFFFFFFFFu){
  uint2 packed[4]={left,right,up,down};
  for(uint i=0;i<4;++i){
   crtxpt_gi_lighting_native::LightFeedbackReservoir other=crtxpt_gi_lighting_native::LightFeedbackReservoir::Unpack8Byte(packed[i]);
   if(other.CandidateIndex==0xFFFFFFFFu)continue;
   other.Scale(.05f);state=crtxpt_gi_lighting_native::Hash32(state);float random=float(state>>8)/16777216.0;
   r.Merge(random,other);
  }
 }
 return r.CandidateIndex;
}
#undef FLT_EPSILON_MINI
#undef MAX_SOLID_ANGLE_PDF
#undef RTXPT_INVALID_LIGHT_INDEX

#endif // CORONA_RTXPT_GI_NATIVE_LIGHTING_LITERAL
)RTXPTLIGHT",
};
inline const std::string LiteralSource = [] {
    std::string source;
    source.reserve(19173);
    for (std::string_view part : LiteralSourceParts) source.append(part);
    return source;
}();
}
