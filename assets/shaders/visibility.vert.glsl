#version 460
#extension GL_EXT_nonuniform_qualifier : enable

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(push_constant) uniform PushConsts
{
    uint textureIndex;
    uint uniformBufferIndex;
    uint instanceID;
    uint padding0;
    mat4 modelMatrix;
} pushConsts;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;     // declared to match vertex stride, not used
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint v_instanceID;

void main()
{
    gl_Position = pushConsts.modelMatrix * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    v_instanceID = pushConsts.instanceID;
}
