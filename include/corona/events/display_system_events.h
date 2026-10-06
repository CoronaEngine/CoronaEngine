#pragma once

#include <cstdint>
#include <future>
#include <memory>
#include <optional>

#include "corona/systems/ui/ui_surface_lifecycle.h"
#include "corona/systems/display/published_image.h"

namespace Corona::Events {
/**
 * @brief Display-internal event dispatched through EventBus on the same thread.
 */
struct DisplaySystemDemoEvent {
    int demo_value;
};

/**
 * @brief Cross-thread engine-to-display event dispatched through EventStream.
 */
struct EngineToDisplayDemoEvent {
    float delta_time;
};

/**
 * @brief Cross-thread display-to-engine event dispatched through EventStream.
 */
struct DisplayToEngineDemoEvent {
    float delta_time;
};

/**
 * @brief Display surface change dispatched through EventBus.
 */
struct DisplaySurfaceChangedEvent {
    void* surface;
    std::optional<Corona::Systems::UI::SurfaceCompletionTicket> registration_ticket;  ///< Display registration acknowledgement.
};

/**
 * @brief Display surface removal published through EventBus on the main thread.
 *
 * Triggered when an ImGui viewport window is destroyed. DisplaySystem owns the
 * surface swapchain and must wait for GPU work and release its displayer before
 * the main thread destroys the OS window. This prevents presentation to a
 * destroyed window. DisplaySystem fulfills `done` on its update() thread after
 * teardown; the publisher waits on the future to enforce destruction order.
 *
 * EventBus dispatch is synchronous: callbacks run on the publishing main thread.
 * They must only queue the request and return. Actual teardown and completion
 * notification run on the Display thread.
 */
struct DisplaySurfaceRemovedEvent {
    void* surface = nullptr;
    std::shared_ptr<std::promise<void>> done;                                    ///< Fulfilled after teardown on the Display thread.
    std::optional<Corona::Systems::UI::SurfaceCompletionTicket> removal_ticket;  ///< Surface lifecycle teardown acknowledgement.
};

/**
 * @brief Optics layer frame ready (published by OpticsSystem, consumed by DisplaySystem)
 */
struct OpticsFrameReadyEvent {
    void* surface = nullptr;
    std::uintptr_t image_handle = 0;
    uint64_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t viewport_x = 0;
    uint32_t viewport_y = 0;
    uint32_t viewport_width = 0;
    uint32_t viewport_height = 0;
    Systems::Detail::PublishedImage published_image;  ///< Required: copy the producer allocation's lifetime token.
};

/** Display has submitted a composite that consumes a Native optics frame. */
struct OpticsFrameConsumedEvent {
    void* surface = nullptr;
    uint64_t frame_index = 0;
    uint64_t submit_serial = 0;
};

/**
 * @brief UI layer frame ready (published by VulkanBackend, consumed by DisplaySystem)
 */
struct UIFrameReadyEvent {
    void* surface = nullptr;
    std::uintptr_t image_handle = 0;
    uint64_t frame_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::optional<Corona::Systems::UI::SurfaceCompletionTicket> first_present_ticket;  ///< First-frame presentation acknowledgement.
    Systems::Detail::PublishedImage published_image;  ///< Required: copy the producer allocation's lifetime token.
};

}  // namespace Corona::Events
