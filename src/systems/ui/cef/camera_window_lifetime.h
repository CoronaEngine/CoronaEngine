#pragma once

#include <memory>
#include <mutex>
#include <utility>

namespace Corona::Systems::UI {

// Native identity lease. A queued window operation holds the lock while using
// an engine handle; scene replacement/deletion retires it before destroying it.
struct CameraWindowAuthority {
    std::mutex mutex;
    bool active = true;
};

class CameraWindowLifetime {
public:
    CameraWindowLifetime() = default;
    CameraWindowLifetime(const CameraWindowLifetime&) = delete;
    CameraWindowLifetime& operator=(const CameraWindowLifetime&) = delete;
    CameraWindowLifetime(CameraWindowLifetime&&) noexcept = default;
    CameraWindowLifetime& operator=(CameraWindowLifetime&& other) noexcept {
        if (this != &other) { retire(); authority_ = std::move(other.authority_); }
        return *this;
    }
    ~CameraWindowLifetime() { retire(); }

    std::shared_ptr<CameraWindowAuthority> token() const { return authority_; }
    void retire() {
        const auto state = std::exchange(authority_, {});
        if (!state) return;
        std::lock_guard lock(state->mutex);
        state->active = false;
    }

private:
    std::shared_ptr<CameraWindowAuthority> authority_ = std::make_shared<CameraWindowAuthority>();
};

}  // namespace Corona::Systems::UI
