#version 460

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(push_constant) uniform PushConsts
{
    mat4 lightViewProjModel;
} pushConsts;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

void main()
{
    gl_Position = pushConsts.lightViewProjModel * vec4(inPosition, 1.0);
}
