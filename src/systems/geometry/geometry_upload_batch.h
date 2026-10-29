#pragma once

#include <horizon.h>
#include <functional>
#include <vector>

namespace Corona::Systems::GeometryDetail {

// Keep staging and destination resources alive until this batch's timeline
// completes. Waiting for queue idle here would also wait for unrelated renders.
class UploadBatch {
public:
    void copy_buffer(Horizon::HardwareBuffer source, Horizon::HardwareBuffer target,
                     std::size_t bytes) {
        copies_.emplace_back([source, target, bytes](Horizon::HardwareStream& stream) {
            stream << Horizon::CopyBufferCommand{source, target, 0, 0, bytes};
        });
        account(bytes);
    }

    void copy_image(Horizon::HardwareBuffer source, Horizon::HardwareImage target,
                    std::size_t bytes) {
        copies_.emplace_back([source, target](Horizon::HardwareStream& stream) {
            stream << target.copy_from(source);
        });
        account(bytes);
    }

    void finish() {
        if (copies_.empty()) return;
        auto stream = executor_.stream();
        for (const auto& copy : copies_) copy(stream);
        const auto receipt = stream << Horizon::commit();
        executor_.wait_completed(receipt);
        copies_.clear();
        bytes_ = 0;
    }

private:
    void account(std::size_t bytes) {
        bytes_ += bytes;
        // Bound staging memory even for a single mesh with several large maps.
        if (bytes_ >= 32 * 1024 * 1024) finish();
    }
    Horizon::HardwareExecutor executor_;
    std::vector<std::function<void(Horizon::HardwareStream&)>> copies_;
    std::size_t bytes_ = 0;
};

} // namespace Corona::Systems::GeometryDetail
