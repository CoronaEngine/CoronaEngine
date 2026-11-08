#pragma once

#include <span>
#include <volk.h>

namespace Corona::Systems::DisplayDetail {

struct PresentationSurface {
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkExtent2D extent{};
};

// Uses Horizon's swapchain format preference for the main device.
// This queries the native surface; it does not claim to expose swapchain state.
[[nodiscard]] VkSurfaceFormatKHR choose_presentation_surface_format(std::span<const VkSurfaceFormatKHR> formats);
[[nodiscard]] bool presentation_surface_encodes_srgb(VkFormat format) noexcept;
[[nodiscard]] PresentationSurface query_presentation_surface(void* native_window);

// SDL exposes NSWindow on Cocoa; Horizon consumes its NSView.
// Native surface identities held by Engine remain unchanged.
[[nodiscard]] void* horizon_native_window(void* native_window);

}  // namespace Corona::Systems::DisplayDetail
