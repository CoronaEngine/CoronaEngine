#pragma once

#include <corona/camera_command_authority.h>

#include <memory>
#include <atomic>
#include <mutex>
#include <utility>

namespace Corona::Systems::UI {

// Per-camera state published by the window owner. -1 retains the persisted
// startup preference until this camera has participated in window lifecycle.
struct CameraWindowState {
    std::atomic<int> requested_open{-1};
};

// Native identity lease. A queued window operation holds the lock while using
// an engine handle; scene replacement/deletion retires it before destroying it.
using CameraWindowAuthority = Corona::CameraCommandAuthority;

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
