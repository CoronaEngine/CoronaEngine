// GI-only primitive boundary for the unmodified matched native model.
// All directions point away from the surface. F includes the scatter cosine.
crtxpt_gi_native::StandardBSDFData crtxpt_gi_bsdf_data(float4 p0, float4 p1, float4 p2, float4 p3)
{
    return crtxpt_gi_native::StandardBSDFData::make(
        p0.xyz, p1.xyz, p0.w, p1.w, p3.y, p2.xyz, p2.w, p3.x);
}

crtxpt_gi_native::FalcorBSDF crtxpt_gi_bsdf_make(
    float4 p0, float4 p1, float4 p2, float4 p3, uint2 header, float3 Nworld, float3 Vworld)
{
    crtxpt_gi_native::ShadingData sd;
    sd.mtl.packedData = header;
    sd.N = Nworld;
    sd.V = Vworld;
    // Keep StandardBSDF's actual ShadingData construction route, including
    // the native overload's forwarding of V,N to __init's N,V parameters.
    return crtxpt_gi_native::FalcorBSDF::make(sd, crtxpt_gi_bsdf_data(p0, p1, p2, p3));
}

float4x4 crtxpt_gi_bsdf_eval(
    float4 p0, float4 p1, float4 p2, float4 p3, uint2 header,
    float3 Nworld, float3 Vworld, float3 localView, float3 localScatter)
{
    crtxpt_gi_native::FalcorBSDF bsdf = crtxpt_gi_bsdf_make(p0, p1, p2, p3, header, Nworld, Vworld);
    float3 diffuse, specular;
    bsdf.eval(localView, localScatter, diffuse, specular);
    // A transport container: rows are extracted directly, never multiplied.
    return float4x4(float4(diffuse, 0), float4(specular, 0), float4(0,0,0,0), float4(0,0,0,0));
}

float crtxpt_gi_bsdf_pdf(
    float4 p0, float4 p1, float4 p2, float4 p3, uint2 header,
    float3 Nworld, float3 Vworld, float3 localView, float3 localScatter)
{
    crtxpt_gi_native::FalcorBSDF bsdf = crtxpt_gi_bsdf_make(p0, p1, p2, p3, header, Nworld, Vworld);
    return bsdf.evalPdf(localView, localScatter);
}

float4x4 crtxpt_gi_bsdf_sample(
    float4 p0, float4 p1, float4 p2, float4 p3, uint2 header,
    float3 Nworld, float3 Vworld, float3 localView, float3 u)
{
    crtxpt_gi_native::FalcorBSDF bsdf = crtxpt_gi_bsdf_make(p0, p1, p2, p3, header, Nworld, Vworld);
    float3 direction, weight;
    float pdf, lobeP;
    uint lobe;
    bool valid = bsdf.sample(localView, direction, pdf, weight, lobe, lobeP, u);
    // Preserve native component weight, mixture PDF and delta PDF=0.
    return float4x4(float4(direction, pdf), float4(weight, lobeP),
                    float4(asfloat(lobe), valid ? 1.f : 0.f, 0, 0), float4(0,0,0,0));
}

uint crtxpt_gi_bsdf_lobes(float4 p0, float4 p1, float4 p2, float4 p3)
{
    crtxpt_gi_native::StandardBSDFData data = crtxpt_gi_bsdf_data(p0, p1, p2, p3);
    return crtxpt_gi_native::FalcorBSDF::getLobes(data);
}


// Exact native enumeration; local-space directions, including zero-probability
// entries. The original evaluator currently returns count=2, with transmission
// at index0 and reflection at index1. Keep capacity3 and its original count.
float4x4 crtxpt_gi_bsdf_delta_lobes(
    float4 p0, float4 p1, float4 p2, float4 p3, uint2 header,
    float3 Nworld, float3 Vworld, float3 localView, out float4x4 tail)
{
    crtxpt_gi_native::FalcorBSDF bsdf = crtxpt_gi_bsdf_make(p0, p1, p2, p3, header, Nworld, Vworld);
    crtxpt_gi_native::DeltaLobe lobes[crtxpt_gi_native::cMaxDeltaLobes];
    for (uint i=0;i<crtxpt_gi_native::cMaxDeltaLobes;i++) lobes[i]=crtxpt_gi_native::DeltaLobe::make();
    int count=0; float nonDeltaPart=0;
    bsdf.evalDeltaLobes(localView,lobes,count,nonDeltaPart);
    tail=float4x4(float4(lobes[2].dir,lobes[2].probability),
                  float4(lobes[2].thp,asfloat(uint(lobes[2].transmission))),
                  float4(asfloat(uint(count)),nonDeltaPart,0,0),float4(0,0,0,0));
    return float4x4(float4(lobes[0].dir,lobes[0].probability),
                    float4(lobes[0].thp,asfloat(uint(lobes[0].transmission))),
                    float4(lobes[1].dir,lobes[1].probability),
                    float4(lobes[1].thp,asfloat(uint(lobes[1].transmission))));
}
