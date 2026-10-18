#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts {
    uint inputImage;
    uint outputImage;
    uint outputWidth;
    uint outputHeight;
    float lenticularPitch;
    float slant;
    float phaseOffset;
    float parallaxScale;
    vec4 rgbSubpixelOffsets;
} pushConsts;

layout(set = 2, binding = 0, rgba16f) uniform image2D images[];

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

float wrapPhaseCentered(float phase)
{
    float wrapped = fract(phase);
    return 2.0 * wrapped - 1.0;
}

// Continuous green-channel phase gamma_g in [-1, 1): a shader-friendly proxy
// for the exit angle. We deliberately use ONLY the central (green) sub-pixel
// offset and produce a single unified texture coordinate. Shifting R/G/B
// independently (as physical sub-pixel separation would suggest) causes severe
// color fringing at sharp UI edges; a single green-driven sample trades
// negligible optical exactness for absolute color cohesion.
float greenPhaseCentered(ivec2 pos)
{
    float greenOffset = pushConsts.rgbSubpixelOffsets[1];
    float pitch = max(abs(pushConsts.lenticularPitch), 1.0e-5);
    float phaseAccumulator =
        (float(pos.x) + greenOffset - pushConsts.slant * float(pos.y)) / pitch +
        pushConsts.phaseOffset;
    return wrapPhaseCentered(phaseAccumulator);
}

// Horizontal-only bilinear (2-tap linear) fetch. The overlay is a storage image
// (no hardware sampler), so we interpolate manually between the two neighboring
// columns to smooth the fractional sub-pixel offset and remove the staircase
// shimmer that nearest-neighbor rounding produced. The warp is X-only, so Y is
// sampled directly.
vec4 sampleBilinearX(float sampleX, int y)
{
    float maxX = float(int(pushConsts.outputWidth) - 1);
    float clamped = clamp(sampleX, 0.0, maxX);
    int x0 = int(floor(clamped));
    int x1 = min(x0 + 1, int(pushConsts.outputWidth) - 1);
    float w = clamped - float(x0);
    vec4 c0 = imageLoad(images[pushConsts.inputImage], ivec2(x0, y));
    vec4 c1 = imageLoad(images[pushConsts.inputImage], ivec2(x1, y));
    return mix(c0, c1, w);
}

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
    if (pos.x >= int(pushConsts.outputWidth) || pos.y >= int(pushConsts.outputHeight)) {
        return;
    }

    float gamma = greenPhaseCentered(pos);
    float sampleX = float(pos.x) + gamma * pushConsts.parallaxScale;
    vec4 warped = sampleBilinearX(sampleX, pos.y);

    if (warped.a <= 0.0) {
        imageStore(images[pushConsts.outputImage], pos, vec4(0.0));
        return;
    }

    imageStore(images[pushConsts.outputImage], pos, warped);
}
