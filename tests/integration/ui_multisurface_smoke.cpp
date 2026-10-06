#include <SDL3/SDL.h>

#include <corona/systems/display/display_system.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/ui/quad_compositor.h>
#include <corona/systems/ui/vulkan_backend.h>
#include <corona/events/display_system_events.h>

#include <cstdlib>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr int kSkip = 77;

bool verify_quad_pixels(Corona::Systems::DisplaySystem& display) try {
    namespace Horizon = Corona::Horizon;
    using namespace Corona::Systems;
    constexpr uint32_t width = 32;
    constexpr uint32_t height = 16;
    const auto capture_path = std::filesystem::temp_directory_path() /
                              ("corona-ui-pixels-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".raw");
    const auto remove_capture = [](const std::filesystem::path* path) {
        _putenv_s("HORIZON_FRAME_HASH_DUMP", "");
        std::error_code error;
        std::filesystem::remove(*path, error);
    };
    const std::unique_ptr<const std::filesystem::path, decltype(remove_capture)> capture_cleanup(&capture_path, remove_capture);
    // The backend capture path supplies a transfer-to-host barrier and coherent
    // staging memory; waiting on a shader receipt alone is not a CPU readback.
    _putenv_s("HORIZON_FRAME_HASH", "1");
    _putenv_s("HORIZON_FRAME_HASH_DUMP", capture_path.string().c_str());
    const auto window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>(
        SDL_CreateWindow("Corona UI pixel regression", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS), SDL_DestroyWindow);
    if (!window) {
        std::cerr << "UI pixel regression: cannot create capture window\n";
        return false;
    }
    const auto properties = SDL_GetWindowProperties(window.get());
    auto* surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    VulkanBackend backend(window.get());
    if (!backend.initialize()) {
        std::cerr << "UI pixel regression: cannot initialize backend\n";
        return false;
    }
    bool cleanup_failed = false;
    const auto retire_surface = [&](VulkanBackend* active_backend) noexcept {
        try {
            Corona::Kernel::KernelContext::instance().event_bus()->publish(
                Corona::Events::DisplaySurfaceRemovedEvent{surface});
            display.update();
        } catch (...) {
            std::cerr << "UI pixel regression: failed to retire Display surface\n";
            cleanup_failed = true;
        }
        try {
            active_backend->shutdown();
        } catch (...) {
            std::cerr << "UI pixel regression: failed to shut down backend\n";
            cleanup_failed = true;
        }
    };
    // Unwind Display and backend resources before the native window, even when
    // texture upload or rendering throws.
    std::unique_ptr<VulkanBackend, decltype(retire_surface)> surface_cleanup(&backend, retire_surface);
    display.update();
    Horizon::HardwareExecutor upload_executor;

    const std::array<std::byte, 4> blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
    Horizon::HardwareImage texture(Horizon::HardwareImageDesc::texture_2d(
        1, 1, Horizon::Format::SRGBA8_UNORM));
    const auto staging = Horizon::HardwareBuffer::from_bytes(
        blue, 1, Horizon::BufferUsage_TransferSrc, "ui.test.texture_upload");
    const auto upload_receipt = upload_executor.stream()
                                << texture.copy_from(staging)
                                << Horizon::commit();
    std::array<QuadDraw, 2> quads;
    quads[0].dest_max = ktm::fvec2(16.0f, 16.0f);
    quads[0].color = ktm::fvec4(1.0f, 0.0f, 0.0f, 1.0f);
    quads[1].texture = &texture;
    quads[1].texture_ready = upload_receipt;
    quads[1].dest_min = ktm::fvec2(16.0f, 0.0f);
    quads[1].dest_max = ktm::fvec2(32.0f, 16.0f);
    quads[1].has_clip = true;
    quads[1].clip_rect = ktm::fvec4(24.0f, 0.0f, 32.0f, 16.0f);
    for (int frame = 0; frame < 5; ++frame) {
        SDL_PumpEvents();
        backend.new_frame(surface);
        backend.render_quads(surface, quads, width, height);
        backend.present_surface(surface);
        display.update();
    }
    surface_cleanup.reset();
    if (cleanup_failed) return false;
    using Pixel = std::array<uint16_t, 4>;
    std::array<Pixel, width * height> pixels;
    std::ifstream capture(capture_path, std::ios::binary);
    capture.read(reinterpret_cast<char*>(pixels.data()), sizeof(pixels));
    const bool captured = capture.gcount() == sizeof(pixels);
    capture.close();
    if (!captured) {
        std::cerr << "UI pixel regression: no complete frame capture\n";
        return false;
    }
    const auto expect_pixel = [&](uint32_t x, uint32_t y, Pixel expected) {
        const auto& actual = pixels[y * width + x];
        if (actual != expected) {
            std::cerr << "UI pixel regression: (" << x << "," << y << ") expected half-float bits "
                      << expected[0] << "," << expected[1] << "," << expected[2] << "," << expected[3]
                      << " but got " << actual[0] << "," << actual[1] << "," << actual[2] << "," << actual[3] << '\n';
            return false;
        }
        return true;
    };
    constexpr uint16_t one = 0x3c00;  // IEEE 754 binary16 encoding of 1.0.
    return expect_pixel(4, 12, {one, 0, 0, one}) &&
           expect_pixel(12, 4, {one, 0, 0, one}) &&
           expect_pixel(26, 12, {0, 0, one, one}) &&
           expect_pixel(30, 2, {0, 0, one, one}) &&
           expect_pixel(20, 8, {0, 0, 0, one});
} catch (const std::exception& error) {
    std::cerr << "UI pixel regression setup failed: " << error.what() << '\n';
    return false;
}

class KernelSystemContext final : public Corona::Kernel::ISystemContext {
   public:
    Corona::Kernel::IEventBus* event_bus() override { return Corona::Kernel::KernelContext::instance().event_bus(); }
    Corona::Kernel::IEventBusStream* event_stream() override { return Corona::Kernel::KernelContext::instance().event_stream(); }
    Corona::Kernel::ISystem* get_system(std::string_view) override { return nullptr; }
    float get_delta_time() const override { return 1.0f / 120.0f; }
    uint64_t get_frame_number() const override { return frame_; }
    uint64_t frame_ = 0;
};

void destroy_windows(std::vector<SDL_Window*>& windows) {
    for (auto* window : windows) {
        if (window) SDL_DestroyWindow(window);
    }
    windows.clear();
}

int run_smoke() {
    if (std::getenv("CORONA_RUN_GPU_SMOKE") == nullptr ||
        std::string(std::getenv("CORONA_RUN_GPU_SMOKE")) != "1") {
        std::cout << "UiMultiSurfaceSmoke skipped; set CORONA_RUN_GPU_SMOKE=1 to enable\n";
        return kSkip;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "UiMultiSurfaceSmoke skipped: SDL_Init failed: " << SDL_GetError() << '\n';
        return kSkip;
    }
    auto& kernel = Corona::Kernel::KernelContext::instance();
    if (!kernel.initialize()) {
        std::cerr << "UiMultiSurfaceSmoke skipped: KernelContext initialization failed\n";
        SDL_Quit();
        return kSkip;
    }

    std::vector<SDL_Window*> windows;
    auto* main_window = SDL_CreateWindow("Corona UI multisurface smoke", 320, 240,
                                         SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!main_window) {
        std::cerr << "UiMultiSurfaceSmoke skipped: main SDL window failed: " << SDL_GetError() << '\n';
        SDL_Quit();
        return kSkip;
    }
    windows.push_back(main_window);

    Corona::Systems::DisplaySystem display;
    KernelSystemContext context;
    if (!display.initialize(&context)) {
        std::cerr << "UiMultiSurfaceSmoke failed: DisplaySystem initialization failed\n";
        destroy_windows(windows);
        kernel.shutdown();
        SDL_Quit();
        return 1;
    }
    if (!verify_quad_pixels(display)) {
        display.shutdown();
        kernel.shutdown();
        destroy_windows(windows);
        SDL_Quit();
        return 1;
    }
    Corona::Systems::VulkanBackend backend(main_window);
    if (!backend.initialize()) {
        std::cerr << "UiMultiSurfaceSmoke skipped: VulkanBackend initialization failed\n";
        display.shutdown();
        destroy_windows(windows);
        kernel.shutdown();
        SDL_Quit();
        return kSkip;
    }

    // Exercise the real DisplaySystem lifecycle as part of the smoke target. The
    // main surface uses the production KernelContext/EventBus path; secondary
    // registration and removal acknowledgements remain a follow-up integration
    // step, while this test stays CEF-free.
    display.update();
    // Before the first submission, both producer and consumer receipts are empty.
    // Starting a frame and resizing must not try to wait on those receipts.
    backend.new_frame(backend.main_surface());
    backend.rebuild(backend.main_surface(), 400, 300);
    if (backend.surface_width(backend.main_surface()) != 400 ||
        backend.surface_height(backend.main_surface()) != 300) {
        std::cerr << "UiMultiSurfaceSmoke failed resizing before the first submission\n";
        return 1;
    }

    const auto render_solid = [&](void* surface, uint32_t width, uint32_t height) {
        Corona::Systems::QuadDraw quad;
        quad.dest_min = ktm::fvec2(0.0f, 0.0f);
        quad.dest_max = ktm::fvec2(static_cast<float>(width), static_cast<float>(height));
        quad.color = ktm::fvec4(0.12f, 0.28f, 0.72f, 1.0f);
        const std::array<Corona::Systems::QuadDraw, 1> quads{quad};
        backend.render_quads(surface, quads, width, height);
    };

    // Cover 1, 3 and 16 windows, including a same-frame registration burst.
    for (const int requested : {1, 3, 16}) {
        std::vector<void*> surfaces;
        for (int i = 1; i < requested; ++i) {
            auto* window = SDL_CreateWindow("Corona UI secondary", 160, 120,
                                            SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
            if (!window) {
                std::cerr << "UiMultiSurfaceSmoke failed creating secondary: " << SDL_GetError() << '\n';
                display.shutdown();
                backend.shutdown();
                destroy_windows(windows);
                SDL_Quit();
                return 1;
            }
            windows.push_back(window);
            const auto properties = SDL_GetWindowProperties(window);
            void* native_surface = static_cast<void*>(SDL_GetPointerProperty(
                properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
            if (!native_surface) {
                native_surface = window;
            }
            if (!backend.register_surface(native_surface, window)) {
                std::cerr << "UiMultiSurfaceSmoke failed registering secondary surface\n";
                display.shutdown();
                backend.shutdown();
                destroy_windows(windows);
                SDL_Quit();
                return 1;
            }
            surfaces.push_back(native_surface);
        }

        for (void* surface : surfaces) {
            backend.new_frame(surface);
            backend.rebuild(surface, 160, 120);
            render_solid(surface, 160, 120);
            backend.present_surface(surface);
        }
        backend.new_frame(backend.main_surface());
        backend.rebuild(backend.main_surface(), 320, 240);
        render_solid(backend.main_surface(), 320, 240);
        backend.present_surface(backend.main_surface());
        display.update();

        for (void* surface : surfaces) backend.unregister_surface(surface);
        while (windows.size() > 1) {
            SDL_DestroyWindow(windows.back());
            windows.pop_back();
        }
    }

    // Resize/minimize/restore path on the main window.
    SDL_SetWindowSize(main_window, 640, 480);
    backend.rebuild(backend.main_surface(), 640, 480);
    render_solid(backend.main_surface(), 640, 480);
    SDL_MinimizeWindow(main_window);
    SDL_RestoreWindow(main_window);
    backend.rebuild(backend.main_surface(), 320, 240);
    render_solid(backend.main_surface(), 320, 240);

    // Repeated create/destroy catches stale surface/image handles.
    for (int cycle = 0; cycle < 100; ++cycle) {
        auto* window = SDL_CreateWindow("Corona UI cycle", 96, 96,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window) break;
        windows.push_back(window);
        const auto properties = SDL_GetWindowProperties(window);
        void* native_surface = static_cast<void*>(SDL_GetPointerProperty(
            properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!native_surface) native_surface = window;
        if (!backend.register_surface(native_surface, window)) break;
        backend.new_frame(native_surface);
        backend.rebuild(native_surface, 96, 96);
        render_solid(native_surface, 96, 96);
        backend.unregister_surface(native_surface);
        SDL_DestroyWindow(window);
        windows.pop_back();
    }

    // Direct shutdown with three live secondaries.
    std::vector<void*> live_surfaces;
    for (int i = 0; i < 3; ++i) {
        auto* window = SDL_CreateWindow("Corona UI shutdown", 128, 128,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
        if (!window) break;
        windows.push_back(window);
        const auto properties = SDL_GetWindowProperties(window);
        void* native_surface = static_cast<void*>(SDL_GetPointerProperty(
            properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!native_surface) native_surface = window;
        if (backend.register_surface(native_surface, window)) live_surfaces.push_back(native_surface);
    }
    // Retire unrendered surfaces both explicitly and through backend shutdown.
    if (!live_surfaces.empty()) backend.unregister_surface(live_surfaces.back());

    display.shutdown();
    backend.shutdown();
    kernel.shutdown();
    destroy_windows(windows);
    SDL_Quit();
    std::cout << "UiMultiSurfaceSmoke passed: 1/3/16, burst, resize/minimize/restore, 100 cycles, shutdown drain\n";
    return 0;
}

}  // namespace

int main() { return run_smoke(); }
