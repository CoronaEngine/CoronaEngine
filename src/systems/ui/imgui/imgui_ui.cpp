#include "imgui_ui.h"

#include <algorithm>

#include <corona/kernel/core/i_logger.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <corona/systems/ui/vulkan_backend.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "cef/browser_manager.h"
#include "cef/cef_client.h"

namespace Corona::Systems::UI {

namespace {
constexpr int kViewportCursorPixels = 48;
constexpr char kMouseIconRelativePath[] = "assets/icon/mouse_icon.png";

struct CursorIconPixels {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};

std::filesystem::path find_mouse_icon_path() {
    std::error_code ec;
    auto current = std::filesystem::current_path(ec);
    if (!ec) {
        for (auto dir = current; !dir.empty(); dir = dir.parent_path()) {
            auto candidate = dir / kMouseIconRelativePath;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return candidate;
            }
            ec.clear();
            if (dir == dir.parent_path()) {
                break;
            }
        }
    }
    return std::filesystem::path(kMouseIconRelativePath);
}

std::optional<CursorIconPixels> load_mouse_icon_pixels() {
    const auto icon_path = find_mouse_icon_path();
    const auto image_id = Resource::ResourceManager::get_instance().import_sync(icon_path);
    if (image_id == Resource::IResource::INVALID_UID) {
        CFW_LOG_WARNING("Viewport cursor icon load failed: {}", icon_path.string());
        return std::nullopt;
    }

    auto image = Resource::ResourceManager::get_instance().acquire_read<Resource::Image>(image_id);
    if (!image || image->get_width() <= 0 || image->get_height() <= 0 || image->get_data() == nullptr) {
        CFW_LOG_WARNING("Viewport cursor icon data invalid: {}", icon_path.string());
        return std::nullopt;
    }

    CursorIconPixels pixels;
    pixels.width = image->get_width();
    pixels.height = image->get_height();
    const int channels = image->get_channels();
    const auto pixel_count = static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height);
    pixels.rgba.resize(pixel_count * 4);
    const unsigned char* src = image->get_data();
    if (channels == 4) {
        std::copy(src, src + pixel_count * 4, pixels.rgba.begin());
    } else if (channels == 3) {
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels.rgba[i * 4 + 0] = src[i * 3 + 0];
            pixels.rgba[i * 4 + 1] = src[i * 3 + 1];
            pixels.rgba[i * 4 + 2] = src[i * 3 + 2];
            pixels.rgba[i * 4 + 3] = 255;
        }
    } else if (channels == 1) {
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels.rgba[i * 4 + 0] = src[i];
            pixels.rgba[i * 4 + 1] = src[i];
            pixels.rgba[i * 4 + 2] = src[i];
            pixels.rgba[i * 4 + 3] = 255;
        }
    } else {
        CFW_LOG_WARNING("Viewport cursor icon has unsupported channel count: {}", channels);
        return std::nullopt;
    }
    return pixels;
}

std::vector<unsigned char> resize_cursor_icon(const CursorIconPixels& src, int width, int height) {
    std::vector<unsigned char> dst(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const int src_y = std::min(src.height - 1, (y * src.height) / height);
        for (int x = 0; x < width; ++x) {
            const int src_x = std::min(src.width - 1, (x * src.width) / width);
            const size_t src_offset = (static_cast<size_t>(src_y) * src.width + src_x) * 4;
            const size_t dst_offset = (static_cast<size_t>(y) * width + x) * 4;
            std::copy_n(src.rgba.data() + src_offset, 4, dst.data() + dst_offset);
        }
    }
    return dst;
}
}  // namespace

// ============================================================================
// SDL/ImGui 生命周期管理
// ============================================================================

bool initialize_sdl_imgui(SDL_Window*& window, ImGuiIO*& io, std::unique_ptr<VulkanBackend>& vulkan_backend) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        CFW_LOG_ERROR("Failed to initialize SDL: {}", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    // SDL desktop modes are physical pixels on high-DPI Windows; SDL window
    // sizes are desktop/window coordinates.
    int initial_width = 1920;
    int initial_height = 1080;
    int initial_x = SDL_WINDOWPOS_CENTERED;
    int initial_y = SDL_WINDOWPOS_CENTERED;
    SDL_DisplayID primary_display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* desktop_mode = nullptr;
    if (primary_display != 0) {
        desktop_mode = SDL_GetDesktopDisplayMode(primary_display);
    }

    if (desktop_mode) {
        float display_scale = SDL_GetDisplayContentScale(primary_display);
        if (display_scale <= 0.0f) {
            display_scale = 1.0f;
        }

        float desktop_width = static_cast<float>(desktop_mode->w) / display_scale;
        float desktop_height = static_cast<float>(desktop_mode->h) / display_scale;

        SDL_Rect usable_bounds{};
        if (SDL_GetDisplayUsableBounds(primary_display, &usable_bounds) &&
            usable_bounds.w > 0 && usable_bounds.h > 0) {
            desktop_width = std::min(desktop_width, static_cast<float>(usable_bounds.w));
            desktop_height = std::min(desktop_height, static_cast<float>(usable_bounds.h));
        }

        initial_width = static_cast<int>(desktop_width * 0.8f);
        initial_height = static_cast<int>(desktop_height * 0.8f);
        initial_x = static_cast<int>((desktop_width - static_cast<float>(initial_width)) * 0.5f);
        initial_y = static_cast<int>((desktop_height - static_cast<float>(initial_height)) * 0.5f);
    }

    window = SDL_CreateWindow("Corona Engine (Horizon)", initial_width, initial_height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        CFW_LOG_ERROR("Failed to create window: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    SDL_SetWindowPosition(window, initial_x, initial_y);
    // SDL_ShowWindow(window);
    SDL_StartTextInput(window);
    // SDL_MaximizeWindow(window);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "1");
    BrowserManager::instance().set_main_window(window);

    vulkan_backend = std::make_unique<VulkanBackend>(window);
    if (!vulkan_backend->initialize()) {
        CFW_LOG_ERROR("Failed to initialize Cabbage UI backend");
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    io = &ImGui::GetIO();
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io->ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io->ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // TODO: 明确这是自定义 renderer，并禁用未实现的 RendererHasTextures 路径
    // 确保字体图集在首帧前已建立，避免 atlas->Builder 为 nullptr
    // 不熟看你们怎么改
    io->BackendRendererName = "Corona.Horizon";
    io->BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

    io->Fonts->AddFontDefault();
    io->Fonts->Build();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(0.2f, 0.2f, 0.8f, 0.3f);
    style.WindowRounding = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(1.0f, 1.0f);

    /*ImGui_ImplSDL3_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = vulkan_backend->get_instance();
    init_info.PhysicalDevice = vulkan_backend->get_physical_device();
    init_info.Device = vulkan_backend->get_device();
    init_info.QueueFamily = vulkan_backend->get_queue_family();
    init_info.Queue = vulkan_backend->get_queue();
    init_info.DescriptorPool = vulkan_backend->get_descriptor_pool();
    init_info.PipelineInfoMain.RenderPass = vulkan_backend->get_render_pass();
    init_info.MinImageCount = vulkan_backend->get_min_image_count();
    init_info.ImageCount = vulkan_backend->get_image_count();
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = false;

    ImGui_ImplVulkan_Init(&init_info);*/

    ImGui_ImplSDL3_InitForOther(window);

    // Register renderer viewport callbacks now that ImGui context and SDL3 platform are ready.
    vulkan_backend->register_viewport_callbacks();

    return true;
}

void shutdown_sdl_imgui(SDL_Window*& window, ImGuiIO*& io, std::unique_ptr<VulkanBackend>& vulkan_backend) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (vulkan_backend) {
        vulkan_backend->shutdown();
        vulkan_backend.reset();
        // BrowserManager::instance().set_vulkan_backend(nullptr);
    }

    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }

    SDL_StopTextInput(window);
    SDL_Quit();

    io = nullptr;
}

// ============================================================================
// UiLayoutManager 实现
// ============================================================================

ImGuiID UiLayoutManager::setup_dockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking |
                                    ImGuiWindowFlags_NoTitleBar |
                                    ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus |
                                    ImGuiWindowFlags_NoNavFocus;
    window_flags |= ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_DockingEmptyBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, window_flags);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);

    ImGuiID dock_space_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dock_space_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar);

    dockspace_active_ = true;

    return dock_space_id;
}

void UiLayoutManager::end_dockspace() {
    if (dockspace_active_) {
        ImGui::End();
        dockspace_active_ = false;
    }
}

SDL_Cursor* UiFrameRunner::ensure_viewport_system_cursor() {
    if (viewport_system_cursor_ || viewport_system_cursor_load_attempted_) {
        return viewport_system_cursor_;
    }
    viewport_system_cursor_load_attempted_ = true;

    auto pixels = load_mouse_icon_pixels();
    if (!pixels) {
        return nullptr;
    }

    auto resized = resize_cursor_icon(*pixels, kViewportCursorPixels, kViewportCursorPixels);
    SDL_Surface* surface = SDL_CreateSurfaceFrom(kViewportCursorPixels,
                                                 kViewportCursorPixels,
                                                 SDL_PIXELFORMAT_RGBA32,
                                                 resized.data(),
                                                 kViewportCursorPixels * 4);
    if (!surface) {
        CFW_LOG_WARNING("Viewport cursor SDL surface creation failed: {}", SDL_GetError());
        return nullptr;
    }

    viewport_system_cursor_ = SDL_CreateColorCursor(surface, 0, 0);
    SDL_DestroySurface(surface);
    if (!viewport_system_cursor_) {
        CFW_LOG_WARNING("Viewport cursor creation failed: {}", SDL_GetError());
    }
    return viewport_system_cursor_;
}

void UiFrameRunner::apply_system_cursor_visibility(SDL_Window* main_window, int active_tab_id) {
    bool should_hide = false;
    bool should_use_custom = false;
    SDL_Window* mouse_window = SDL_GetMouseFocus();
    const SDL_WindowID mouse_window_id = mouse_window ? SDL_GetWindowID(mouse_window) : 0;

    if (mouse_window_id != 0) {
        for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
            if (!tab || !tab->open || tab->minimized) {
                continue;
            }
            if (tab->camera_view && tab->platform_window_id == mouse_window_id) {
                should_hide = tab->hide_system_cursor.load(std::memory_order_relaxed);
                should_use_custom = tab->use_custom_system_cursor.load(std::memory_order_relaxed);
                break;
            }
            if (!tab->camera_view && tab->docking_pos == "main" && main_window &&
                mouse_window == main_window && tab_id == active_tab_id) {
                should_hide = tab->hide_system_cursor.load(std::memory_order_relaxed);
                should_use_custom = tab->use_custom_system_cursor.load(std::memory_order_relaxed);
                break;
            }
        }
    }

    if (should_hide) {
        // ImGui_ImplSDL3_NewFrame() may restore the OS cursor every frame.
        // Keep reapplying hidden while an Optics 3D cursor owns this viewport.
        SDL_HideCursor();
        system_cursor_hidden_ = true;
        return;
    }

    if (system_cursor_hidden_) {
        SDL_ShowCursor();
        system_cursor_hidden_ = false;
    }

    if (should_use_custom) {
        if (SDL_Cursor* cursor = ensure_viewport_system_cursor()) {
            SDL_SetCursor(cursor);
            system_cursor_custom_ = true;
            return;
        }
    }

    if (system_cursor_custom_) {
        if (SDL_Cursor* default_cursor = SDL_GetDefaultCursor()) {
            SDL_SetCursor(default_cursor);
        }
        system_cursor_custom_ = false;
    }
}
// ============================================================================
// UiFrameRunner 实现
// ============================================================================

void UiFrameRunner::run_frame(UiFrameContext& context) {
    if (!context.running || !context.active_tab_id || !context.vulkan_backend) {
        return;
    }

    auto route_browser_platform_window = [&](SDL_WindowID window_id) {
        if (window_id == 0) {
            return;
        }
        for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
            if (tab && tab->docking_pos != "main" &&
                tab->platform_window_id == window_id) {
                *context.active_tab_id = tab_id;
                url_input_active_tab_ = -1;
                return;
            }
        }
    };

    auto route_event_window = [&](SDL_WindowID window_id) {
        if (context.window && window_id == SDL_GetWindowID(context.window)) {
            // Embedded browser docks share the main SDL window with the main
            // editor tab. Preserve the tab selected by ImGui mouse hit-testing.
            return;
        }
        route_browser_platform_window(window_id);
    };

    auto result = event_handler_.process_events(
        context.window, url_input_active_tab_,
        [&](const SDL_Event& event) {
            route_event_window(event.key.windowID);
            input_handler_.process_sdl_key_event(event);
        },
        [&](const SDL_Event& event) {
            route_event_window(event.text.windowID);
            input_handler_.process_sdl_text_event(event);
        },
        [&](const SDL_Event& event) {
            route_event_window(event.edit.windowID);
            input_handler_.process_sdl_ime_event(event);
        });

    if (SDL_Window* focused_window = SDL_GetKeyboardFocus()) {
        if (focused_window != context.window) {
            route_browser_platform_window(SDL_GetWindowID(focused_window));
        }
    }
#ifdef _WIN32
    if (HWND foreground = GetForegroundWindow()) {
        HWND main_hwnd = context.window
            ? static_cast<HWND>(SDL_GetPointerProperty(
                  SDL_GetWindowProperties(context.window),
                  SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                  nullptr))
            : nullptr;
        if (!main_hwnd || foreground != main_hwnd) {
            for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
                if (tab && tab->docking_pos != "main" &&
                    tab->platform_handle_raw == foreground) {
                    *context.active_tab_id = tab_id;
                    url_input_active_tab_ = -1;
                    break;
                }
            }
        }
    }
#endif

    if (result.should_quit) {
        *context.running = false;
    }

    if (result.window_resized && context.window && context.window_size_changed) {
        *context.window_size_changed = true;
        context.vulkan_backend->request_rebuild();
    }

    // Forward text input before texture uploads and GPU presentation can block
    // this main-thread UI frame.
    if (*context.active_tab_id != -1 && url_input_active_tab_ == -1) {
        auto* tab = BrowserManager::instance().get_tab(*context.active_tab_id);
        if (tab && tab->client && tab->client->GetBrowser()) {
            tab->client->GetBrowser()->GetHost()->SetFocus(true);
            input_handler_.send_key_events_to_browser(tab->client->GetBrowser());
        } else {
            input_handler_.clear_pending_events();
        }
    } else {
        input_handler_.clear_pending_events();
    }

    if (context.vulkan_backend->is_rebuild_needed()) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(context.window, &width, &height);
        context.vulkan_backend->rebuild(width, height);
    }

    context.vulkan_backend->new_frame();
    ImGui_ImplSDL3_NewFrame();
    apply_system_cursor_visibility(context.window, *context.active_tab_id);

    // Ensure RendererHasTextures stays disabled — our custom renderer does not
    // implement the ImGui texture management API (font atlas is manual).
    if (context.io) {
        context.io->BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    }

    ImGui::NewFrame();

    ImGuiID dock_space_id = layout_manager_.setup_dockspace();

    std::vector<int> tabs_to_close = browser_renderer_.render_browser_tabs(dock_space_id, *context.active_tab_id, url_input_active_tab_, context.io);

    layout_manager_.end_dockspace();

    for (auto tab_id : tabs_to_close) {
        BrowserManager::instance().remove_tab(tab_id);
        if (tab_id == *context.active_tab_id) {
            *context.active_tab_id = -1;
        }
        if (tab_id == url_input_active_tab_) {
            url_input_active_tab_ = -1;
        }
    }

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
    if (!is_minimized) {
        context.vulkan_backend->render_frame(draw_data);
        context.vulkan_backend->present_frame();
    }

    if (context.io && (context.io->ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

}  // namespace Corona::Systems::UI
