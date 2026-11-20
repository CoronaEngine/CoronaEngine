#include <SDL3/SDL.h>
#include <process.h>

#include <corona/systems/display/display_system.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/ui/quad_compositor.h>
#include <corona/systems/ui/vulkan_backend.h>
#include <corona/events/display_system_events.h>
#include <corona/shared_data_hub.h>
#include "../../src/systems/common/render_resource_bindings.h"
#include "../../src/systems/display/presentation_surface.h"

#include <cstdlib>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kSkip = 77;

bool verify_surface_format_policy() {
    using namespace Corona::Systems::DisplayDetail;
    const std::array formats{
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    if (choose_presentation_surface_format(formats).format != VK_FORMAT_B8G8R8A8_SRGB ||
        !presentation_surface_encodes_srgb(VK_FORMAT_R8G8B8A8_SRGB)) {
        std::cerr << "Surface format policy: prefer SRGB independent of enumeration order\n";
        return false;
    }
    const std::array unorm{
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    if (choose_presentation_surface_format(unorm).format != VK_FORMAT_R8G8B8A8_UNORM ||
        presentation_surface_encodes_srgb(unorm.front().format)) {
        std::cerr << "Surface format policy: preserve UNORM-only surfaces and shader encoding\n";
        return false;
    }
    const std::array unconstrained{
        VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    const std::array fallback{
        VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT}};
    if (choose_presentation_surface_format(unconstrained).format != VK_FORMAT_B8G8R8A8_SRGB ||
        choose_presentation_surface_format(fallback).format != fallback.front().format) {
        std::cerr << "Surface format policy: preserve unconstrained and first-format fallbacks\n";
        return false;
    }
    try {
        (void)choose_presentation_surface_format({});
    } catch (const std::runtime_error&) {
        return true;
    }
    std::cerr << "Surface format policy: an empty format list must fail\n";
    return false;
}

bool verify_quad_pixels(Corona::Systems::DisplaySystem& display, const std::string& test_case) try {
    namespace Horizon = Corona::Horizon;
    using namespace Corona::Systems;
    constexpr uint32_t width = 32;
    constexpr uint32_t height = 16;
    const bool scene_only = test_case == "scene-only";
    const bool ui_only = test_case == "ui-only";
    const bool shader_srgb = test_case == "shader-srgb";
    const bool shader_unorm = test_case == "shader-unorm";
    const bool direct_shader = shader_srgb || shader_unorm;
    bool encode_output_srgb = shader_unorm;
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
    std::uintptr_t scene_handle = 0;
    Detail::PublishedImage scene_publication;
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
        if (scene_handle != 0) {
            try {
                scene_publication.retire().wait();
                auto& storage = Corona::SharedDataHub::instance().image_storage();
                if (auto image = storage.acquire_write(scene_handle)) {
                    Horizon::HardwareExecutor drain;
                    if (image->submit_receipt.serial != 0) drain.wait_idle(image->submit_receipt);
                    if (image->consumed_receipt.serial != 0) drain.wait_idle(image->consumed_receipt);
                    *image = Corona::ImageDevice{};
                }
                storage.deallocate(scene_handle);
                scene_handle = 0;
            } catch (...) {
                std::cerr << "UI pixel regression: failed to retire scene image\n";
                cleanup_failed = true;
            }
        }
    };
    // Unwind Display and backend resources before the native window, even when
    // texture upload or rendering throws.
    std::unique_ptr<VulkanBackend, decltype(retire_surface)> surface_cleanup(&backend, retire_surface);
    display.update();
    Horizon::HardwareExecutor upload_executor;

    // Optics publishes display-encoded values in a float image. The real
    // Display composite must decode these before the SRGB swapchain encodes
    // its output, including where the UI layer is completely transparent.
    using Pixel = std::array<uint16_t, 4>;
    std::array<Pixel, width * height> scene_pixels;
    scene_pixels.fill({0x3800, 0x3800, 0x3800, 0x3c00});  // binary16 (0.5, 0.5, 0.5, 1).
    Horizon::HardwareImage scene(Horizon::HardwareImageDesc::texture_2d(
        width, height, Horizon::Format::RGBA16_FLOAT,
        Horizon::ImageUsage_Storage | Horizon::ImageUsage_TransferDst | Horizon::ImageUsage_TransferSrc,
        "ui.test.encoded_scene"));
    const auto scene_staging = Horizon::HardwareBuffer::from_bytes(
        std::as_bytes(std::span(scene_pixels)), 1, Horizon::BufferUsage_TransferSrc, "ui.test.scene_upload");
    const auto scene_receipt = upload_executor.stream()
                               << scene.copy_from(scene_staging)
                               << Horizon::commit();
    auto& image_storage = Corona::SharedDataHub::instance().image_storage();
    scene_handle = image_storage.allocate();
    scene_publication = Detail::PublishedImage(scene_handle);
    if (auto image_device = image_storage.acquire_write(scene_handle)) {
        *image_device = Corona::ImageDevice{};
        image_device->image = scene;
        image_device->submit_receipt = scene_receipt;
        image_device->metadata = {1, width, height, 0, 0, width, height};
    } else {
        std::cerr << "UI pixel regression: cannot publish scene image\n";
        return false;
    }
    Corona::Events::OpticsFrameReadyEvent scene_frame{surface, scene_handle, 1, width, height,
                                                     0, 0, width, height};
    scene_frame.published_image = scene_publication;
    if (!ui_only && !direct_shader) {
        Corona::Kernel::KernelContext::instance().event_bus()->publish(scene_frame);
    }

    const std::array<std::byte, 4> blue{std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
    Horizon::HardwareImage texture(Horizon::HardwareImageDesc::texture_2d(
        1, 1, Horizon::Format::SRGBA8_UNORM));
    const auto staging = Horizon::HardwareBuffer::from_bytes(
        blue, 1, Horizon::BufferUsage_TransferSrc, "ui.test.texture_upload");
    const auto upload_receipt = upload_executor.stream()
                                << texture.copy_from(staging)
                                << Horizon::commit();
    // Match CEF's premultiplied BGRA upload and SRGB texture format: opaque
    // gray, approximately 50% black, and fully transparent pixels.
    const std::array<std::byte, 12> cef_pixels{
        std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{128},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    Horizon::HardwareImage cef_texture(Horizon::HardwareImageDesc::texture_2d(
        3, 1, Horizon::Format::SBGRA8_UNORM));
    const auto cef_staging = Horizon::HardwareBuffer::from_bytes(
        cef_pixels, 1, Horizon::BufferUsage_TransferSrc, "ui.test.cef_upload");
    const auto cef_receipt = upload_executor.stream()
                             << cef_texture.copy_from(cef_staging)
                             << Horizon::commit();
    std::array<QuadDraw, 5> quads;
    quads[0].dest_max = ktm::fvec2(16.0f, 16.0f);
    quads[0].color = ktm::fvec4(1.0f, 0.0f, 0.0f, 1.0f);
    quads[1].texture = &texture;
    quads[1].texture_ready = upload_receipt;
    quads[1].dest_min = ktm::fvec2(16.0f, 0.0f);
    quads[1].dest_max = ktm::fvec2(32.0f, 16.0f);
    quads[1].has_clip = true;
    quads[1].clip_rect = ktm::fvec4(24.0f, 0.0f, 32.0f, 16.0f);
    quads[2].texture = &cef_texture;
    quads[2].texture_ready = cef_receipt;
    quads[2].uv_min = quads[2].uv_max = ktm::fvec2(1.0f / 6.0f, 0.5f);
    quads[2].dest_min = ktm::fvec2(16.0f, 0.0f);
    quads[2].dest_max = ktm::fvec2(24.0f, 4.0f);
    quads[3].texture = &cef_texture;
    quads[3].texture_ready = cef_receipt;
    quads[3].uv_min = quads[3].uv_max = ktm::fvec2(0.5f, 0.5f);
    quads[3].dest_min = ktm::fvec2(16.0f, 4.0f);
    quads[3].dest_max = ktm::fvec2(24.0f, 8.0f);
    quads[4].texture = &cef_texture;
    quads[4].texture_ready = cef_receipt;
    quads[4].uv_min = quads[4].uv_max = ktm::fvec2(5.0f / 6.0f, 0.5f);
    quads[4].dest_min = ktm::fvec2(16.0f, 8.0f);
    quads[4].dest_max = ktm::fvec2(24.0f, 16.0f);
    if (direct_shader) {
        // Exercise both actual shader output branches independently of which
        // swapchain formats this GPU supports. The dump captures the float
        // source before presentation, so the target's conversion is excluded.
        surface_cleanup.reset();
        if (cleanup_failed) return false;
        std::array<Pixel, width * height> ui_pixels{};
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                if (x >= 8 && x < 16) ui_pixels[y * width + x] = {0, 0, 0, 0x3800};
                if (x >= 16 && x < 24) ui_pixels[y * width + x] = {0x32e8, 0x32e8, 0x32e8, 0x3c00};
            }
        }
        Horizon::HardwareImage ui(Horizon::HardwareImageDesc::texture_2d(
            width, height, Horizon::Format::RGBA16_FLOAT,
            Horizon::ImageUsage_Storage | Horizon::ImageUsage_TransferDst, "ui.test.linear_ui"));
        Horizon::HardwareImage output(Horizon::HardwareImageDesc::texture_2d(
            width, height, Horizon::Format::RGBA16_FLOAT,
            Horizon::ImageUsage_Storage | Horizon::ImageUsage_TransferSrc, "ui.test.shader_output"));
        const auto ui_staging = Horizon::HardwareBuffer::from_bytes(
            std::as_bytes(std::span(ui_pixels)), 1, Horizon::BufferUsage_TransferSrc, "ui.test.linear_ui_upload");
        const auto ui_receipt = upload_executor.stream() << ui.copy_from(ui_staging) << Horizon::commit();
        Horizon::ComputePipeline<composite_comp_glsl_t> pipeline(composite_comp_glsl, ktm::uvec3(8, 8, 1));
        RenderDetail::bind_storage_image(pipeline, pipeline.pushConsts.bgImage, scene);
        RenderDetail::bind_storage_image(pipeline, pipeline.pushConsts.fgImage, ui);
        RenderDetail::bind_storage_image(pipeline, pipeline.pushConsts.outputImage, output);
        pipeline.pushConsts.outputWidth = width;
        pipeline.pushConsts.outputHeight = height;
        pipeline.pushConsts.bgWidth = width;
        pipeline.pushConsts.bgHeight = height;
        pipeline.pushConsts.fgWidth = width;
        pipeline.pushConsts.fgHeight = height;
        pipeline.pushConsts.bgViewportX = 0;
        pipeline.pushConsts.bgViewportY = 0;
        pipeline.pushConsts.bgViewportWidth = width;
        pipeline.pushConsts.bgViewportHeight = height;
        pipeline.pushConsts.fgOpaque = 0;
        pipeline.pushConsts.encodeOutputSrgb = shader_unorm ? 1u : 0u;
        upload_executor.wait(scene_receipt);
        upload_executor.wait(ui_receipt);
        const auto composite_receipt = upload_executor.stream()
                                       << pipeline.dispatch_extent(width, height) << Horizon::commit();
        upload_executor.wait(composite_receipt);
        Horizon::HardwareDisplayer displayer(DisplayDetail::horizon_native_window(surface));
        const auto present_output = [&] {
            const auto receipt = upload_executor.stream()
                                 << Horizon::present(displayer, output) << Horizon::commit();
            upload_executor.wait_idle(receipt);
        };
        if (shader_srgb) {
            // Exercise real native resize/restore inside FrameHash's capture
            // budget. The last captured source is the restored-size image;
            // pixel assertions below verify it without a fabricated generation.
            present_output();
            SDL_SetWindowSize(window.get(), width + 16, height + 16);
            SDL_PumpEvents();
            const auto resized = DisplayDetail::query_presentation_surface(surface);
            if (resized.extent.width != width + 16 || resized.extent.height != height + 16) {
                std::cerr << "UI pixel regression: native window did not resize\n";
                return false;
            }
            present_output();
            SDL_SetWindowSize(window.get(), width, height);
            SDL_PumpEvents();
            const auto restored = DisplayDetail::query_presentation_surface(surface);
            if (restored.extent.width != width || restored.extent.height != height ||
                restored.format.format != resized.format.format) {
                std::cerr << "UI pixel regression: native window did not restore its extent/format\n";
                return false;
            }
        }
        for (int frame = 0; frame < 5; ++frame) present_output();
    } else {
        for (int frame = 0; frame < 5; ++frame) {
            if (scene_only && frame == 2) SDL_SetWindowSize(window.get(), width + 16, height + 16);
            if (scene_only && frame == 3) SDL_SetWindowSize(window.get(), width, height);
            SDL_PumpEvents();
            if (scene_only && (frame == 2 || frame == 3)) {
                const auto native = DisplayDetail::query_presentation_surface(surface);
                const uint32_t expected_width = frame == 2 ? width + 16 : width;
                const uint32_t expected_height = frame == 2 ? height + 16 : height;
                if (native.extent.width != expected_width || native.extent.height != expected_height) {
                    std::cerr << "UI pixel regression: Display native extent did not resize/restore\n";
                    return false;
                }
            }
            if (!ui_only && !scene_only && frame > 0) {
                // Publish real producer submissions for this constant diagnostic.
                // Reusing one receipt would correctly hit Display's frame cache.
                if (auto image_device = image_storage.acquire_write(scene_handle)) {
                    if (image_device->consumed_receipt.serial != 0) {
                        upload_executor.wait(image_device->consumed_receipt);
                    }
                    image_device->submit_receipt = upload_executor.stream()
                                                   << scene.copy_from(scene_staging)
                                                   << Horizon::commit();
                    image_device->metadata.frame_index = frame + 1;
                } else {
                    std::cerr << "UI pixel regression: cannot publish next scene frame\n";
                    return false;
                }
                scene_frame.frame_index = frame + 1;
                Corona::Kernel::KernelContext::instance().event_bus()->publish(scene_frame);
            }
            // scene-only deliberately retains its original producer receipt:
            // frame 1 exercises cached re-presentation; resize/restore must
            // still produce a correct final pixel capture without a new scene.
            if (!scene_only) {
                backend.new_frame(surface);
                backend.render_quads(surface, quads, width, height);
                backend.present_surface(surface);
            }
            display.update();
        }
    }
    surface_cleanup.reset();
    if (cleanup_failed) return false;
    if (!direct_shader) {
        const auto presentation = DisplayDetail::query_presentation_surface(surface);
        encode_output_srgb = !DisplayDetail::presentation_surface_encodes_srgb(presentation.format.format);
    }
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
    // IEC 61966-2-1: encoded 0.5 corresponds to linear 0.2140411405;
    // use its precomputed binary16 value, not a CPU copy of the shader math.
    constexpr uint16_t linear_gray = 0x32d9;
    const auto expect_gray = [&](uint32_t x, uint32_t y, uint16_t expected, const char* label) {
        const auto& actual = pixels[y * width + x];
        const bool matches = actual[3] == one &&
            actual[0] >= expected - 1 && actual[0] <= expected + 1 &&
            actual[1] >= expected - 1 && actual[1] <= expected + 1 &&
            actual[2] >= expected - 1 && actual[2] <= expected + 1;
        if (!matches) {
            std::cerr << "UI pixel regression " << test_case << ": " << label
                      << " expected half bits " << expected << " (+/-1 ULP), got "
                      << actual[0] << "," << actual[1] << "," << actual[2] << "," << actual[3] << '\n';
        } else {
            std::cout << "UI pixel regression " << test_case << ": " << label
                      << " verified half bits " << actual[0] << " (expected " << expected << ")\n";
        }
        return matches;
    };
    if (direct_shader) {
        // Precomputed values: linear scene .2140411405, half-covered scene
        // .1070205703, and decoded UI byte 128 .2158605001. Their encoded
        // counterparts are .5, .3607802138 and .5019607843 respectively.
        return expect_gray(4, 8, shader_unorm ? 0x3800 : linear_gray, "transparent UI") &&
               expect_gray(12, 8, shader_unorm ? 0x35c6 : 0x2ed9, "50% black UI") &&
               expect_gray(20, 8, shader_unorm ? 0x3804 : 0x32e8, "opaque gray UI");
    }
    if (scene_only) return expect_gray(20, 8, encode_output_srgb ? 0x3800 : linear_gray, "scene without UI");
    const bool scene_linear = ui_only ? expect_pixel(20, 8, {0, 0, 0, one})
                                     : expect_gray(20, 8, encode_output_srgb ? 0x3800 : linear_gray,
                                                   "scene through transparent UI");
    return expect_pixel(4, 12, {one, 0, 0, one}) &&
           expect_pixel(12, 4, {one, 0, 0, one}) &&
           expect_pixel(26, 12, {0, 0, one, one}) &&
           expect_pixel(30, 2, {0, 0, one, one}) &&
           expect_gray(20, 2, encode_output_srgb ? 0x3804 : 0x32e8, "SRGB UI byte 128") &&
           (ui_only ? expect_pixel(20, 6, {0, 0, 0, one})
                    : expect_gray(20, 6, encode_output_srgb ? 0x35c3 : 0x2ed3, "CEF black alpha 128/255")) &&
           scene_linear;
} catch (const std::exception& error) {
    std::cerr << "UI pixel regression setup failed: " << error.what() << '\n';
    return false;
}

void destroy_windows(std::vector<SDL_Window*>& windows) {
    for (auto* window : windows) {
        if (window) SDL_DestroyWindow(window);
    }
    windows.clear();
}

int run_smoke(const std::string& pixel_case = "combined", bool lifecycle = true) {
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

    // Registration supplies SystemBase's context, which is used when Display
    // acknowledges consumed Optics frames. Keep updates synchronous in this test.
    auto display_system = std::make_shared<Corona::Systems::DisplaySystem>();
    kernel.system_manager()->register_system(display_system);
    auto& display = *display_system;
    if (!kernel.system_manager()->initialize_all()) {
        std::cerr << "UiMultiSurfaceSmoke failed: DisplaySystem initialization failed\n";
        destroy_windows(windows);
        kernel.shutdown();
        SDL_Quit();
        return 1;
    }
    const bool pixels_passed = verify_quad_pixels(display, pixel_case);
    if (!pixels_passed || !lifecycle) {
        display.shutdown();
        kernel.shutdown();
        destroy_windows(windows);
        SDL_Quit();
        if (pixels_passed) std::cout << "UI pixel regression passed: " << pixel_case << '\n';
        return pixels_passed ? 0 : 1;
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

int main(int argc, char** argv) {
    if (!verify_surface_format_policy()) return 1;
    if (argc == 2 && std::string(argv[1]) == "--surface-format-policy") return 0;
    if (argc == 3 && std::string(argv[1]) == "--display-pixels") return run_smoke(argv[2], false);
    if (const char* enabled = std::getenv("CORONA_RUN_GPU_SMOKE"); enabled && std::string(enabled) == "1") {
        // FrameHash has a per-process capture budget. Each child executes the
        // real shader and readback with a fresh budget; one smoke invocation
        // covers all cases without requiring extra CMake targets or settings.
        for (const char* pixel_case : {"ui-only", "scene-only", "shader-srgb", "shader-unorm"}) {
            const auto result = _spawnl(_P_WAIT, argv[0], argv[0], "--display-pixels", pixel_case, nullptr);
            if (result != 0) return result == -1 ? 1 : static_cast<int>(result);
        }
    }
    return run_smoke();
}
