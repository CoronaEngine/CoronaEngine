#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

#include "presentation_surface.h"
#include <horizon.h>
#include "hardware_wrapper_vulkan/hardware_context.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace Corona::Systems::DisplayDetail {
namespace {

void check_vulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed. VkResult=" + std::to_string(result));
    }
}

struct TemporarySurface {
    VkInstance instance{};
    VkSurfaceKHR handle{};
    ~TemporarySurface() {
        if (handle != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance, handle, nullptr);
    }
};

VkExtent2D drawable_extent(void* native_window) {
#if defined(_WIN32)
    const auto window = static_cast<HWND>(native_window);
    RECT rect{};
    if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) ||
        !GetClientRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return {};
    }
    return {static_cast<uint32_t>(rect.right - rect.left), static_cast<uint32_t>(rect.bottom - rect.top)};
#elif defined(__APPLE__)
    NSView* view = static_cast<NSView*>(native_window);
    if (!view.window || !view.window.visible || view.window.miniaturized) return {};
    const CGSize size = [view convertSizeToBacking:view.bounds.size];
    if (size.width <= 0 || size.height <= 0) return {};
    return {static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)};
#else
    (void)native_window;
    throw std::runtime_error("Presentation surfaces require a Win32 or Cocoa native window");
#endif
}

void create_native_surface(TemporarySurface& surface, void* native_window) {
#if defined(_WIN32)
    if (!vkCreateWin32SurfaceKHR) throw std::runtime_error("vkCreateWin32SurfaceKHR is unavailable");
    VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = static_cast<HWND>(native_window);
    check_vulkan(vkCreateWin32SurfaceKHR(surface.instance, &info, nullptr, &surface.handle), "vkCreateWin32SurfaceKHR");
#elif defined(__APPLE__)
    if (!vkCreateMetalSurfaceEXT) throw std::runtime_error("vkCreateMetalSurfaceEXT is unavailable");
    @autoreleasepool {
        NSView* view = static_cast<NSView*>(native_window);
        view.wantsLayer = YES;
        if (![view.layer isKindOfClass:[CAMetalLayer class]]) view.layer = [CAMetalLayer layer];
        VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        info.pLayer = static_cast<CAMetalLayer*>(view.layer);
        check_vulkan(vkCreateMetalSurfaceEXT(surface.instance, &info, nullptr, &surface.handle), "vkCreateMetalSurfaceEXT");
    }
#else
    (void)surface;
    (void)native_window;
    throw std::runtime_error("Presentation surfaces require a Win32 or Cocoa native window");
#endif
}

}  // namespace

VkSurfaceFormatKHR choose_presentation_surface_format(std::span<const VkSurfaceFormatKHR> formats) {
    if (formats.empty()) throw std::runtime_error("No Vulkan surface formats are available for presentation");
    if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
        return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    }
    for (const VkFormat preferred : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        const auto found = std::ranges::find_if(formats, [preferred](const VkSurfaceFormatKHR& format) {
            return format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (found != formats.end()) return *found;
    }
    return formats.front();
}

bool presentation_surface_encodes_srgb(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
    }
}

void* horizon_native_window(void* native_window) {
    if (!native_window) throw std::runtime_error("Presentation native window is null");
#if defined(__APPLE__)
    id object = static_cast<id>(native_window);
    if ([object isKindOfClass:[NSWindow class]]) {
        NSWindow* window = static_cast<NSWindow*>(object);
        return window.contentView;
    }
    if (![object isKindOfClass:[NSView class]]) throw std::runtime_error("Presentation requires NSWindow or NSView");
#endif
    return native_window;
}

PresentationSurface query_presentation_surface(void* native_window) {
    native_window = horizon_native_window(native_window);
    PresentationSurface result;
    result.extent = drawable_extent(native_window);
    if (result.extent.width == 0 || result.extent.height == 0) return result;

    TemporarySurface surface{Horizon::vulkan_instance()};
    create_native_surface(surface, native_window);
    const auto physical_device = Horizon::main_device_context().device_manager.physical_device();
    uint32_t count = 0;
    check_vulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface.handle, &count, nullptr),
                 "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(count);
    if (count != 0) {
        check_vulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface.handle, &count, formats.data()),
                     "vkGetPhysicalDeviceSurfaceFormatsKHR");
        formats.resize(count);
    }
    result.format = choose_presentation_surface_format(formats);
    return result;
}

}  // namespace Corona::Systems::DisplayDetail
