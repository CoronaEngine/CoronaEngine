#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts
{    
    uint textureIndex;
    uint uniformBufferIndex;
    mat4 modelMatrix;
    vec4 materialColor;
    uint objectID;
} pushConsts;

layout(set = 1, binding = 0) buffer UniformBufferObject
{
    mat4 viewProjMatrix;
} uniformBufferObjects[];


layout (set = 0, binding = 0) uniform sampler2D textures[];

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec2 fragMotionVector;

layout(location = 0) out vec3 gbufferPostion;
layout(location = 1) out vec3 gbufferNormal;
layout(location = 2) out vec4 gbufferBaseColor;
layout(location = 3) out vec2 gbufferMotionVector;
layout(location = 4) out vec4 gbufferObjectID;

void main()
{
    // Alpha-cutout to avoid translucent Z-fighting on foliage
    vec4 sampleColor = texture(textures[pushConsts.textureIndex], fragTexCoord);
    if (sampleColor.a < 0.5f) {
        discard;
    }

    // 材质颜色乘以纹理颜色
    vec3 finalColor = sampleColor.rgb * pushConsts.materialColor.rgb;
    gbufferBaseColor = vec4(finalColor, 1.0f);
    
    // Flip normal for back-facing fragments (double-sided rendering support)
    vec3 normal = gl_FrontFacing ? fragNormal : -fragNormal;
    gbufferNormal = normalize(normal);
    
    gbufferPostion = fragPos;
    gbufferMotionVector = fragMotionVector;
    gbufferObjectID = vec4(float(pushConsts.objectID), 0.0, 0.0, 1.0);
}