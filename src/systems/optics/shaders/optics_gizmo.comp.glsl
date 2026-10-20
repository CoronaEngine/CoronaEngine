#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts {
    uint outputImage;
    uint outputWidth;
    uint outputHeight;
    uint originX;
    uint originY;
    vec2 gizmoOrigin;
    vec2 xDirection;
    vec2 yDirection;
    vec2 zDirection;
    uint xImage;
    uint yImage;
    uint zImage;
    uint activeAxis;
    uint hoverAxis;
    uint preserveExisting;
    float axisLength;
    vec4 xSourceAnchorTip;
    vec4 ySourceAnchorTip;
    vec4 zSourceAnchorTip;
} pushConsts;

layout(set = 0, binding = 0) uniform sampler2D textures[];
layout(set = 2, binding = 0, rgba16f) uniform image2D images[];

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

vec4 over(vec4 bg, vec4 fg)
{
    return vec4(fg.rgb + bg.rgb * (1.0 - fg.a),
                fg.a + bg.a * (1.0 - fg.a));
}

vec4 sampleAxis(uint axis,
                uint descriptor,
                vec2 screenDirection,
                vec4 sourceAnchorTip,
                vec2 screenRelative)
{
    ivec2 imageSize = textureSize(textures[nonuniformEXT(descriptor)], 0);
    if (imageSize.x <= 0 || imageSize.y <= 0) {
        return vec4(0.0);
    }

    vec2 sourceAxis = sourceAnchorTip.zw - sourceAnchorTip.xy;
    float sourceLength = length(sourceAxis);
    if (sourceLength <= 1.0e-4 || pushConsts.axisLength <= 0.0) {
        return vec4(0.0);
    }
    vec2 sourceDirection = sourceAxis / sourceLength;
    vec2 sourcePerpendicular = vec2(-sourceDirection.y, sourceDirection.x);
    vec2 screenPerpendicular = vec2(-screenDirection.y, screenDirection.x);
    float scale = pushConsts.axisLength / sourceLength;
    vec2 sourceRelative =
        sourceDirection * (dot(screenRelative, screenDirection) / scale) +
        sourcePerpendicular * (dot(screenRelative, screenPerpendicular) / scale);
    vec2 sourcePixel = sourceAnchorTip.xy + sourceRelative;
    if (sourcePixel.x < 0.0 || sourcePixel.y < 0.0 ||
        sourcePixel.x >= float(imageSize.x) || sourcePixel.y >= float(imageSize.y)) {
        return vec4(0.0);
    }

    vec2 uv = (sourcePixel + vec2(0.5)) / vec2(imageSize);
    vec4 texel = clamp(textureLod(textures[nonuniformEXT(descriptor)], uv, 0.0),
                       vec4(0.0), vec4(1.0));
    if (texel.a <= 0.01) {
        return vec4(0.0);
    }

    float brightness = 1.0;
    if (pushConsts.activeAxis != 0u) {
        brightness = pushConsts.activeAxis == axis ? 1.25 : 0.55;
    } else if (pushConsts.hoverAxis == axis) {
        brightness = 1.25;
    }
    texel.rgb = clamp(texel.rgb * brightness, vec3(0.0), vec3(1.0));
    return vec4(texel.rgb * texel.a, texel.a);
}

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy) +
                ivec2(pushConsts.originX, pushConsts.originY);
    if (pos.x >= int(pushConsts.outputWidth) ||
        pos.y >= int(pushConsts.outputHeight)) {
        return;
    }

    vec4 result = pushConsts.preserveExisting != 0u
        ? imageLoad(images[nonuniformEXT(pushConsts.outputImage)], pos)
        : vec4(0.0);
    vec2 relative = vec2(pos) + vec2(0.5) - pushConsts.gizmoOrigin;

    // Stable back-to-front order: Z, Y, X.
    result = over(result, sampleAxis(3u, pushConsts.zImage, pushConsts.zDirection,
                                     pushConsts.zSourceAnchorTip, relative));
    result = over(result, sampleAxis(2u, pushConsts.yImage, pushConsts.yDirection,
                                     pushConsts.ySourceAnchorTip, relative));
    result = over(result, sampleAxis(1u, pushConsts.xImage, pushConsts.xDirection,
                                     pushConsts.xSourceAnchorTip, relative));
    imageStore(images[nonuniformEXT(pushConsts.outputImage)], pos, result);
}
