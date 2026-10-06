#include "cef/actor_selection_routing.h"

#include <cstdlib>

namespace {

void expect(bool condition) {
    if (!condition) std::abort();
}

}  // namespace

int main() {
    using Corona::Systems::UI::make_actor_selection_event_payload;

    const auto camera_view = make_actor_selection_event_payload(
        "scene.ini",
        "model",
        "Ball",
        {{"sourceViewport", "cameraView"}, {"sourceCameraHandle", 22}});
    expect(camera_view.at("scene") == "scene.ini");
    expect(camera_view.at("actor") == "Ball");
    expect(camera_view.at("source_viewport") == "cameraView");
    expect(camera_view.at("source_camera_handle") == 22);

    const auto default_main =
        make_actor_selection_event_payload("scene.ini", "model", "Ball", {});
    expect(default_main.at("source_viewport") == "main");
    expect(default_main.at("source_camera_handle") == 0);

    const auto invalid_source = make_actor_selection_event_payload(
        "scene.ini",
        "model",
        "Ball",
        {{"sourceViewport", "unknown"}, {"sourceCameraHandle", -1}});
    expect(invalid_source.at("source_viewport") == "main");
    expect(invalid_source.at("source_camera_handle") == 0);
    return 0;
}
