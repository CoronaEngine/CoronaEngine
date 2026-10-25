#include "../common/render_resource_bindings.h"

// Horizon's public stream accepts recordable commands; the recorder definition
// is private. Isolate this compatibility bridge in one translation unit.
#include "hardware_wrapper_vulkan/hardware/command_ir.h"
#include "hardware_wrapper_vulkan/pipeline/vulkan_compute_pipeline.h"
#include "hardware_wrapper_vulkan/resource_pool.h"
#include "horizon_resource_bridge.h"

namespace Corona::Systems::RenderDetail {
void RetainSubmission::record(Horizon::CommandRecorder& recorder) const {
    if (object) recorder.keep_alive(object);
}

void IndirectDispatch::record(Horizon::CommandRecorder& recorder) const {
    Horizon::CommandRecorder snapshot;
    using Store = Horizon::ResourceStore<Horizon::ComputePipelineWrap, Horizon::NoopReleaser>;
    const auto resource = Horizon::read<Store>(Horizon::ResourceBridge::token(*pipeline));
    if (!resource || !resource->impl)
        throw std::logic_error("Indirect dispatch requires a valid compute pipeline");
    const auto implementation = std::static_pointer_cast<Horizon::VulkanComputePipeline>(resource->impl);
    implementation->bind_auto_resources();
    implementation->record_into(*pipeline, snapshot);
    auto task = snapshot.close();
    if (task.commands.size() != 1 || task.commands.front().op != Horizon::CommandOp::Dispatch)
        throw std::logic_error("Expected one compute dispatch for indirect resources");
    auto dispatch = std::move(task.commands.front().payload.dispatch);
    // These are elements of the same bindless table. A stable logical binding
    // avoids compiling a pipeline variant for every visible texture count.
    constexpr uint32_t logical_binding = 0x10000;
    const auto append = [&](const Horizon::ResourceHandle& handle, uint32_t set,
                            Horizon::DispatchBindingKind kind) {
        dispatch.bindings.push_back({set, logical_binding, handle, kind, Horizon::AccessKind::Read});
        dispatch.resource_uses.push_back({handle, Horizon::AccessKind::Read});
    };
    for (const auto& buffer : buffers)
        append(buffer, 1, Horizon::DispatchBindingKind::StorageBuffer);
    for (const auto& image : images)
        append(image, 0, Horizon::DispatchBindingKind::StorageImage);
    recorder.dispatch(std::move(dispatch));
}
} // namespace Corona::Systems::RenderDetail
