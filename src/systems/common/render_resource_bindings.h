#pragma once

#include "horizon.h"

namespace Corona::Systems::RenderDetail {

// A descriptor number alone is not a resource dependency. Keep the reflected
// push-constant offset, but explicitly register the resource with Horizon.
// Bindless image tracking is keyed by (set,binding), so use a distinct logical
// binding for each handle. No per-resource descriptor set is allocated.
template <typename Pipeline, typename Key>
void bind_storage_image(Pipeline& pipeline, const Key& key,
                        const Horizon::HardwareImage& image) {
    auto slot = Horizon::BindingSlot::from(key);
    slot.bind_type = EmbeddedShader::ShaderCodeModule::ShaderResources::storageTexture;
    slot.set = 2;
    slot.binding = static_cast<uint32_t>(slot.byte_offset);
    pipeline.bind_image(slot, image);
}

template <typename Pipeline, typename Key>
void bind_sampled_image(Pipeline& pipeline, const Key& key,
                        const Horizon::HardwareImage& image) {
    auto slot = Horizon::BindingSlot::from(key);
    slot.bind_type = EmbeddedShader::ShaderCodeModule::ShaderResources::sampledImages;
    slot.set = 0;
    slot.binding = static_cast<uint32_t>(slot.byte_offset);
    pipeline.bind_image(slot, image);
}

template <typename Pipeline, typename Key>
void bind_storage_buffer(Pipeline& pipeline, const Key& key,
                         const Horizon::HardwareBuffer& buffer) {
    auto slot = Horizon::BindingSlot::from(key);
    slot.bind_type = EmbeddedShader::ShaderCodeModule::ShaderResources::storageBuffer;
    slot.set = 1;
    slot.binding = static_cast<uint32_t>(slot.byte_offset);
    pipeline.bind_buffer(slot, buffer);
}

struct RetainSubmission {
    std::shared_ptr<void> object;
    void record(Horizon::CommandRecorder& recorder) const;
};

} // namespace Corona::Systems::RenderDetail
