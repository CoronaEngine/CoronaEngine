#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(push_constant) uniform PushConsts {
    uint bgImage;
    uint fgImage;
    uint outputImage;
    uint outputWidth;
    uint outputHeight;
} pushConsts;

layout(set = 2, binding = 0, rgba16f) uniform image2D images[];

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

    if (pos.x >= pushConsts.outputWidth || pos.y >= pushConsts.outputHeight) {
        return;
    }

    vec4 bg = imageLoad(images[pushConsts.bgImage], pos);
    vec4 fg = imageLoad(images[pushConsts.fgImage], pos);
    vec3 color = fg.rgb + bg.rgb * (1.0 - fg.a);

    imageStore(images[pushConsts.outputImage], pos, vec4(color, 1.0));
}
