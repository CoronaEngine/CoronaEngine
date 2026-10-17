#include <corona/systems/ui/cef_paint_upload.h>
#include <corona/systems/ui/vulkan_backend.h>
#include <horizon/core/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "cef/browser_manager.h"
#include "cef/cef_client.h"

namespace Corona::Systems::UI {
namespace {
constexpr uint64_t kDeferredTextureDestroyFrames = 4;

UiTextureId descriptor_to_texture_id(uint32_t descriptor) {
    return static_cast<UiTextureId>(static_cast<std::uint64_t>(descriptor) + 1u);
}

// Horizon 移除了 HardwareImage::upload()。上传改为 staging buffer + copy_from()，
// 每帧新建一块 staging —— 这一点是刻意的，原因都写在这里，改动前请先读：
//
// 1) 生命周期不需要 App 操心。copy_from() 会把源 buffer 按值存进
//    CopyBufferToImageCommand（hardware_image.cpp 里的 `{ src, *this, ... }`），
//    而 HardwareBuffer 是 ResourceHandle，内部持有 shared_ptr<IResourceRef>。
//    因此提交之后命令自己就持有这块 buffer，这里的局部 shared_ptr 析构并不会让它
//    提前消失。Horizon 已无 keep_alive 之类的 API，也不需要。
//
// 2) 反过来，复用同一块常驻 staging 是不安全的。HardwareExecutor::wait() 并不等待：
//    它只把 receipt 的 token 登记为「下一次 commit 要在 GPU 侧等待」
//    （execution.cpp: wait() 只往 pending_waits_ 里 push，随即返回），
//    真正阻塞主机的只有 wait_idle()（queue->wait_idle()，会等整条队列空闲）。
//    所以「wait 一下再重写同一块 buffer」并不能阻止 CPU 在 GPU 仍读它时写入
//    （write_bytes() 就是裸 memcpy），结果是画面撕裂 —— 这正是一次实测回归的成因。
//    要安全复用，必须做 K 深 ring + wait_idle（见 P1-2/P1-3 的设计）。
Horizon::SubmitReceipt upload_image_async(Horizon::HardwareExecutor& executor,
                                         Horizon::HardwareImage& image,
                                         std::span<const std::byte> bytes) {
    Horizon::HardwareBufferDesc staging_desc;
    staging_desc.element_count = bytes.size_bytes();
    staging_desc.element_size = 1;
    staging_desc.usage = Horizon::BufferUsage_TransferSrc;
    staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
    auto staging = std::make_shared<Horizon::HardwareBuffer>(staging_desc, bytes);

    return executor.stream()
        << image.copy_from(*staging)
        << Horizon::commit();
}
}  // namespace

void BrowserManager::destroy_tab_texture(BrowserTab* tab) {
    if (!tab || !is_valid_texture_id(tab->texture_id)) {
        return;
    }

    const UiTextureId texture_id = tab->texture_id;
    tab->texture_id = k_invalid_texture_id;

    auto it = owned_images_.find(texture_id);
    if (it != owned_images_.end()) {
        deferred_texture_destroys_.push_back(
            DeferredTextureDestroy{std::move(it->second), frame_index_});
        owned_images_.erase(texture_id);
    }
}

void BrowserManager::retire_deferred_tab_textures(bool force) {
    if (deferred_texture_destroys_.empty()) {
        return;
    }

    std::erase_if(
        deferred_texture_destroys_,
        [this, force](DeferredTextureDestroy& pending) {
            if (!force &&
                frame_index_ < pending.queued_frame + kDeferredTextureDestroyFrames) {
                return false;
            }
            browser_upload_executor_.wait_idle(pending.image.upload_receipt);
            return true;
        });
}

UiTextureId BrowserManager::create_browser_texture(int width, int height) {
    const uint32_t safe_width = static_cast<uint32_t>(std::max(width, 1));
    const uint32_t safe_height = static_cast<uint32_t>(std::max(height, 1));

    OwnedImage owned{};
    // CEF hands over BGRA; keep that byte order end to end and let the sampler read it as
    // B8G8R8A8_SRGB (resource_manager.cpp) instead of paying a per-pixel swap on the CPU.
    // Changing this format without changing cef_client.cpp / popup_overlay.cpp swaps colours:
    // tests/systems/ui/test_cef_texture_channel_order.py pins all three together.
    owned.image = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        safe_width,
        safe_height,
        Horizon::Format::SBGRA8_UNORM,
        Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst | Horizon::ImageUsage_TransferSrc,
        "cef.browser_texture"));
    if (!owned.image) {
        return k_invalid_texture_id;
    }

    owned.width = safe_width;
    owned.height = safe_height;

    const std::vector<uint8_t> transparent_pixels(
        static_cast<size_t>(safe_width) * static_cast<size_t>(safe_height) * 4u,
        0u);
    owned.upload_receipt = upload_image_async(
        browser_upload_executor_,
        owned.image,
        std::as_bytes(std::span<const uint8_t>(transparent_pixels.data(),
                                               transparent_pixels.size())));

    const uint32_t descriptor = owned.image.store_descriptor();
    const UiTextureId texture_id = descriptor_to_texture_id(descriptor);

    owned_images_[texture_id] = std::move(owned);
    return texture_id;
}

void BrowserManager::update_texture(int tab_id) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) {
        return;
    }

    BrowserTab* tab = it->second.get();

    std::vector<uint8_t> pixels;
    UiTextureId texture_id = k_invalid_texture_id;
    int paint_width = 0;
    int paint_height = 0;
    int requested_width = 0;
    int requested_height = 0;

    {
        std::unique_lock<std::mutex> lock(tab->mutex);
        if (!is_valid_texture_id(tab->texture_id) ||
            !compose_pending_popup_frame(tab->pixel_buffer, tab->paint_width,
                                         tab->paint_height, tab->popup,
                                         tab->buffer_dirty, pixels)) {
            return;
        }

        texture_id = tab->texture_id;
        paint_width = tab->paint_width;
        paint_height = tab->paint_height;
        requested_width = tab->width;
        requested_height = tab->height;
    }

    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return;
    }

    const PaintUploadAction action =
        decide_paint_upload(paint_width, paint_height,
                            static_cast<int>(image_it->second.width),
                            static_cast<int>(image_it->second.height),
                            requested_width, requested_height);

    if (action == PaintUploadAction::RefuseStalePaint) {
        // Uploading this buffer would consume it at the texture's row length and shear every row
        // by (paint_width - texture_width) pixels (measured: +32 / -22 / -81 px during a panel
        // edge drag). Drop it and ask CEF for a paint at the size we requested.
        static std::atomic<std::uint64_t> mismatch_frames{0};
        const std::uint64_t mismatch = mismatch_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mismatch == 1 || mismatch % 60 == 0) {
            CFW_LOG_WARNING(
                "[CEF/Upload] refused stale paint: buffer={}x{} texture={}x{} requested={}x{} "
                "(row shift would be {} px); requesting a repaint (count={})",
                paint_width, paint_height, image_it->second.width, image_it->second.height,
                requested_width, requested_height,
                paint_width - static_cast<int>(image_it->second.width), mismatch);
        }
        if (tab->client && tab->client->GetBrowser() && tab->client->GetBrowser()->GetHost()) {
            tab->client->GetBrowser()->GetHost()->Invalidate(PET_VIEW);
        }
        return;
    }

    if (action == PaintUploadAction::RecreateThenUpload) {
        // First paint at the requested size: build the texture at exactly that size now, so the
        // content arrives in the same update instead of leaving a transparent texture on screen.
        // (create_browser_texture seeds it transparent; the paint upload below replaces that
        // before anything is presented, which is the point of deferring the rebuild.)
        const auto recreate_started = std::chrono::steady_clock::now();
        const uint32_t previous_texture_width = image_it->second.width;
        const uint32_t previous_texture_height = image_it->second.height;
        {
            std::lock_guard<std::mutex> lock(tab->mutex);
            destroy_tab_texture(tab);
            tab->texture_id = create_browser_texture(paint_width, paint_height);
            texture_id = tab->texture_id;
        }
        image_it = owned_images_.find(texture_id);
        if (image_it == owned_images_.end()) {
            return;
        }
        const double recreate_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - recreate_started)
                                       .count();
        CFW_LOG_INFO("[CEF/Upload] recreated texture for tab={} {}x{} -> {}x{} cost={:.2f}ms",
                     tab_id, previous_texture_width, previous_texture_height, paint_width,
                     paint_height, recreate_ms);
    }

    constexpr size_t kRgbaBytesPerPixel = 4;
    const size_t expected_size =
        static_cast<size_t>(image_it->second.width) *
        static_cast<size_t>(image_it->second.height) *
        kRgbaBytesPerPixel;

    if (pixels.size() == expected_size) {
        auto& owned = image_it->second;
        browser_upload_executor_.wait(owned.upload_receipt);
        owned.upload_receipt = upload_image_async(
            browser_upload_executor_,
            owned.image,
            std::as_bytes(std::span<const uint8_t>(pixels.data(), expected_size)));
    }
}

const Horizon::HardwareImage* BrowserManager::get_texture_image(UiTextureId texture_id) const {
    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return nullptr;
    }
    return &image_it->second.image;
}

Horizon::SubmitReceipt BrowserManager::get_texture_upload_receipt(UiTextureId texture_id) const {
    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return {};
    }
    return image_it->second.upload_receipt;
}

void BrowserManager::wait_for_texture_upload(UiTextureId texture_id) {
    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return;
    }
    browser_upload_executor_.wait_idle(image_it->second.upload_receipt);
}

void BrowserManager::resize_tab(int tab_id, int width, int height) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) {
        return;
    }

    BrowserTab* tab = it->second.get();
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width == tab->width && height == tab->height) {
        return;
    }

    const int previous_width = tab->width;
    const int previous_height = tab->height;
    const bool was_floating = tab->floating;
    const bool was_camera_view = tab->camera_view;
    const auto resize_started = std::chrono::steady_clock::now();

    tab->width = width;
    tab->height = height;

    {
        std::lock_guard<std::mutex> lock(tab->mutex);
        tab->popup.clear();
        tab->buffer_dirty = true;
    }

    // The texture is NOT rebuilt here any more. CEF still has to relayout the page at the new
    // view size and will answer with a paint of that size; creating a fresh (empty) texture now
    // would show a blank panel for however many frames that takes. Keep the current texture and
    // let update_texture() rebuild it when the matching paint actually arrives - see
    // corona/systems/ui/cef_paint_upload.h. The quad keeps drawing the old texture over the new
    // destination rect in the meantime, which stretches it by at most one coalescing step.
    if (tab->client) {
        tab->client->Resize(tab->width, tab->height);
    }

    // This path used to be completely silent, which is why a lag report about dragging a panel
    // border had no measurable evidence: a floating panel's edge drag reaches here once per
    // changed pixel (ui_frame_runner.cpp apply_floating_resize), and each call throws away the
    // whole CEF texture, clears the popup and forces a full renderer relayout. Log the cost so
    // the churn rate and per-call price can be read straight out of a session log.
    const double resize_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - resize_started)
                                 .count();
    CFW_LOG_INFO("[CEF/Resize] tab={} {}x{} -> {}x{} floating={} camera_view={} cost={:.2f}ms "
                 "(texture rebuild deferred to the matching paint)",
                 tab_id, previous_width, previous_height, tab->width, tab->height, was_floating,
                 was_camera_view, resize_ms);
}
}  // namespace Corona::Systems::UI
