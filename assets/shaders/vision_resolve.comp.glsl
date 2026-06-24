#version 460
#extension GL_EXT_nonuniform_qualifier : enable

// Zero-copy Vision resolve pass.
// Reads Vision's PRE-tonemap linear HDR color (float32 RGBA, row-major
// y*width+x) from a CUDA buffer imported into Vulkan, applies Vision's exposure
// + ACES tone map, and writes the engine's RGBA16F display image. The float32 ->
// half16 narrowing happens implicitly on imageStore into the rgba16 target, so
// no CPU readback / conversion is needed.
//
// The source is Vision's accumulation_buffer_/rt_buffer_ (the input to
// FrameBuffer::tone_mapping_), NOT view_texture_: that final-color texture is a
// cuArray whose memory cannot be exported. So we tone map here instead, and this
// MUST match the Native tonemap (tonemap.comp) exactly or Vision<->Native
// switching shifts color. Both do:
//     exposed = Vision exposure curve (1 - exp(-color * E))  // NOT engine c*E
//     ldr     = ACES(exposed)
//     out     = linearToSrgb(ldr)   // sRGB encode for the UNORM swapchain
// The swapchain is R8G8B8A8_UNORM (SRGB_NONLINEAR colorspace) and the present
// blit does NOT auto-encode, so the sRGB OETF must be applied here in-shader.

layout (local_size_x = 8, local_size_y = 8) in;

// Bindless SSBO pool (matches lighting.comp.glsl set=1 layout).
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
// Bindless storage-image pool (matches tonemap.comp.glsl set=2 rgba16 layout).
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint  srcBufferIndex;   // imported Vision pre-tonemap buffer (float4 per pixel)
    uint  outputImage;      // engine finalOutputImage (RGBA16F)
    float exposure;         // Vision FrameBuffer exposure (default 1.0)
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

// 标准 sRGB OETF，必须与 Native tonemap.comp 的 linearToSrgb 完全一致。
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
    uint base = (gl_GlobalInvocationID.y * pushConsts.gbufferSize.x +
                 gl_GlobalInvocationID.x) * 4u;

    uint srcIdx = nonuniformEXT(pushConsts.srcBufferIndex);
    vec3 hdr = vec3(
        uintBitsToFloat(ssbos[srcIdx].data[base + 0u]),
        uintBitsToFloat(ssbos[srcIdx].data[base + 1u]),
        uintBitsToFloat(ssbos[srcIdx].data[base + 2u]));

    // Vision exposure curve, then ACES, then sRGB encode (matches Native tonemap).
    vec3 exposed = vec3(1.0) - exp(-hdr * pushConsts.exposure);
    vec3 ldr = linearToSrgb(acesFilmicToneMapCurve(exposed));

    imageStore(imagesRGBA16[nonuniformEXT(pushConsts.outputImage)], pixel, vec4(ldr, 1.0));
}
