#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 2, binding = 0, rgba16f) uniform image2D inputImageRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint inputImage;
    uint outputImage;
    float exposure;
} pushConsts;


vec3 acesFilmicToneMapCurve(vec3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;

    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// 标准 sRGB OETF（与 _SRGB framebuffer 的硬件行为一致）。swapchain 为
// R8G8B8A8_UNORM + SRGB_NONLINEAR，blit 到 UNORM 不会自动编码，必须在此手动编码，
// 否则线性 LDR 直送 sRGB 显示器会整体偏暗。Vision 路径 (vision_resolve.comp) 必须
// 使用完全相同的曲线，否则 Native<->Vision 切换会出现色差。
vec3 linearToSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92,
               1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}


void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

    vec4 hdrColor = imageLoad(inputImageRGBA16[pushConsts.inputImage], pixel);

    vec3 exposed = hdrColor.rgb * pushConsts.exposure;
    vec3 ldrColor = linearToSrgb(acesFilmicToneMapCurve(exposed));

    imageStore(inputImageRGBA16[pushConsts.outputImage], pixel, vec4(ldrColor, hdrColor.a));
}
