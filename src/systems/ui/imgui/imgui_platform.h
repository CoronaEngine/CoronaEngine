#pragma once

namespace Corona::Systems { class VulkanBackend; }
namespace Corona::Systems::UI {
void install_imgui_platform(VulkanBackend& backend);
void retry_imgui_platform_retirement();
bool imgui_platform_retirement_pending();
void render_imgui_platform_windows(VulkanBackend& backend);
}
