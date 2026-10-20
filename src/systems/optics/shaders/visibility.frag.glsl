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

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint v_instanceID;

layout(location = 0) out uvec4 visibilityData;

void main()
{
    visibilityData = uvec4(v_instanceID, uint(gl_PrimitiveID), 0u, 0u);
}
