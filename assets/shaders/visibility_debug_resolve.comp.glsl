#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform usampler2D uintTextures[];
layout(set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint outputImageIndex;
} pushConsts;

vec3 pseudoColor(uint id)
{
    return vec3(
        fract(float(id) * 0.618033988749),
        fract(float(id) * 0.381966011251),
        fract(float(id) * 0.737096774194));
}

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    uvec4 vis = texelFetch(uintTextures[nonuniformEXT(pushConsts.visibilityImageIndex)], pixel, 0);

    uint instanceID = vis.r;
    uint primitiveID = vis.g;

    vec4 color = vec4(0.0);
    if (instanceID != 0u) {
        color = vec4(pseudoColor(instanceID ^ (primitiveID * 2654435761u)), 1.0);
    }

    imageStore(imagesRGBA16[nonuniformEXT(pushConsts.outputImageIndex)], pixel, color);
}
