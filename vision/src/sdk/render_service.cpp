//
// Created by Zero on 2026/4/30.
//

#include "render_service.h"

namespace vision {
namespace sdk {

namespace {

class UnsupportedRenderSession final : public RenderSession {
private:
    SessionConfig config_{};
    std::string last_error_{"Vision source-level render session is declared but not implemented yet."};

public:
    explicit UnsupportedRenderSession(SessionConfig config) noexcept
        : config_(std::move(config)) {}

    [[nodiscard]] VisionExtent2D extent() const noexcept override {
        return config_.initial_extent;
    }

    VisionStatusCode update_scene(const SceneSource &) noexcept override {
        return VisionStatusCode_Unsupported;
    }

    [[nodiscard]] RenderResult render(const RenderRequest &request) noexcept override {
        RenderResult result;
        result.status = VisionStatusCode_Unsupported;
        result.extent = request.output.extent;
        result.format = request.output.format;
        result.message = last_error_;
        return result;
    }

    VisionStatusCode resize(VisionExtent2D extent) noexcept override {
        config_.initial_extent = extent;
        return VisionStatusCode_Success;
    }

    VisionStatusCode reset_accumulation() noexcept override {
        return VisionStatusCode_Unsupported;
    }

    [[nodiscard]] std::string last_error() const override {
        return last_error_;
    }
};

}// namespace

std::unique_ptr<RenderSession> create_render_session(const SessionConfig &config) {
    return std::make_unique<UnsupportedRenderSession>(config);
}

}// namespace sdk
}// namespace vision