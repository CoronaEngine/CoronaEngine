#pragma once
#include "archive_fixture.h"
#include <corona/systems/script/python_runtime_coordinator.h>
#include <thread>

namespace ArchiveFixture {
namespace Py = Corona::Script::Python;
class ArchiveTestHost {
    Py::PythonRuntimeCoordinator coordinator_;
    std::jthread service_;
public:
    ArchiveTestHost() : service_([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto request = coordinator_.wait_pop(std::chrono::milliseconds(50));
            if (!request) continue;
            const auto message = Json::parse(request->payload_json);
            auto reply = call(request->function.c_str(), message.at("args").at(0), request->module.c_str());
            coordinator_.complete(request->request_id, Py::PythonRuntimeResponse::success(reply.dump()));
        }
    }) { Py::install_active_python_runtime_coordinator(&coordinator_); }
    ~ArchiveTestHost() {
        Py::install_active_python_runtime_coordinator(nullptr);
        service_.request_stop();
        coordinator_.stop();
    }
};
}
