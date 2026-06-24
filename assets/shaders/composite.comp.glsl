#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts {
    uint bgImage;
    uint fgImage;
    uint outputImage;
    uint outputWidth;
    uint outputHeight;
    uint bgWidth;
    uint bgHeight;
    uint fgWidth;
    uint fgHeight;
    uint bgViewportX;
    uint bgViewportY;
    uint bgViewportWidth;
    uint bgViewportHeight;
    uint fgOpaque;
} pushConsts;

layout(set = 2, binding = 0, rgba16f) uniform image2D images[];

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

    if (pos.x >= pushConsts.outputWidth || pos.y >= pushConsts.outputHeight) {
        return;
    }

    vec4 bg = vec4(0.0);
    uint bgViewportWidth = max(pushConsts.bgViewportWidth, 1u);
    uint bgViewportHeight = max(pushConsts.bgViewportHeight, 1u);
    uint bgViewportX1 = pushConsts.bgViewportX + bgViewportWidth;
    uint bgViewportY1 = pushConsts.bgViewportY + bgViewportHeight;

    if (uint(pos.x) >= pushConsts.bgViewportX &&
        uint(pos.y) >= pushConsts.bgViewportY &&
        uint(pos.x) < bgViewportX1 &&
        uint(pos.y) < bgViewportY1) {
        vec2 bgUv = (vec2(pos) - vec2(pushConsts.bgViewportX, pushConsts.bgViewportY) + 0.5) /
                    vec2(bgViewportWidth, bgViewportHeight);

        ivec2 bgSize = ivec2(pushConsts.bgWidth, pushConsts.bgHeight);
        vec2 bgTexel = bgUv * vec2(bgSize) - 0.5;

        ivec2 base = ivec2(floor(bgTexel));
        ivec2 c0 = clamp(base, ivec2(0), bgSize - ivec2(1));
        ivec2 c1 = clamp(base + ivec2(1, 0), ivec2(0), bgSize - ivec2(1));
        ivec2 c2 = clamp(base + ivec2(0, 1), ivec2(0), bgSize - ivec2(1));
        ivec2 c3 = clamp(base + ivec2(1, 1), ivec2(0), bgSize - ivec2(1));
        vec2 f = fract(bgTexel);

        bg = mix(
            mix(imageLoad(images[pushConsts.bgImage], c0),
                imageLoad(images[pushConsts.bgImage], c1), f.x),
            mix(imageLoad(images[pushConsts.bgImage], c2),
                imageLoad(images[pushConsts.bgImage], c3), f.x),
            f.y
        );
    }

    vec2 uv = (vec2(pos) + 0.5) / vec2(pushConsts.outputWidth, pushConsts.outputHeight);
    ivec2 fgSize = ivec2(pushConsts.fgWidth, pushConsts.fgHeight);
    ivec2 fgPos = clamp(ivec2(floor(uv * vec2(fgSize))), ivec2(0), fgSize - ivec2(1));
    vec4 fg = imageLoad(images[pushConsts.fgImage], fgPos);
    float fgAlpha = pushConsts.fgOpaque != 0u ? 1.0 : fg.a;
    vec3 color = fg.rgb + bg.rgb * (1.0 - fgAlpha);

    imageStore(images[pushConsts.outputImage], pos, vec4(color.rgb, 1.0));
}
