#pragma once
#include <SDL3/SDL.h>
#include <memory>
#include <unordered_map>
#include "cef/browser_ui.h"
namespace Corona::Systems { class VulkanBackend; }
namespace Corona::Systems::UI {
bool initialize_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& backend);
void shutdown_sdl_ui(SDL_Window*& window, std::unique_ptr<VulkanBackend>& backend);
struct UiFrameContext {
    SDL_Window* window = nullptr;
    VulkanBackend* vulkan_backend = nullptr;
    int* active_tab_id = nullptr;
    bool* running = nullptr;
    bool* window_size_changed = nullptr;
};
class UiFrameRunner {
 public:
    void run_frame(UiFrameContext& context);
 private:
    BrowserInputHandler input_handler_;
    int captured_tab_ = -1;
    int hovered_tab_ = -1;
    unsigned int dockspace_ = 0;
    std::unordered_map<std::string, unsigned int> default_docks_;
};
}
