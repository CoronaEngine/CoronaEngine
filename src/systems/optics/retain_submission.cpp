#include "../common/render_resource_bindings.h"

// Horizon's public stream accepts recordable commands; the recorder definition
// is private. Isolate this compatibility bridge in one translation unit.
#include "hardware_wrapper_vulkan/hardware/command_ir.h"

namespace Corona::Systems::RenderDetail {
void RetainSubmission::record(Horizon::CommandRecorder& recorder) const {
    if (object) recorder.keep_alive(object);
}
} // namespace Corona::Systems::RenderDetail
