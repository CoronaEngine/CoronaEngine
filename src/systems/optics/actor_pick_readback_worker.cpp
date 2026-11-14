#include <corona/systems/optics/actor_pick_readback_worker.h>

#include <utility>

namespace Corona::Systems::OpticsDetail {

using Corona::Concurrency::TaskClass;

ActorPickReadbackWorker::Reservation::Reservation(ActorPickReadbackWorker* owner,
                                                  std::uint64_t id) noexcept
    : owner_(owner), id_(id) {}

ActorPickReadbackWorker::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), id_(std::exchange(other.id_, 0)) {}

ActorPickReadbackWorker::Reservation& ActorPickReadbackWorker::Reservation::operator=(
    Reservation&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

ActorPickReadbackWorker::Reservation::~Reservation() {
    reset();
}

void ActorPickReadbackWorker::Reservation::reset() noexcept {
    if (owner_ != nullptr && id_ != 0) {
        owner_->cancel(id_);
    }
    owner_ = nullptr;
    id_ = 0;
}

ActorPickReadbackWorker::ActorPickReadbackWorker(std::size_t capacity)
    : capacity_(capacity) {}

ActorPickReadbackWorker::~ActorPickReadbackWorker() {
    shutdown();
}

void ActorPickReadbackWorker::start() {
    std::lock_guard lock(mutex_);
    accepting_ = true;
}

std::optional<ActorPickReadbackWorker::Reservation> ActorPickReadbackWorker::reserve() {
    std::lock_guard lock(mutex_);
    if (!accepting_ || occupied_ >= capacity_) {
        return std::nullopt;
    }

    const std::uint64_t id = next_reservation_id_++;
    reservations_.insert(id);
    ++occupied_;
    return Reservation(this, id);
}

bool ActorPickReadbackWorker::submit(Reservation&& reservation, Work work) {
    if (!work) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        if (!accepting_ || reservation.owner_ != this || reservation.id_ == 0 ||
            !reservations_.contains(reservation.id_)) {
            return false;
        }
        // 槽位所有权移交给任务：任务结束时归还，Reservation 不再负责取消。
        reservations_.erase(reservation.id_);
        reservation.owner_ = nullptr;
        reservation.id_ = 0;
    }

    auto* worker = this;
    const bool enqueued = in_flight_.submit(
        TaskClass::Realtime, [worker, work = std::move(work)]() mutable {
            try {
                work();
            } catch (...) {
                // GPU/回读任务自身会把失败翻译成完成回调；这里只保证不杀死 worker。
            }
            std::lock_guard lock(worker->mutex_);
            if (worker->occupied_ > 0) --worker->occupied_;
        });

    if (!enqueued) {
        // 线程池已停止：立即归还槽位，避免容量永久泄漏。
        std::lock_guard lock(mutex_);
        if (occupied_ > 0) --occupied_;
    }
    return enqueued;
}

void ActorPickReadbackWorker::shutdown() {
    {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        occupied_ = occupied_ > reservations_.size() ? occupied_ - reservations_.size() : 0;
        reservations_.clear();
    }
    // 等已提交任务结束：它们可能正卡在 GPU 时间线上，不能带着悬空 this 退出。
    in_flight_.wait();
    std::lock_guard lock(mutex_);
    occupied_ = 0;
}

std::size_t ActorPickReadbackWorker::pending_count() const noexcept {
    std::lock_guard lock(mutex_);
    return occupied_;
}

void ActorPickReadbackWorker::cancel(std::uint64_t id) noexcept {
    std::lock_guard lock(mutex_);
    if (reservations_.erase(id) == 1) {
        if (occupied_ > 0) --occupied_;
    }
}

}  // namespace Corona::Systems::OpticsDetail
