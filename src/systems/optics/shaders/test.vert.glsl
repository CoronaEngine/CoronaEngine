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
    mat4 modelMatrix;
    vec4 materialColor;
    uint objectID;
} pushConsts;

layout(set = 1, binding = 0) readonly buffer UniformBufferObject
{
    mat4 viewProjMatrix;
} uniformBufferObjects[];

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec2 fragMotionVector;


void main() 
{
    vec4 worldPos = pushConsts.modelMatrix * vec4(inPosition, 1.0f);
    fragPos = worldPos.xyz; 
    gl_Position = uniformBufferObjects[pushConsts.uniformBufferIndex].viewProjMatrix * worldPos;

    fragTexCoord = inTexCoord;
    // Transform normal to world space (use inverse transpose for non-uniform scaling)
    mat3 normalMatrix = transpose(inverse(mat3(pushConsts.modelMatrix)));
    fragNormal = normalize(normalMatrix * inNormal);

    fragMotionVector = vec2(0.0f);
}
