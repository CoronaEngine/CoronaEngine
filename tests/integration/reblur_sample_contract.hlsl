// Diagnostic hardware linear-clamp sampler oracle for the REBLUR EDSL port.
// Compile with the native NRD Vulkan binding shifts: s=100, t=200, b=300, u=400.
struct ProbeConstants
{
    uint64_t output;
};
[[vk::push_constant]] ConstantBuffer<ProbeConstants> g_Constants;
Texture2D<float4> g_Input : register(t0);
SamplerState g_LinearClamp : register(s0);

[numthreads(16, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= 256)
        return;
    float2 uv = (float2((i % 16) * .137f + .12345f,
                       (i / 16) * .141f + .23456f) + .5f) / 4.f;
    float4 value = g_Input.SampleLevel(g_LinearClamp, uv, 0);
    vk::RawBufferStore<float4>(g_Constants.output + uint64_t(i) * 16, value, 16);
}
