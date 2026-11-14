// CPU shader compilation probe; it can also be dispatched by a validation
// harness. It has no scene/renderer dependency and never runs in product code.
#include "standard_bsdf.slangh"

struct ProbeInput {
    float4 p0, p1, p2, p3;
    uint2 header;
    float3 Nworld, Vworld, localView, localScatter, u;
};
StructuredBuffer<ProbeInput> inputs : register(t0);
RWStructuredBuffer<float4> outputs : register(u0);

[numthreads(1,1,1)]
void main(uint3 dispatch : SV_DispatchThreadID)
{
    ProbeInput p = inputs[dispatch.x];
    uint base = dispatch.x * 8;
    float4x4 f = crtxpt_gi_bsdf_eval(p.p0,p.p1,p.p2,p.p3,p.header,p.Nworld,p.Vworld,p.localView,p.localScatter);
    float4x4 s = crtxpt_gi_bsdf_sample(p.p0,p.p1,p.p2,p.p3,p.header,p.Nworld,p.Vworld,p.localView,p.u);
    float pdf = crtxpt_gi_bsdf_pdf(p.p0,p.p1,p.p2,p.p3,p.header,p.Nworld,p.Vworld,p.localView,p.localScatter);
    uint lobes = crtxpt_gi_bsdf_lobes(p.p0,p.p1,p.p2,p.p3);
    outputs[base+0] = f[0];
    outputs[base+1] = f[1];
    outputs[base+2] = float4(pdf,asfloat(lobes),0,0);
    outputs[base+3] = s[0];
    outputs[base+4] = s[1];
    outputs[base+5] = s[2];
    // Direct native methods are retained as an independent boundary oracle.
    crtxpt_gi_native::ShadingData sd;
    sd.mtl.packedData = p.header; sd.N = p.Nworld; sd.V = p.Vworld;
    crtxpt_gi_native::StandardBSDFData data = crtxpt_gi_native::StandardBSDFData::make(
        p.p0.xyz,p.p1.xyz,p.p0.w,p.p1.w,p.p3.y,p.p2.xyz,p.p2.w,p.p3.x);
    crtxpt_gi_native::FalcorBSDF direct = crtxpt_gi_native::FalcorBSDF::make(sd,data);
    float3 diffuse,specular,direction,weight;
    float samplePdf,lobeP;
    uint lobe;
    direct.eval(p.localView,p.localScatter,diffuse,specular);
    bool valid = direct.sample(p.localView,direction,samplePdf,weight,lobe,lobeP,p.u);
    uint mismatch = any(asuint(f[0].xyz) != asuint(diffuse)) ? 1u : 0u;
    mismatch |= any(asuint(f[1].xyz) != asuint(specular)) ? 2u : 0u;
    mismatch |= asuint(pdf) != asuint(direct.evalPdf(p.localView,p.localScatter)) ? 4u : 0u;
    mismatch |= any(asuint(s[0]) != asuint(float4(direction,samplePdf))) ? 8u : 0u;
    mismatch |= any(asuint(s[1]) != asuint(float4(weight,lobeP))) ? 16u : 0u;
    mismatch |= asuint(s[2].x) != lobe || (s[2].y != 0.f) != valid ? 32u : 0u;
    mismatch |= lobes != crtxpt_gi_native::FalcorBSDF::getLobes(data) ? 64u : 0u;
    outputs[base+6] = float4(asfloat(mismatch),0,0,0);
    outputs[base+7] = float4(0,0,0,0);
}
