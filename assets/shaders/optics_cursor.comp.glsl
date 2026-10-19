#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts {
    uint outputImage;
    uint outputWidth;
    uint outputHeight;
    uint originX;
    uint originY;
    float cursorX;
    float cursorY;
    uint cursorShape;
    uint cursorImage;
    float cursorSize;
    uint preserveExisting;
} pushConsts;

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 2, binding = 0, rgba16f) uniform image2D images[];

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

vec4 over(vec4 bg, vec4 fg)
{
    return vec4(fg.rgb + bg.rgb * (1.0 - fg.a),
                fg.a + bg.a * (1.0 - fg.a));
}

vec4 sampleCursor(vec2 local)
{
    if (pushConsts.cursorShape == 5u || pushConsts.cursorSize <= 0.0) {
        return vec4(0.0);
    }
    if (local.x < 0.0 || local.y < 0.0 ||
        local.x >= pushConsts.cursorSize || local.y >= pushConsts.cursorSize) {
        return vec4(0.0);
    }

    vec2 uv = (local + vec2(0.5)) / vec2(pushConsts.cursorSize);
    vec4 texel = textureLod(textures[nonuniformEXT(pushConsts.cursorImage)], uv, 0.0);
    texel = clamp(texel, vec4(0.0), vec4(1.0));
    return vec4(texel.rgb * texel.a, texel.a);
}

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy) + ivec2(pushConsts.originX, pushConsts.originY);
    if (pos.x >= int(pushConsts.outputWidth) || pos.y >= int(pushConsts.outputHeight)) {
        return;
    }

    vec4 base = pushConsts.preserveExisting != 0u
        ? imageLoad(images[nonuniformEXT(pushConsts.outputImage)], pos)
        : vec4(0.0);
    vec2 local = vec2(pos) + vec2(0.5) - vec2(pushConsts.cursorX, pushConsts.cursorY);
    vec4 cursor = sampleCursor(local);
    imageStore(images[nonuniformEXT(pushConsts.outputImage)], pos, over(base, cursor));
}
