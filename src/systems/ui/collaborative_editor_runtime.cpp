#include "collaborative_editor_runtime.h"

#include "cef/cef_editor_native_api_registry.h"

#include <array>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

void tick_collaborative_editor_runtime(std::size_t batch_limit) {
    register_builtin_native_api_handlers();
    static constexpr std::array<std::string_view, 4> kPollFunctions{
        "poll_pending_actor_create",
        "poll_pending_actor_state_update",
        "poll_pending_actor_transform",
        "poll_pending_actor_delete",
    };

    for (const auto function : kPollFunctions) {
        for (std::size_t i = 0; i < batch_limit; ++i) {
            const auto result = NativeApiRegistry::instance().dispatch(
                NativeRequest{"Network", std::string(function), nlohmann::json::array()},
                NativeContext{});
            if (!result || !result->success) {
                break;
            }
            if (!result->data.value("has_pending", false) ||
                result->data.value("retrying", false)) {
                break;
            }
        }
    }
}

}  // namespace Corona::Systems::UI
