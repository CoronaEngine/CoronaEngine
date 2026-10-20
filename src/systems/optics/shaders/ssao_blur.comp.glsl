#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint inputImageIndex;
    uint outputImageIndex;
} pushConsts;

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 maxPixel = ivec2(pushConsts.gbufferSize) - ivec2(1);
    float ao = 0.0;

    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            ivec2 coord = clamp(pixel + ivec2(x, y), ivec2(0), maxPixel);
            ao += texelFetch(textures[nonuniformEXT(pushConsts.inputImageIndex)], coord, 0).r;
        }
    }

    ao *= 1.0 / 16.0;
    imageStore(imagesRGBA16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
               vec4(ao, ao, ao, 1.0));
}
