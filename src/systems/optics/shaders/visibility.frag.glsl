#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts
{
    uint textureIndex;
    uint uniformBufferIndex;
    uint instanceID;
    uint padding0;
    mat4 modelMatrix;
} pushConsts;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 2, binding = 0, rg32ui) readonly uniform uimage2D visibilityImages[];

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint v_instanceID;

layout(location = 0) out uvec4 visibilityData;

void main()
{
    // Auxiliary exit-surface pass: rasterize the furthest surface belonging to
    // the object visible in the main VBuffer. Do not pair unrelated glass panes.
    // A compute-produced snapshot is already in GENERAL layout. Raster inputs
    // must not sample the original visibility/depth attachment layouts.
    // uniformBufferIndex carries this draw's object ID; zero padding keeps UI intact.
    if (pushConsts.padding0 != 0u) {
        ivec2 pixel=ivec2(gl_FragCoord.xy);
        uvec2 front=imageLoad(visibilityImages[nonuniformEXT(pushConsts.textureIndex)],pixel).rg;
        if(front.x==0u || front.x!=pushConsts.uniformBufferIndex)discard;
        float depth=uintBitsToFloat(front.y);
        if(gl_FragCoord.z>=depth-1e-7)discard;
    }
    visibilityData = uvec4(v_instanceID, uint(gl_PrimitiveID), 0u, 0u);
}
