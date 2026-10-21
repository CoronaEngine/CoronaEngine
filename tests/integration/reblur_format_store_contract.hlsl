// Diagnostic native image-store and explicit half-encoding oracle for REBLUR.
// Compile with the native NRD Vulkan binding shifts: s=100, t=200, b=300, u=400.
// The source is uploaded as raw float32 bits. No arithmetic or intermediate
// float16 conversion precedes the four floating-point image stores. The fifth
// integer image records explicit f32tof16 encoding, as used by NRD PackData2;
// the sixth image decodes independent uploaded half bits, as in UnpackData2.
struct ProbeConstants
{
    uint count;
};
[[vk::push_constant]] ConstantBuffer<ProbeConstants> g_Constants;
ByteAddressBuffer g_Input : register(t0);
ByteAddressBuffer g_EncodedInput : register(t1);
[[vk::image_format("rgb10a2")]] RWTexture2D<float4> g_OutRgb10A2 : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_OutRgba16F : register(u1);
[[vk::image_format("r16f")]] RWTexture2D<float> g_OutR16F : register(u2);
[[vk::image_format("r8")]] RWTexture2D<float> g_OutR8 : register(u3);
[[vk::image_format("r32ui")]] RWTexture2D<uint> g_OutF32ToF16 : register(u4);
[[vk::image_format("r32f")]] RWTexture2D<float> g_OutF16ToF32 : register(u5);

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= g_Constants.count)
        return;
    float4 value = asfloat(g_Input.Load4(i * 16));
    uint2 pixel = uint2(i, 0);
    g_OutRgb10A2[pixel] = value;
    g_OutRgba16F[pixel] = value;
    g_OutR16F[pixel] = value.x;
    g_OutR8[pixel] = value.x;
    g_OutF32ToF16[pixel] = f32tof16(value.x);
    g_OutF16ToF32[pixel] = f16tof32(g_EncodedInput.Load(i * 4));
}
