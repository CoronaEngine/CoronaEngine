// Exercise the real Optics shutdown path with a CPU-only Device handle/deleter.
// No backend plugin or GPU device is created by this test.
#include <corona/systems/optics/optics_system.h>
#include "base/mgr/global.h"
#include "rhi/device.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace Corona::Systems {
struct VisionDeviceLifetimeTest {
    static inline std::vector<std::string> events;
    static inline int handle_token;

    static void require(bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    static void destroy_device(ocarina::Device::Impl* handle) {
        require(handle == reinterpret_cast<ocarina::Device::Impl*>(&handle_token),
                "unexpected device deleter handle");
        events.emplace_back("device");
    }

    static void attach_device(OpticsSystem& optics, bool initialized) {
        // Opaque sentinel: only stored and passed to our deleter, never dereferenced.
        optics.vision_device_ = std::make_unique<ocarina::Device>(
            ocarina::Device::Handle{reinterpret_cast<ocarina::Device::Impl*>(&handle_token), &destroy_device});
        optics.vision_initialized_ = initialized;
        vision::Global::instance().set_device(optics.vision_device_.get());
        auto resource = std::shared_ptr<Vision::VisionSceneResource>(
            new Vision::VisionSceneResource{}, [](auto* value) {
                events.emplace_back("scene");
                delete value;
            });
        optics.vision_scene_resource_ = std::move(resource);
    }

    static void run() {
        OpticsSystem optics;
        optics.shutdown_vision_backend();
        require(events.empty(), "unused Vision should not create or destroy a device");

        attach_device(optics, true);
        // Scene unload must release dependent resources but keep the device reusable.
        optics.clear_vision_runtimes();
        require(events == std::vector<std::string>{"scene"} && optics.vision_device_,
                "scene unload destroyed the reusable compute device");
        optics.shutdown_vision_backend();
        require(events == std::vector<std::string>{"scene", "device"},
                "shutdown did not explicitly destroy the device after scene resources");
        require(!optics.vision_device_ && !optics.vision_initialized_, "shutdown left stale owner state");
        optics.shutdown_vision_backend();
        require(events.size() == 2, "repeated shutdown double-destroyed the device");

        events.clear();
        attach_device(optics, false); // Device creation succeeded but scene initialization failed.
        optics.shutdown_vision_backend();
        require(events == std::vector<std::string>{"scene", "device"},
                "partial initialization leaked the device or released it before dependents");
        require(!optics.vision_device_ && !optics.vision_initialized_, "partial shutdown left stale state");
        std::cout << "Vision device lifetime CPU tests passed\n";
    }
};
}

int main() {
    Corona::Systems::VisionDeviceLifetimeTest::run();
}
