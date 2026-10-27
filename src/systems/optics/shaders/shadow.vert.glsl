#version 460
out gl_PerVertex {vec4 gl_Position;};
layout(push_constant) uniform PushConsts {
    mat4 lightViewProjModel;
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    uint lpvInfo;
    uint shadowInfo;
} pushConsts;
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inTexCoord;
layout(location=0) out vec3 worldPosition;
layout(location=1) out vec3 worldNormal;
void main() {
    vec4 p=vec4(inPosition,1);
    worldPosition=vec3(dot(pushConsts.modelRow0,p),dot(pushConsts.modelRow1,p),dot(pushConsts.modelRow2,p));
    mat3 model=transpose(mat3(pushConsts.modelRow0.xyz,pushConsts.modelRow1.xyz,pushConsts.modelRow2.xyz));
    worldNormal=transpose(inverse(model))*inNormal;
    gl_Position=pushConsts.lightViewProjModel*p;
}
