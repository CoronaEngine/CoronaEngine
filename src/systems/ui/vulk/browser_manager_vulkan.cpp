#include <corona/systems/ui/vulkan_backend.h>
#include <corona/kernel/core/i_logger.h>

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

ImTextureID descriptor_to_texture_id(uint32_t descriptor) {
    return static_cast<ImTextureID>(static_cast<ImU64>(descriptor) + 1u);
}
}  // namespace

void BrowserManager::destroy_tab_texture(BrowserTab* tab) {
    if (!tab || !is_valid_texture_id(tab->texture_id)) {
        return;
    }

    const ImTextureID texture_id = tab->texture_id;
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

ImTextureID BrowserManager::create_browser_texture(int width, int height) {
    const uint32_t safe_width = static_cast<uint32_t>(std::max(width, 1));
    const uint32_t safe_height = static_cast<uint32_t>(std::max(height, 1));

    OwnedImage owned{};
    owned.image = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        safe_width,
        safe_height,
        Horizon::Format::SRGBA8_UNORM,
        Horizon::ImageUsageFlags::Sampled | Horizon::ImageUsageFlags::TransferDst | Horizon::ImageUsageFlags::TransferSrc,
        "cef.browser_texture"));
    if (!owned.image) {
        return k_invalid_texture_id;
    }

    owned.width = safe_width;
    owned.height = safe_height;

    const std::vector<uint8_t> transparent_pixels(
        static_cast<size_t>(safe_width) * static_cast<size_t>(safe_height) * 4u,
        0u);
    owned.upload_receipt =
        browser_upload_executor_.stream()
        << owned.image.upload(std::as_bytes(std::span<const uint8_t>(transparent_pixels.data(),
                                                                     transparent_pixels.size())))
        << Horizon::commit();

    const uint32_t descriptor = owned.image.storeSampledDescriptor();
    const ImTextureID texture_id = descriptor_to_texture_id(descriptor);

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
    ImTextureID texture_id = k_invalid_texture_id;

    {
        std::unique_lock<std::mutex> lock(tab->mutex);
        if (!(tab->buffer_dirty && !tab->pixel_buffer.empty() && is_valid_texture_id(tab->texture_id))) {
            return;
        }

        texture_id = tab->texture_id;
        pixels.swap(tab->pixel_buffer);
        tab->buffer_dirty = false;
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
        owned.upload_receipt =
            browser_upload_executor_.stream()
            << owned.image.upload(std::as_bytes(std::span<const uint8_t>(pixels.data(), expected_size)))
            << Horizon::commit();
    }
}

const Horizon::HardwareImage* BrowserManager::get_texture_image(ImTextureID texture_id) const {
    auto image_it = owned_images_.find(texture_id);
    if (image_it == owned_images_.end()) {
        return nullptr;
    }
    return &image_it->second.image;
}

void BrowserManager::wait_for_texture_upload(ImTextureID texture_id) {
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

    destroy_tab_texture(tab);
    tab->texture_id = create_browser_texture(tab->width, tab->height);

    if (tab->client) {
        tab->client->Resize(tab->width, tab->height);
    }
}
}  // namespace Corona::Systems::UI
