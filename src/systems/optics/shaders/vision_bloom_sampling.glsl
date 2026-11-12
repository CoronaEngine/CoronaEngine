vec3 loadVisionBloomTexel(uint bufferIndex, uvec2 size, uvec2 pixel)
{
    uint base = (pixel.y * size.x + pixel.x) * 2u;
    vec2 rg = unpackHalf2x16(ssbos[nonuniformEXT(bufferIndex)].data[base]);
    float b = unpackHalf2x16(ssbos[nonuniformEXT(bufferIndex)].data[base + 1u]).x;
    return vec3(rg, b);
}

vec3 sampleVisionBloom(uint bufferIndex, uvec2 size, vec2 uv)
{
    vec2 pos = clamp(uv * vec2(size) - 0.5, vec2(0.0), vec2(size) - 1.0);
    uvec2 lo = uvec2(floor(pos));
    uvec2 hi = min(lo + 1u, size - 1u);
    vec2 t = fract(pos);
    return mix(mix(loadVisionBloomTexel(bufferIndex, size, lo),
                   loadVisionBloomTexel(bufferIndex, size, uvec2(hi.x, lo.y)), t.x),
               mix(loadVisionBloomTexel(bufferIndex, size, uvec2(lo.x, hi.y)),
                   loadVisionBloomTexel(bufferIndex, size, hi), t.x), t.y);
}

vec3 compositeVisionBloom(vec3 hdr, uvec2 pixel, uvec2 size,
                          uint bloomBufferIndex, uvec2 bloomSize, float intensity)
{
    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec3 blended = mix(hdr, sampleVisionBloom(bloomBufferIndex, bloomSize, uv), intensity);
    // RTXPT blends into its RGBA16F HDR target before metering and tone mapping.
    vec2 rg = unpackHalf2x16(packHalf2x16(blended.rg));
    float b = unpackHalf2x16(packHalf2x16(vec2(blended.b, 1.0))).x;
    return vec3(rg, b);
}
