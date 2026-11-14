#pragma once
#include <string_view>
namespace vision::rtxpt_gi::prefix_literal { inline constexpr std::string_view Source=R"CRTXPTPFX(/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/
#ifndef CORONA_RTXPT_GI_PREFIX_XFORM
#define CORONA_RTXPT_GI_PREFIX_XFORM
namespace crtxpt_gi_prefix_native {
float3x3 MatrixRotateFromTo( const float3 from, const float3 to, uniform bool columnMajor = true )
{
    const float e = dot(from, to);
    const float f = abs(e); //(e < 0)? -e:e;

    if( f > float( 1.0f - 1e-10f ) )
        return float3x3(1,0,0,0,1,0,0,0,1);

    const float3 v      = cross( from, to );
    /* ... use this hand optimized version (9 mults less) */
    const float h       = (1.0f)/(1.0f + e);      /* optimization by Gottfried Chen */
    const float hvx     = h * v.x;
    const float hvz     = h * v.z;
    const float hvxy    = hvx * v.y;
    const float hvxz    = hvx * v.z;
    const float hvyz    = hvz * v.y;

    float3x3 mtx;
    if (columnMajor)
    {
        mtx[0][0] = e + hvx * v.x;
        mtx[0][1] = hvxy - v.z;
        mtx[0][2] = hvxz + v.y;
        mtx[1][0] = hvxy + v.z;
        mtx[1][1] = e + h * v.y * v.y;
        mtx[1][2] = hvyz - v.x;
        mtx[2][0] = hvxz - v.y;
        mtx[2][1] = hvyz + v.x;
        mtx[2][2] = e + hvz * v.z;
    }
    else
    {
        mtx[0][0] = e + hvx * v.x;
        mtx[1][0] = hvxy - v.z;
        mtx[2][0] = hvxz + v.y;
        mtx[0][1] = hvxy + v.z;
        mtx[1][1] = e + h * v.y * v.y;
        mtx[2][1] = hvyz - v.x;
        mtx[0][2] = hvxz - v.y;
        mtx[1][2] = hvyz + v.x;
        mtx[2][2] = e + hvz * v.z;
    }
    return mtx;
}
}
float4x4 crtxpt_prefix_identity(){return float4x4(1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1);}
float4x4 crtxpt_prefix_transform(float4x4 old,float3 T,float3 B,float3 N,float3 newDir,float3 oldDir,bool transmission){
 float3x3 local;
 if(transmission) local=crtxpt_gi_prefix_native::MatrixRotateFromTo(newDir,oldDir);
 else {float3x3 tangent=float3x3(T,B,N);local=mul(float3x3(1,0,0,0,1,0,0,0,-1),tangent);local=mul(transpose(tangent),local);}
 float3x3 m=mul((float3x3)old,local);
 return float4x4(m[0][0],m[0][1],m[0][2],0,m[1][0],m[1][1],m[1][2],0,m[2][0],m[2][1],m[2][2],0,0,0,0,1);
}
float3 crtxpt_prefix_virtual(float4x4 image,float3 pos,float3 pivot){return mul((float3x3)image,pos-pivot)+pivot;}
#endif
)CRTXPTPFX"; }
