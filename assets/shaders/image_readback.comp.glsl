#version 460
#extension GL_EXT_nonuniform_qualifier : enable
layout(local_size_x=8,local_size_y=8) in;
layout(set=2,binding=0,rgba16f) uniform readonly image2D images[];
layout(set=1,binding=0) buffer Readback { uint pixels[]; } buffers[];
layout(push_constant) uniform Params {
    uint inputImage;
    uint outputBuffer;
    uvec2 size;
} pc;
void main() {
    uvec2 p=gl_GlobalInvocationID.xy;
    if(any(greaterThanEqual(p,pc.size))) return;
    vec4 value=imageLoad(images[pc.inputImage],ivec2(p));
    if(any(isnan(value)) || any(isinf(value))) value=vec4(0,0,0,1);
    buffers[pc.outputBuffer].pixels[p.y*pc.size.x+p.x]=packUnorm4x8(clamp(value,0.0,1.0));
}
