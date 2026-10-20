#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout(local_size_x = 1, local_size_y = 1) in;

layout(set = 1, binding = 0) buffer SSBOPool { uint data[]; } ssbos[];
layout(set = 2, binding = 0, rg32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 pixel;
    uint visibilityImageIndex;
    uint outputBufferIndex;
} pushConsts;

void main()
{
    // Pick is intentionally tolerant to a small amount of pointer/viewport
    // rounding error. Keep the exact center pixel as the first choice, then
    // search the nearest non-zero visibility id in a 5x5 neighborhood.
    const int kRadius = 2;
    const uint visibilityIndex = pushConsts.visibilityImageIndex;
    const ivec2 center = ivec2(pushConsts.pixel);
    const ivec2 extent = imageSize(imagesRGBA32UI[nonuniformEXT(visibilityIndex)]);

    uint bestInstance = 0u;
    int bestDistance = 1 << 30;
    for (int offsetY = -kRadius; offsetY <= kRadius; ++offsetY) {
        for (int offsetX = -kRadius; offsetX <= kRadius; ++offsetX) {
            const ivec2 samplePixel = center + ivec2(offsetX, offsetY);
            if (samplePixel.x < 0 || samplePixel.y < 0 ||
                samplePixel.x >= extent.x || samplePixel.y >= extent.y) {
                continue;
            }

            const uint instance = imageLoad(
                imagesRGBA32UI[nonuniformEXT(visibilityIndex)], samplePixel).r;
            if (instance == 0u) {
                continue;
            }

            const int distance = offsetX * offsetX + offsetY * offsetY;
            if (bestInstance == 0u || distance < bestDistance) {
                bestInstance = instance;
                bestDistance = distance;
            }
        }
    }

    ssbos[nonuniformEXT(pushConsts.outputBufferIndex)].data[0] = bestInstance;
}
