#include "../common/render_resource_bindings.h"

// Horizon's public stream accepts recordable commands; the recorder definition
// is private. Isolate this compatibility bridge in one translation unit.
#include "hardware_wrapper_vulkan/hardware/command_ir.h"
#include "hardware_wrapper_vulkan/pipeline/vulkan_compute_pipeline.h"
#include "hardware_wrapper_vulkan/pipeline/vulkan_rasterizer_pipeline.h"
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
    for (const auto& image : writable_images) {
        dispatch.bindings.push_back({2, logical_binding, image,
            Horizon::DispatchBindingKind::StorageImage, Horizon::AccessKind::ReadWrite});
        dispatch.resource_uses.push_back({image, Horizon::AccessKind::ReadWrite});
    }
    recorder.dispatch(std::move(dispatch));
}

void IndirectRaster::record(Horizon::CommandRecorder& recorder) const {
    using Store=Horizon::ResourceStore<Horizon::RasterizerPipelineWrap,Horizon::NoopReleaser>;
    const auto resource=Horizon::read<Store>(Horizon::ResourceBridge::token(*pipeline));
    if(!resource || !resource->impl)
        throw std::logic_error("Indirect raster requires a valid graphics pipeline");
    const auto implementation=std::static_pointer_cast<Horizon::VulkanRasterizerPipeline>(resource->impl);
    implementation->bind_auto_resources();
    Horizon::CommandRecorder snapshot;
    implementation->record_into(snapshot);
    auto task=snapshot.close();
    for(auto& command:task.commands) {
        if(command.op==Horizon::CommandOp::BeginRendering)
            recorder.begin_rendering(command.payload.rendering);
        else if(command.op==Horizon::CommandOp::EndRendering)
            recorder.end_rendering();
        else if(command.op==Horizon::CommandOp::DrawIndexedIndirect) {
            auto draw=std::move(command.payload.draw_indexed_indirect);
            for(const auto& image:images)draw.resource_uses.push_back({image,Horizon::AccessKind::Read});
            recorder.draw_indexed_indirect({command.resources.at(0).handle},
                {command.resources.at(1).handle},{command.resources.at(2).handle},std::move(draw));
        } else throw std::logic_error("Unexpected command in indirect raster pass");
    }
}
} // namespace Corona::Systems::RenderDetail
