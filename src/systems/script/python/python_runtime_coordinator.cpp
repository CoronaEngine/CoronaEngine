#include <corona/systems/script/python_runtime_coordinator.h>

#include <utility>
#include <vector>

namespace Corona::Script::Python {

namespace {
std::atomic<PythonRuntimeCoordinator*> g_active_coordinator{nullptr};
}

namespace Detail {
struct PythonRuntimeTicketState {
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    PythonRuntimeResponse response;
    std::shared_ptr<PythonRuntimeCancellation> cancellation;
};
}  // namespace Detail

namespace {
bool finish_ticket(const std::shared_ptr<Detail::PythonRuntimeTicketState>& ticket,
                   PythonRuntimeResponse response) {
    {
        std::lock_guard lock(ticket->mutex);
        if (ticket->done) return false;
        ticket->done = true;
        ticket->response = std::move(response);
    }
    ticket->cv.notify_all();
    return true;
}
}  // namespace

PythonRuntimeResponse PythonRuntimeResponse::success(std::string payload) {
    return {PythonRuntimeResponseStatus::Success, std::move(payload), {}};
}

PythonRuntimeResponse PythonRuntimeResponse::failure(std::string message) {
    return {PythonRuntimeResponseStatus::Error, {}, std::move(message)};
}

PythonRuntimeResponse PythonRuntimeResponse::timeout() {
    return {PythonRuntimeResponseStatus::Timeout, {}, "python runtime request timed out"};
}

PythonRuntimeResponse PythonRuntimeResponse::queue_full() {
    return {PythonRuntimeResponseStatus::QueueFull, {}, "python runtime queue is full"};
}

PythonRuntimeResponse PythonRuntimeResponse::runtime_stopping() {
    return {PythonRuntimeResponseStatus::RuntimeStopping, {}, "python runtime is stopping"};
}

PythonRuntimeResponse PythonRuntimeTicket::wait(std::chrono::milliseconds timeout) const {
    if (!accepted || !state_) return response;
    std::unique_lock lock(state_->mutex);
    if (!state_->cv.wait_for(lock, timeout, [&] { return state_->done; })) {
        return PythonRuntimeResponse::timeout();
    }
    return state_->response;
}

PythonRuntimeCoordinator::PythonRuntimeCoordinator(std::size_t capacity)
    : capacity_(capacity) {}

PythonRuntimeTicket PythonRuntimeCoordinator::submit(PythonRuntimeRequest request) {
    PythonRuntimeTicket result;
    if (state() != PythonRuntimeState::Accepting) {
        result.response = PythonRuntimeResponse::runtime_stopping();
        return result;
    }

    auto ticket = std::make_shared<Detail::PythonRuntimeTicketState>();
    if (!request.cancellation) {
        request.cancellation = std::make_shared<PythonRuntimeCancellation>(false);
    }
    ticket->cancellation = request.cancellation;
    {
        std::lock_guard lock(mutex_);
        if (state_.load(std::memory_order_relaxed) != PythonRuntimeState::Accepting) {
            result.response = PythonRuntimeResponse::runtime_stopping();
            return result;
        }
        const bool lifecycle_control =
            request.kind == PythonRuntimeRequestKind::LifecycleControl;
        const auto effective_capacity = capacity_ + (lifecycle_control ? 1u : 0u);
        if (pending_.size() >= effective_capacity) {
            result.response = PythonRuntimeResponse::queue_full();
            return result;
        }
        request.request_id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        result.accepted = true;
        result.request_id = request.request_id;
        result.state_ = ticket;
        pending_.emplace(request.request_id, ticket);
        if (lifecycle_control) {
            queue_.push_front(std::move(request));
        } else {
            queue_.push_back(std::move(request));
        }
    }
    queue_cv_.notify_one();
    return result;
}

PythonRuntimeResponse PythonRuntimeCoordinator::submit_and_wait(
    PythonRuntimeRequest request,
    std::chrono::milliseconds timeout) {
    if (is_consumer_thread()) {
        return PythonRuntimeResponse::failure(
            "synchronous python runtime request from the consumer thread would deadlock");
    }
    if (request.deadline == std::chrono::steady_clock::time_point::max()) {
        request.deadline = std::chrono::steady_clock::now() + timeout;
    }
    auto ticket = submit(std::move(request));
    if (!ticket.accepted) return ticket.response;
    auto response = ticket.wait(timeout);
    if (response.status == PythonRuntimeResponseStatus::Timeout) {
        cancel(ticket.request_id, response);
    }
    return response;
}

std::optional<PythonRuntimeRequest> PythonRuntimeCoordinator::wait_pop(
    std::chrono::milliseconds timeout) {
    if (!bind_consumer_thread()) return std::nullopt;
    std::unique_lock lock(mutex_);
    queue_cv_.wait_for(lock, timeout, [&] {
        return !queue_.empty() || state_.load(std::memory_order_relaxed) != PythonRuntimeState::Accepting;
    });
    while (!queue_.empty()) {
        auto request = std::move(queue_.front());
        queue_.pop_front();
        auto pending = pending_.find(request.request_id);
        if (pending == pending_.end()) {
            continue;
        }
        if (request.deadline <= std::chrono::steady_clock::now()) {
            if (request.cancellation) {
                request.cancellation->store(true, std::memory_order_release);
            }
            auto ticket = std::move(pending->second);
            pending_.erase(pending);
            finish_ticket(ticket, PythonRuntimeResponse::timeout());
            continue;
        }
        current_request_ = request;
        return request;
    }
    return std::nullopt;
}

bool PythonRuntimeCoordinator::complete(std::uint64_t request_id,
                                        PythonRuntimeResponse response) {
    std::shared_ptr<Detail::PythonRuntimeTicketState> ticket;
    {
        std::lock_guard lock(mutex_);
        if (current_request_ && current_request_->request_id == request_id) {
            current_request_.reset();
        }
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return false;
        ticket = std::move(it->second);
        pending_.erase(it);
    }
    return finish_ticket(ticket, std::move(response));
}

bool PythonRuntimeCoordinator::cancel(std::uint64_t request_id,
                                      PythonRuntimeResponse response) {
    std::shared_ptr<PythonRuntimeCancellation> cancellation;
    {
        std::lock_guard lock(mutex_);
        const auto it = pending_.find(request_id);
        if (it != pending_.end()) {
            cancellation = it->second->cancellation;
        }
    }
    if (cancellation) {
        cancellation->store(true, std::memory_order_release);
    }
    return complete(request_id, std::move(response));
}

void PythonRuntimeCoordinator::transition_and_cancel(PythonRuntimeState next) {
    std::vector<std::shared_ptr<Detail::PythonRuntimeTicketState>> tickets;
    {
        std::lock_guard lock(mutex_);
        state_.store(next, std::memory_order_release);
        tickets.reserve(pending_.size());
        for (auto& [_, ticket] : pending_) {
            if (ticket->cancellation) {
                ticket->cancellation->store(true, std::memory_order_release);
            }
            tickets.push_back(std::move(ticket));
        }
        pending_.clear();
        queue_.clear();
    }
    for (auto& ticket : tickets) finish_ticket(ticket, PythonRuntimeResponse::runtime_stopping());
    queue_cv_.notify_all();
}

void PythonRuntimeCoordinator::begin_quiescing() {
    transition_and_cancel(PythonRuntimeState::Quiescing);
}

void PythonRuntimeCoordinator::begin_draining() {
    transition_and_cancel(PythonRuntimeState::Draining);
}

void PythonRuntimeCoordinator::begin_python_stopping() {
    transition_and_cancel(PythonRuntimeState::PythonStopping);
}

void PythonRuntimeCoordinator::stop() {
    transition_and_cancel(PythonRuntimeState::Stopped);
}

PythonRuntimeState PythonRuntimeCoordinator::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

std::size_t PythonRuntimeCoordinator::pending_count() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

PythonRuntimeSnapshot PythonRuntimeCoordinator::snapshot() const {
    std::lock_guard lock(mutex_);
    PythonRuntimeSnapshot result;
    result.state = state_.load(std::memory_order_relaxed);
    result.queued_count = queue_.size();
    result.pending_count = pending_.size();
    result.consumer_thread_bound = consumer_thread_id_ != std::thread::id{};
    result.consumer_thread_token = std::hash<std::thread::id>{}(consumer_thread_id_);
    result.current_request = current_request_;
    result.execution_phase = execution_phase_;
    return result;
}

void PythonRuntimeCoordinator::set_execution_phase(std::string phase) {
    std::lock_guard lock(mutex_);
    execution_phase_ = std::move(phase);
}

bool PythonRuntimeCoordinator::bind_consumer_thread() {
    std::lock_guard lock(mutex_);
    const auto current = std::this_thread::get_id();
    if (consumer_thread_id_ == std::thread::id{}) {
        consumer_thread_id_ = current;
        return true;
    }
    return consumer_thread_id_ == current;
}

bool PythonRuntimeCoordinator::is_consumer_thread() const {
    std::lock_guard lock(mutex_);
    return consumer_thread_id_ == std::this_thread::get_id();
}

PythonRuntimeCoordinator* active_python_runtime_coordinator() noexcept {
    return g_active_coordinator.load(std::memory_order_acquire);
}

void install_active_python_runtime_coordinator(PythonRuntimeCoordinator* coordinator) noexcept {
    g_active_coordinator.store(coordinator, std::memory_order_release);
}

}  // namespace Corona::Script::Python
