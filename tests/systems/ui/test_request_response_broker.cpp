#include "request_response_broker.h"

#include <cassert>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

using Corona::Systems::UI::CefRequestContext;
using Corona::Systems::UI::CefRequestResponseBroker;
using Corona::Systems::UI::CefResponse;

int main() {
    CefRequestResponseBroker broker;
    std::vector<std::pair<std::string, std::string>> delivered;
    broker.set_dispatcher([&](const CefRequestContext& request, const CefResponse& response) {
        delivered.emplace_back(request.request_id, response.status);
    });

    assert(broker.register_request({"req-1", 10, "frame-20", 7, "scene.ini"}));
    assert(!broker.register_request({"req-1", 10, "frame-20", 7, "scene.ini"}));
    assert(broker.complete("req-1", {"success", {}}));
    assert(!broker.complete("req-1", {"success", {}}));
    broker.dispatch();
    assert(delivered.size() == 1);
    assert(delivered[0].first == "req-1");
    assert(delivered[0].second == "success");

    assert(broker.register_request({"shared-id", 11, "frame-21", 8, "scene.ini"}));
    assert(broker.register_request({"shared-id", 12, "frame-22", 8, "scene.ini"}));
    assert(!broker.complete("shared-id", {"success", {}}));
    assert(broker.complete("shared-id", 12, "scene.ini", {"success", {}}));
    broker.dispatch();
    assert(delivered.size() == 2);
    assert(delivered[1].first == "shared-id");

    assert(broker.register_request({"req-2", 10, "frame-20", 7, "scene.ini"}));
    broker.cancel_browser(10);
    assert(broker.pending_count() == 0);

    assert(broker.register_request({"req-3", 10, "frame-20", 7, "scene.ini"}));
    const auto now = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    broker.expire(now);
    broker.dispatch();
    assert(delivered.size() == 3);
    assert(delivered[2].first == "req-3");
    assert(delivered[2].second == "timeout");
    return 0;
}
