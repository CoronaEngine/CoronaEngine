#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <horizon.h>
#include "gpu_performance.h"

namespace Corona::Systems {

class GpuSubmissionBatch;

// A pending device view is usable only by the batch that recorded its producer.
// Shared ownership also keeps this guard alive when a pipeline is destroyed
// before its batch, without leaving dangling callbacks into the pipeline.
struct GpuSubmissionState {
    bool poisoned{};
    GpuSubmissionBatch* owner{};

    void check(GpuSubmissionBatch* expected = nullptr) const {
        if (poisoned) throw std::runtime_error("GPU submission state is unconfirmed");
        if (owner && owner != expected)
            throw std::logic_error("GPU submission state belongs to a pending batch");
    }
};

// Records dependent compute/copy work until a CPU consumer needs its result.
// Callers must keep host upload contents immutable until flush() returns and
// retain their kernel objects through recording and execution. Horizon retains
// the buffer resources referenced by the recorded commands/submission.
class GpuSubmissionBatch {
   public:
    GpuSubmissionBatch() = default;
    GpuSubmissionBatch(const GpuSubmissionBatch&) = delete;
    GpuSubmissionBatch& operator=(const GpuSubmissionBatch&) = delete;
    GpuSubmissionBatch(GpuSubmissionBatch&&) = delete;
    GpuSubmissionBatch& operator=(GpuSubmissionBatch&&) = delete;

    ~GpuSubmissionBatch() {
        // Never execute abandoned commands implicitly: CPU publication may
        // already have failed or shutdown may be in progress.
        abandon();
        if (receipt_.data) {
            try { executor_.wait_completed(receipt_); } catch (...) {}
        }
    }

    [[nodiscard]] Horizon::HardwareStream& record(std::shared_ptr<GpuSubmissionState> state) {
        if (failed_) throw std::runtime_error("GPU submission batch is unconfirmed");
        if (!state) throw std::invalid_argument("GPU submission batch requires an ownership state");
        state->check(this);
        if (!stream_) stream_ = std::make_unique<Horizon::HardwareStream>(executor_);
        if (state->owner != this) {
            // Allocate before claiming ownership so allocation failure cannot
            // leave a state referring to a batch that does not track it.
            participants_.push_back(state);
            state->owner = this;
        }
        return *stream_;
    }

    // Every upload occupies its own slot until GPU completion. Subsequent
    // flushes reuse slot capacities, avoiding both in-flight staging writes
    // and per-step buffer allocation for a stable workload.
    [[nodiscard]] Horizon::HardwareBuffer upload(std::span<const std::byte> bytes) {
        if (failed_) throw std::runtime_error("GPU submission batch is unconfirmed");
        if (!stream_ || participants_.empty())
            throw std::logic_error("GPU submission upload requires a recorded participant");
        for (const auto& state : participants_) state->check(this);
        if (bytes.empty()) return {};
        if (upload_cursor_ == uploads_.size()) uploads_.emplace_back();
        auto& slot = uploads_[upload_cursor_];
        if (!slot || slot.get_byte_size() < bytes.size()) {
            const auto size = std::max<std::uint64_t>(256, bytes.size());
            if (size > std::numeric_limits<std::uint64_t>::max() / 2)
                throw std::length_error("GPU submission upload is too large");
            auto desc = Horizon::HardwareBufferDesc::typed<std::byte>(
                std::bit_ceil(size), Horizon::BufferUsage_TransferSrc, "Mechanics.batch_upload");
            desc.cpu_access = Horizon::CpuAccessMode::Write;
            slot = Horizon::HardwareBuffer(desc);
        }
        if (!slot.write_bytes(bytes)) throw std::runtime_error("GPU submission batch upload failed");
        ++upload_cursor_;
        return slot;
    }

    // True means a recorded batch was committed and waited for. A successful
    // flush leaves this object reusable for the next CPU-consumption boundary.
    [[nodiscard]] bool flush() {
        if (failed_) throw std::runtime_error("GPU submission batch is unconfirmed");
        if (!stream_) return false;
        GpuPerformance::Scope performance{"batch.flush"};
        try {
            for (const auto& state : participants_) state->check(this);
            {
                GpuPerformance::OperationScope timing{GpuPerformance::Operation::Commit};
                receipt_ = *stream_ << Horizon::commit();
            }
            ++submissions_;
            ++waits_;
            {
                GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
                executor_.wait_completed(receipt_);
            }
            receipt_ = {};
            stream_.reset();
            release_participants(false);
            upload_cursor_ = 0;
            return true;
        } catch (...) {
            // Recording guards may poison just one participant. Since the
            // state transition is shared, every participant becomes unusable.
            failed_ = true;
            release_participants(true);
            stream_.reset();
            // Keep a successfully returned receipt for the destructor to retry
            // completion if the first wait failed.
            throw;
        }
    }

    [[nodiscard]] std::uint64_t submission_count() const noexcept { return submissions_; }
    [[nodiscard]] std::uint64_t wait_count() const noexcept { return waits_; }

    // A step guard invokes this on an early return or exception. A completed
    // batch is untouched; abandoning pending state permanently rejects reuse.
    void abandon() noexcept {
        if (!stream_ && participants_.empty()) return;
        failed_ = true;
        release_participants(true);
        stream_.reset();
    }

   private:
    void release_participants(bool poison) noexcept {
        for (const auto& state : participants_) {
            if (poison) state->poisoned = true;
            state->owner = nullptr;
        }
        participants_.clear();
    }

    Horizon::HardwareExecutor executor_;
    std::unique_ptr<Horizon::HardwareStream> stream_;
    Horizon::SubmitReceipt receipt_;
    std::vector<std::shared_ptr<GpuSubmissionState>> participants_;
    std::vector<Horizon::HardwareBuffer> uploads_;
    std::size_t upload_cursor_{};
    std::uint64_t submissions_{}, waits_{};
    bool failed_{};
};

}  // namespace Corona::Systems
