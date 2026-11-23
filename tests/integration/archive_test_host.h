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
            try {
                if (!request->service_args) throw std::runtime_error("Archive service request has no structured arguments");
                auto reply = call(request->function.c_str(), request->service_args->at(0), request->module.c_str());
                auto response = Py::PythonRuntimeResponse::success({});
                response.service_result = std::make_shared<const Json>(std::move(reply));
                coordinator_.complete(request->request_id, std::move(response));
            } catch (const std::exception& error) {
                coordinator_.complete(request->request_id, Py::PythonRuntimeResponse::failure(error.what()));
            }
        }
    }) { Py::install_active_python_runtime_coordinator(&coordinator_); }
    ~ArchiveTestHost() {
        Py::install_active_python_runtime_coordinator(nullptr);
        service_.request_stop();
        coordinator_.stop();
    }
};
}
