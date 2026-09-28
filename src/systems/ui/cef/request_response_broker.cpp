#include "request_response_broker.h"

#include <horizon/core/logging.h>

#include <algorithm>
#include <utility>

namespace Corona::Systems::UI {

std::string CefRequestResponseBroker::make_key(const CefRequestContext& context) {
    return context.request_id + "\x1f" + std::to_string(context.browser_id) + "\x1f" +
           context.frame_id;
}

bool CefRequestResponseBroker::register_request(CefRequestContext context,
                                                Dispatcher dispatcher,
                                                Clock::time_point now) {
    if (context.request_id.empty()) return false;
    std::lock_guard lock(mutex_);
    const auto key = make_key(context);
    if (pending_.contains(key)) {
        CFW_LOG_WARNING("CEF response broker duplicate request registration: {}", context.request_id);
        return false;
    }
    pending_.emplace(key,
                      Pending{std::move(context), std::move(dispatcher), now + kDefaultTimeout});
    return true;
}

bool CefRequestResponseBroker::complete(std::string_view request_id, CefResponse response) {
    std::lock_guard lock(mutex_);
    auto it = pending_.end();
    for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
        if (candidate->second.context.request_id == request_id) {
            if (it != pending_.end()) {
                CFW_LOG_WARNING("CEF response broker ambiguous request completion: {}", request_id);
                return false;
            }
            it = candidate;
        }
    }
    if (it == pending_.end()) {
        CFW_LOG_WARNING("CEF response broker completion for unknown request: {}", request_id);
        return false;
    }
    completed_.push_back(Completed{std::move(it->second.context),
                                   std::move(it->second.dispatcher),
                                   std::move(response)});
    pending_.erase(it);
    return true;
}

bool CefRequestResponseBroker::complete(std::string_view request_id,
                                        std::uintptr_t camera_handle,
                                        std::string_view scene_id,
                                        CefResponse response) {
    std::lock_guard lock(mutex_);
    auto it = pending_.end();
    for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
        const auto& context = candidate->second.context;
        if (context.request_id == request_id && context.camera_handle == camera_handle &&
            context.scene_id == scene_id) {
            it = candidate;
            break;
        }
    }
    if (it == pending_.end()) {
        CFW_LOG_WARNING("CEF response broker completion for unknown request: {}", request_id);
        return false;
    }
    completed_.push_back(Completed{std::move(it->second.context),
                                   std::move(it->second.dispatcher),
                                   std::move(response)});
    pending_.erase(it);
    return true;
}

bool CefRequestResponseBroker::cancel(std::string_view request_id) {
    std::lock_guard lock(mutex_);
    bool removed = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.context.request_id == request_id) {
            it = pending_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    return removed;
}

std::size_t CefRequestResponseBroker::cancel_browser(int browser_id) {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.context.browser_id == browser_id) {
            it = pending_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    if (count != 0) {
        CFW_LOG_DEBUG("CEF response broker cancelled {} request(s) for browser {}", count, browser_id);
    }
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
                                    [browser_id](const Completed& item) {
                                        return item.context.browser_id == browser_id;
                                    }),
                     completed_.end());
    return count;
}

std::size_t CefRequestResponseBroker::expire(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.deadline <= now) {
            completed_.push_back(Completed{std::move(it->second.context),
                                           std::move(it->second.dispatcher),
                                           CefResponse{"timeout", nlohmann::json::object()}});
            it = pending_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    if (count != 0) {
        CFW_LOG_WARNING("CEF response broker timed out {} request(s)", count);
    }
    return count;
}

std::size_t CefRequestResponseBroker::dispatch() {
    std::vector<Completed> ready;
    Dispatcher fallback;
    {
        std::lock_guard lock(mutex_);
        ready.swap(completed_);
        fallback = default_dispatcher_;
    }
    std::size_t count = 0;
    for (const auto& item : ready) {
        const auto& dispatcher = item.dispatcher ? item.dispatcher : fallback;
        if (dispatcher) {
            dispatcher(item.context, item.response);
            ++count;
        }
    }
    return count;
}

void CefRequestResponseBroker::set_dispatcher(Dispatcher dispatcher) {
    std::lock_guard lock(mutex_);
    default_dispatcher_ = std::move(dispatcher);
}

std::size_t CefRequestResponseBroker::pending_count() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

CefRequestResponseBroker& cef_request_response_broker() {
    static CefRequestResponseBroker broker;
    return broker;
}

}  // namespace Corona::Systems::UI
