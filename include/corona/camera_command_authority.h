#pragma once

#include <mutex>

namespace Corona {

// Shared by the scene owner and deferred native camera commands. Retirement
// waits for an in-progress command before the owner can release camera storage.
struct CameraCommandAuthority {
    std::mutex mutex;
    bool active = true;
};

}  // namespace Corona
