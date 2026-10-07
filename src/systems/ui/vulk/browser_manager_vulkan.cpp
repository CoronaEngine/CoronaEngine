#include <corona/systems/ui/vulkan_backend.h>
#include <horizon/core/logging.h>

#include <algorithm>
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

    {
        std::unique_lock<std::mutex> lock(tab->mutex);
        if (!(tab->buffer_dirty && !tab->pixel_buffer.empty() && is_valid_texture_id(tab->texture_id))) {
            return;
        }

        texture_id = tab->texture_id;
        pixels.swap(tab->pixel_buffer);
        tab->buffer_dirty = false;

        if (tab->popup.visible()) {
            const int view_width = tab->width;
            const int view_height = view_width > 0
                                        ? static_cast<int>(pixels.size() /
                                                           (static_cast<std::size_t>(view_width) * 4u))
                                        : 0;
            tab->popup.composite_over(pixels, view_width, view_height);
        }
    }

    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return;
    }

    constexpr size_t kRgbaBytesPerPixel = 4;
    const size_t expected_size =
        static_cast<size_t>(image_it->second.width) *
        static_cast<size_t>(image_it->second.height) *
        kRgbaBytesPerPixel;

    if (pixels.size() >= expected_size) {
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

    tab->width = width;
    tab->height = height;

    {
        std::lock_guard<std::mutex> lock(tab->mutex);
        tab->popup.clear();
        tab->buffer_dirty = false;
    }

    destroy_tab_texture(tab);
    tab->texture_id = create_browser_texture(tab->width, tab->height);

    if (tab->client) {
        tab->client->Resize(tab->width, tab->height);
    }
}
}  // namespace Corona::Systems::UI
