#include <corona/kernel/core/kernel_context.h>
#include <corona/events/acoustics_system_events.h>
#include <corona/engine/engine_runtime_api.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include "cef/cef_editor_native_api_registry.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/cef_window_operations.h"
#include "cef/cef_scene_operations.h"
#include "cef/scene_tree_actor.h"
#include "cef/cef_viewport_pick.h"
#include "cef/cef_script_input.h"
#include "cef/browser_ui.h"
#include "editor_api/cef_editor_api.h"
#include "optics/camera_viewport_updates.h"

#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace UI = Corona::Systems::UI;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_identity_lifetime() {
    UI::CameraWindowLifetime owner;
    const auto first = owner.token();
    UI::CameraWindowLifetime moved(std::move(owner));
    require(first->active && moved.token() == first, "move must preserve an active camera authority");
    UI::CameraWindowLifetime replacement;
    const auto retired = replacement.token();
    replacement = std::move(moved);
    require(!retired->active && replacement.token() == first, "move assignment must retire the overwritten authority");
    std::unique_lock guard(first->mutex);
    std::promise<void> started;
    auto completion = std::async(std::launch::async, [&] { started.set_value(); replacement.retire(); });
    started.get_future().wait();
    require(completion.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
            "retirement must wait until the in-progress native operation releases its lease");
    guard.unlock();
    completion.get();
    require(!first->active, "retired authority must reject late native work");
}

void test_camera_release_discards_queued_viewport_updates() {
    auto& hub = Corona::SharedDataHub::instance();
    hub.drain_camera_viewport_updates();
    hub.drain_camera_releases();
    const auto retired = hub.camera_storage().allocate();
    const auto live = hub.camera_storage().allocate();
    hub.enqueue_camera_viewport_update({.camera_handle = retired, .width = 400});
    hub.enqueue_camera_viewport_update({.camera_handle = live, .width = 800});
    hub.enqueue_camera_release({.camera_handle = retired});
    const auto updates = hub.drain_camera_viewport_updates();
    const auto releases = hub.drain_camera_releases();
    hub.camera_storage().deallocate(retired);
    hub.camera_storage().deallocate(live);
    require(releases.size() == 1 && releases[0].camera_handle == retired,
        "viewport cleanup must preserve the render owner's release command");
    require(updates.size() == 1 && updates[0].camera_handle == live && updates[0].width == 800,
        "camera retirement must discard its queued viewport updates without dropping another live camera");
}

void test_deferred_viewports_use_the_original_camera_authority() {
    auto& hub = Corona::SharedDataHub::instance();
    const auto camera_handle = hub.camera_storage().allocate();
    const auto peer_handle = hub.camera_storage().allocate();
    auto* surface = reinterpret_cast<void*>(std::uintptr_t{0x1234});
    { auto peer = hub.camera_storage().acquire_write(peer_handle); peer->surface = surface; }
    UI::CameraWindowLifetime old_scene;
    const auto retired = old_scene.token();
    old_scene.retire();
    // The address is occupied by a live camera, as after storage-slot reuse.
    // Occupancy alone must not authorize work captured under the old scene.
    Corona::CameraViewportUpdateCommand command{
        .camera_handle = camera_handle, .surface = surface, .view_open = false,
        .x = 10, .y = 20, .width = 400, .height = 300,
        .render_width = 800, .render_height = 600, .authority = retired,
    };
    const bool retired_applied = Corona::Systems::apply_camera_viewport_update(hub, command);
    { auto camera = hub.camera_storage().acquire_read(camera_handle);
      auto peer = hub.camera_storage().acquire_read(peer_handle);
      require(!retired_applied && !camera->viewport_rect_active && peer->surface == surface,
          "retired viewport commands cannot mutate a reused camera address or detach a live peer"); }
    UI::CameraWindowLifetime current_scene;
    command.authority = current_scene.token();
    require(Corona::Systems::apply_camera_viewport_update(hub, command), "live scoped viewport updates must apply");
    { auto camera = hub.camera_storage().acquire_read(camera_handle);
      auto peer = hub.camera_storage().acquire_read(peer_handle);
      require(camera->surface == surface && camera->view_x == 10 && camera->view_y == 20 &&
          camera->view_width == 400 && camera->width == 800 && camera->height == 600 &&
          camera->aspect == 800.0f / 600 && !camera->follows_default_surface && !peer->surface,
          "valid viewport updates preserve geometry, resolution, aspect and exclusive embedded-surface ownership"); }
    { auto peer = hub.camera_storage().acquire_write(peer_handle); peer->surface = surface; peer->view_open = true; }
    require(Corona::Systems::apply_camera_viewport_update(hub, command), "live viewport updates remain idempotent");
    { auto peer = hub.camera_storage().acquire_read(peer_handle);
      require(peer->surface == surface, "embedded viewport updates cannot detach a floating camera"); }
    hub.camera_storage().deallocate(camera_handle);
    command.authority.reset();
    require(!Corona::Systems::apply_camera_viewport_update(hub, command),
        "an unscoped late viewport for missing storage must not abort the Optics tick");
    { auto peer = hub.camera_storage().acquire_read(peer_handle);
      require(peer->surface == surface, "missing-camera commands cannot perform surface cleanup"); }
    hub.camera_storage().deallocate(peer_handle);
}

void test_cached_web_viewport_binding_retires_with_its_camera() {
    UI::WindowRequest request{UI::WindowMethod::ConfigureViewport, 1};
    request.archive_session = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.archive_session;
    request.scene_id = "scene.ini"; request.camera_id = "primary";
    require(UI::prepare_camera_window_request(request).error_code.empty(), "viewport fixture camera resolves");
    UI::CameraWindowLifetime lifetime;
    request.camera_authority = lifetime.token();
    request.viewport.emplace(); request.viewport->bounds = UI::ViewportBounds{10,20,400,300,800,600};
    UI::BrowserTab tab; tab.open = true; tab.docking_pos = "main";
    require(UI::apply_viewport_configuration(request, tab).error_code.empty() &&
        tab.web_camera_authority == request.camera_authority, "cached bounds retain their original scene authority");
    UI::refresh_viewport_state(tab);
    require(tab.web_camera_handle == request.camera_handle, "refresh preserves a live camera binding");
    lifetime.retire();
    UI::refresh_viewport_state(tab);
    require(!tab.web_camera_handle && !tab.web_camera_authority && !tab.web_camera_width &&
        !tab.web_camera_height && !tab.web_camera_render_width && !tab.web_camera_render_height,
        "scene retirement clears cached bounds before another native UI frame can submit the old camera");
}

Json snapshot() {
    auto camera = [](const char* id, const char* name, bool deletable) -> Json {
        return {{"id",id},{"name",name},{"deletable",deletable},
            {"position",{0,0,-5}},{"forward",{0,0,1}},{"world_up",{0,1,0}},
            {"width",800},{"height",600},{"view_width",800},{"view_height",600}};
    };
    return {{"schema_version",1}, {"archive_session","camera-test-archive"}, {"archive_type","portable_scene"},
        {"project_root",std::filesystem::temp_directory_path().string()}, {"scene",{
            {"route","scene.ini"},{"name","Camera test"},{"core_version","1"},
            {"scripts",Json::object()},{"terrain",Json::object()},{"environment",Json::object()},
            {"vision",Json::object()},{"actors",Json::array({{
                {"name","Transform probe"},{"actor_guid","transform-probe"},{"actor_type","audio"},
                {"route",""},{"asset_path",""},
                {"transform",{{"position",{1,2,3}},{"rotation",{4,5,6}},{"scale",{1,1,1}}}}
            }})},
            {"cameras",Json::array({camera("primary","Primary camera",false),
                                   camera("secondary","Secondary camera",true)})},
            {"active_camera_id","primary"}}}};
}

void test_resource_import_results_and_modal_retirement() {
    const auto info = [] (const std::string&) {
        Corona::API::MediaInfo media;
        media.resource_id = std::numeric_limits<std::uint64_t>::max();
        media.media_type = "audio"; media.codec = "pcm_s16le";
        media.duration_seconds = 2.5; media.sample_rate = 48000; media.channels = 2;
        return media;
    };
    UI::SceneRequest request{UI::SceneMethod::ImportResourceFile};
    request.archive_session = "camera-test-archive"; request.read_scene = "scene.ini";
    int picks = 0, imports = 0;
    const UI::ResourceFilePicker picker = [&](const auto& root, const auto& type) {
        ++picks;
        require(type == request.file_type, "the dialog receives the requested type");
        return std::optional(root / "selected.wav");
    };
    const UI::MediaFileImporter importer = [&](const auto& path) {
        ++imports;
        require(std::filesystem::path(path).is_absolute(), "decoder receives the absolute selected path");
        return info(path);
    };
    for (const auto* type : {"model", "actor", "ui_image", "multimedia"}) {
        request.file_type = type;
        const auto result = UI::import_native_resource_for_test(request, picker, importer);
        const auto& value = result.resource_import;
        require(result.error_code.empty() && value.status == "success" && value.scene == request.read_scene &&
            value.archive_session == *request.archive_session && !value.path.empty(), "import retains its observed scope and selected route");
        if (request.file_type == "multimedia") {
            require(value.resource_id == "18446744073709551615" && value.media_type == "audio" &&
                value.codec == "pcm_s16le" && value.duration == 2.5 && value.sample_rate == 48000 && value.channels == 2,
                "typed import preserves the complete audio metadata and opaque resource identity");
        }
    }
    require(picks == 4 && imports == 1, "only multimedia import invokes the decoder, exactly once");
    request.file_type = "unsupported";
    require(UI::import_native_resource_for_test(request, picker, importer).resource_import.code == "unsupported_file_type" && picks == 4,
        "unsupported types never open the dialog");
    request.file_type = "multimedia";
    require(UI::import_native_resource_for_test(request, [](const auto&, const auto&) {
        return std::optional<std::filesystem::path>{};
    }, importer).resource_import.status == "canceled" && imports == 1, "cancel does not import a resource");
    require(UI::import_native_resource_for_test(request, picker, [](const auto&) {
        return Corona::API::MediaInfo{};
    }).resource_import.code == "media_unrecognized", "decoder failure preserves the import error contract");
    auto stale = request; stale.archive_session = "retired";
    const auto before = picks;
    require(UI::import_native_resource_for_test(stale, picker, importer).error_code == "PROJECT_CONTEXT_CHANGED" && picks == before,
        "stale scope must be rejected before prompting");
    for (bool canceled : {false, true}) {
        // Recreate even the same archive ID: modal work belongs to the old owner.
        UI::load_native_editor_scene_for_test(snapshot());
        const auto imported_before = imports;
        const auto result = UI::import_native_resource_for_test(request, [&](const auto& root, const auto&) {
            UI::load_native_editor_scene_for_test(snapshot());
            return canceled ? std::optional<std::filesystem::path>{} : std::optional(root / "selected.wav");
        }, importer);
        require(result.error_code == "PROJECT_CONTEXT_CHANGED" && imports == imported_before,
            "a dialog returning after scene replacement must reject before decoding, even on cancel or a reused archive ID");
    }
    UI::load_native_editor_scene_for_test(snapshot());
}

void test_audio_adapters_publish_once_with_exact_identity() {
    auto* bus = Corona::Kernel::KernelContext::instance().event_bus();
    std::vector<Corona::Events::PlayAudioEvent> plays;
    std::vector<Corona::Events::StopAudioEvent> stops;
    const auto playing = bus->subscribe<Corona::Events::PlayAudioEvent>([&](const auto& e) { plays.push_back(e); });
    const auto stopping = bus->subscribe<Corona::Events::StopAudioEvent>([&](const auto& e) { stops.push_back(e); });
    UI::SceneRequest request{UI::SceneMethod::PlayAudioResource};
    request.audio.resource_id = std::numeric_limits<std::uint64_t>::max(); request.audio.loop = true;
    const auto reply = UI::execute_scene_operation(request);
    require(reply.error_code.empty() && reply.audio.playing && reply.audio.resource_id == request.audio.resource_id,
        "typed audio replies preserve opaque resource identity and accepted state");
    request.method = UI::SceneMethod::StopAudioResource;
    require(!UI::execute_scene_operation(request).audio.playing, "stop returns the accepted stopped state");
    const auto legacy_play = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "play_audio", {request.audio.resource_id, true}}, {});
    const auto legacy_stop = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "stop_audio", {request.audio.resource_id}}, {});
    require(legacy_play && legacy_play->success && legacy_play->data.value("ok", false) &&
        legacy_stop && legacy_stop->success && legacy_stop->data.value("ok", false), "Python adapters retain their success contract");
    require(plays.size() == 2 && stops.size() == 2, "one event per action across both adapters");
    for (const auto& event : plays) require(event.resource_id == request.audio.resource_id && event.loop && !event.acoustics_handle,
        "play adapters preserve full uint64 resource ID and loop flag");
    for (const auto& event : stops) require(event.resource_id == request.audio.resource_id && !event.acoustics_handle,
        "stop adapters address the same global resource");
    request.audio.resource_id = 0;
    require(UI::execute_scene_operation(request).error_code == "INVALID_ARGUMENT", "typed audio rejects zero");
    const auto invalid = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "play_audio", {0, false}}, {});
    require(invalid && !invalid->success && plays.size() == 2 && stops.size() == 2, "invalid adapters cannot publish events");
    bus->unsubscribe(playing); bus->unsubscribe(stopping);
}

void test_native_scene_values_and_non_web_adapters() {
    const auto active = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(active.archive_session == "camera-test-archive" && active.scene == "scene.ini" && !active.selection,
            "scene reads must expose owned native metadata without creating a selection");
    UI::SceneRequest selection{UI::SceneMethod::SelectActor};
    selection.archive_session = active.archive_session;
    selection.selection = {active.scene, "scene", "", "cameraView", "camera/max"};
    selection.selection.node_index = std::numeric_limits<std::uint32_t>::max() - 1;
    require(UI::execute_scene_operation(selection).error_code.empty(), "typed native selection must succeed");
    const auto observed = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(observed.selection && observed.selection->source_camera_id == "camera/max"
            && observed.selection->node_index == std::numeric_limits<std::uint32_t>::max() - 1,
            "selection values must retain stable camera IDs and optional uint32 precision");
    selection.archive_session = "old-archive";
    require(UI::execute_scene_operation(selection).error_code == "PROJECT_CONTEXT_CHANGED", "old selection scope must fail");
    selection.archive_session = active.archive_session; selection.selection.scene = "other.ini";
    require(UI::execute_scene_operation(selection).error_code == "PROJECT_CONTEXT_CHANGED", "wrong scene must fail");
    UI::CefEditorApiEndpoint cef;
    for (const auto* method : {"SceneTools.read_active_scene", "SceneTools.select_actor"}) {
        const auto* spec = UI::EditorApiRegistry::instance().find(method);
        require(spec && std::string(spec->js_wrapper).empty(), "migrated Web methods cannot retain a manifest wrapper");
        require(!cef.invoke(method, Json::array(), {}).success, "CEF cannot invoke a migrated method through the old registry");
    }
    auto& registry = UI::EditorApiRegistry::instance();
    for (const auto caller : {UI::EditorApiCaller::PythonScript, UI::EditorApiCaller::ScriptRuntime, UI::EditorApiCaller::Native}) {
        const auto read = registry.invoke({"SceneTools.read_active_scene", Json::array(), caller}, {});
        require(read.success && read.data["selection"]["source_camera_id"] == "camera/max",
                "non-Web callers must retain stable selection identity");
    }
    const auto selected = registry.invoke({"SceneTools.select_actor", {"scene.ini", "scene", "", {{"node_index", 0}}},
        UI::EditorApiCaller::PythonScript}, {});
    require(selected.success, "Python selection must share the native authority without a Web transport");
    const auto python_selection = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.selection;
    require(python_selection && python_selection->node_index == 0 && !python_selection->actor_guid,
            "Python and V8 selections must update the same typed state including node zero");
}

void test_owned_actor_index_projection() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest read{UI::SceneMethod::ReadActorIndex};
    read.read_scene = before.scene;
    read.archive_session = before.archive_session;
    const auto reply = UI::execute_scene_operation(read);
    require(reply.error_code.empty() && reply.actor_index.archive_session == before.archive_session &&
            reply.actor_index.scene == before.scene && reply.actor_index.tree_revision == before.tree_revision,
            "actor index must carry observed scene identity and revision");
    require(reply.actor_index.actors.size() == 1 && reply.actor_index.actors[0].actor_guid == "transform-probe" &&
            reply.actor_index.actors[0].name == "Transform probe",
            "actor index must contain stable identity and names without requiring geometry");
    require(reply.actor_index.actors[0].type == "audio",
            "an Actor without a file extension must retain its semantic type in the lightweight index");
    auto bad = read; bad.archive_session.reset();
    require(UI::execute_scene_operation(bad).error_code == "INVALID_ARGUMENT", "an actor index requires an explicit archive");
    bad = read; bad.read_scene.clear();
    require(UI::execute_scene_operation(bad).error_code == "INVALID_ARGUMENT", "an actor index requires an explicit scene");
    bad = read; bad.archive_session = "retired";
    require(UI::execute_scene_operation(bad).error_code == "PROJECT_CONTEXT_CHANGED", "actor index must reject an old archive");
    bad = read; bad.read_scene = "other.ini";
    require(UI::execute_scene_operation(bad).error_code == "PROJECT_CONTEXT_CHANGED", "actor index must reject another scene");
    const auto after = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(after.tree_revision == before.tree_revision && after.archive_session == before.archive_session &&
            after.selection.has_value() == before.selection.has_value(), "actor index reads must not mutate scene state");
}

void test_owned_actor_property_projection() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest read{UI::SceneMethod::ReadActorProperties};
    read.read_scene = before.scene; read.archive_session = before.archive_session; read.read_actor = "transform-probe";
    const auto reply = UI::execute_scene_operation(read);
    require(reply.error_code.empty() && reply.actor_read.archive_session == before.archive_session &&
        reply.actor_read.properties.scene == before.scene && reply.actor_read.properties.actor == "Transform probe" &&
        reply.actor_read.properties.actor_guid == "transform-probe", "property projection carries stable scope and Actor identity");
    require(reply.actor_read.actor_type == "audio" && reply.actor_read.load_status == "loaded" &&
        reply.actor_read.position == std::array<float,3>{1,2,3} && reply.actor_read.rotation == std::array<float,3>{4,5,6} &&
        reply.actor_read.scale == std::array<float,3>{1,1,1}, "property reads preserve transforms and load state without bounds");
    read.read_actor = "Transform probe";
    require(UI::execute_scene_operation(read).actor_read.properties.actor_guid == "transform-probe", "names resolve to the same authoritative Actor");
    for (int kind = 0; kind < 6; ++kind) {
        auto invalid = read;
        if (kind == 0) invalid.archive_session.reset();
        if (kind == 1) invalid.archive_session = "retired";
        if (kind == 2) invalid.read_scene = "wrong.ini";
        if (kind == 3) invalid.read_scene.clear();
        if (kind == 4) invalid.read_actor.clear();
        if (kind == 5) invalid.read_actor = "missing";
        const auto error = UI::execute_scene_operation(invalid).error_code;
        require(error == (kind == 0 || kind == 3 || kind == 4 ? "INVALID_ARGUMENT" :
            kind == 5 ? "ACTOR_NOT_FOUND" : "PROJECT_CONTEXT_CHANGED"), "property reads reject missing or retired identity");
    }
    const auto after = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(after.tree_revision == before.tree_revision && after.selection.has_value() == before.selection.has_value(),
        "property reads cannot change tree revision or selection");
    UI::CefEditorApiEndpoint cef;
    const auto* spec = UI::EditorApiRegistry::instance().find("SceneTools.read_actor");
    require(spec && std::string(spec->js_wrapper).empty() && !cef.invoke("SceneTools.read_actor", Json::array(), {}).success,
        "the migrated Web actor reader must not retain a manifest path");
}

void test_owned_camera_projection() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest read{UI::SceneMethod::ReadCameras};
    read.read_scene = before.scene; read.archive_session = before.archive_session;
    const auto reply = UI::execute_scene_operation(read);
    require(reply.error_code.empty() && reply.camera_list.archive_session == before.archive_session &&
        reply.camera_list.scene == before.scene && reply.camera_list.active_camera_id == "primary" &&
        reply.camera_list.cameras.size() == 2, "camera projection carries observed scope and active camera identity");
    const auto legacy = UI::NativeApiRegistry::instance().dispatch({"SceneTools","list_camera_views", {before.scene}}, {});
    require(legacy && legacy->success, "non-Web camera reads retain their original contract");
    for (size_t index = 0; index < reply.camera_list.cameras.size(); ++index) {
        const auto& value = reply.camera_list.cameras[index]; const auto& expected = legacy->data["cameras"][index];
        require(value.properties.camera_id == expected["camera_id"].get<std::string>() && value.properties.handle == expected["handle"].get<std::uint64_t>() &&
            value.properties.position == expected["position"].get<std::array<float,3>>() &&
            value.properties.width == expected["width"].get<int>() && value.properties.view_open == expected["view_open"].get<bool>() &&
            value.output_mode == expected["output_mode"].get<std::string>() && value.render_backend == expected["render_backend"].get<std::string>() &&
            value.vision_render_mode == expected["vision_render_mode"].get<std::string>() && value.vision_denoiser == expected["vision_denoiser"].get<std::string>() &&
            value.vision_accumulation == expected["vision_accumulation"].get<bool>() && value.vision_denoise == expected["vision_denoise"].get<bool>() &&
            value.vision_stable_planes == expected["vision_stable_planes"].get<bool>() && value.deletable == expected["deletable"].get<bool>(),
            "typed camera values preserve pose, window and rendering control state");
    }
    for (int kind = 0; kind < 4; ++kind) {
        auto invalid = read;
        if (kind == 0) invalid.archive_session.reset();
        if (kind == 1) invalid.read_scene.clear();
        if (kind == 2) invalid.archive_session = "retired";
        if (kind == 3) invalid.read_scene = "other.ini";
        require(UI::execute_scene_operation(invalid).error_code == (kind < 2 ? "INVALID_ARGUMENT" : "PROJECT_CONTEXT_CHANGED"),
            "camera reads reject missing or retired scene identity");
    }
    require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == before.tree_revision,
        "camera reads cannot mutate the tree");
    UI::CefEditorApiEndpoint cef;
    for (const auto* method : {"SceneTools.list_camera_views", "SceneTools.get_scene_snapshot"}) {
        const auto* spec = UI::EditorApiRegistry::instance().find(method);
        require(spec && std::string(spec->js_wrapper).empty() && !cef.invoke(method, Json::array(), {}).success,
            "migrated camera and full snapshot Web routes must remain retired");
    }
}

void test_owned_scene_tree_projection() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest read{UI::SceneMethod::ReadTree};
    read.read_scene = before.scene; read.archive_session = before.archive_session;
    const auto reply = UI::execute_scene_operation(read);
    require(reply.error_code.empty() && reply.tree.cameras.archive_session == before.archive_session &&
        reply.tree.cameras.scene == before.scene && reply.tree.tree_revision == before.tree_revision,
        "tree reads carry the observed archive, scene and revision");
    const auto legacy = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "list_scene_tree", {before.scene}}, {});
    require(legacy && legacy->success && legacy->data["actors"].size() == reply.tree.actors.size(),
        "non-Web tree reads preserve their contract");
    for (size_t index = 0; index < reply.tree.actors.size(); ++index) {
        require(Corona::Ui::Cef::scene_tree_actor_to_json(reply.tree.actors[index]) == legacy->data["actors"][index],
            "tree projections share all Actor row values with the non-Web adapter");
    }
    read.method = UI::SceneMethod::ReadCameras;
    const auto cameras = UI::execute_scene_operation(read).camera_list;
    require(cameras.active_camera_id == reply.tree.cameras.active_camera_id &&
        cameras.cameras.size() == reply.tree.cameras.cameras.size(), "tree reuses the scoped camera projection");
    read.method = UI::SceneMethod::ReadTree;
    for (int kind = 0; kind < 4; ++kind) {
        auto invalid = read;
        if (kind == 0) invalid.archive_session.reset();
        if (kind == 1) invalid.read_scene.clear();
        if (kind == 2) invalid.archive_session = "retired";
        if (kind == 3) invalid.read_scene = "other.ini";
        require(UI::execute_scene_operation(invalid).error_code == (kind < 2 ? "INVALID_ARGUMENT" : "PROJECT_CONTEXT_CHANGED"),
            "tree reads reject missing or retired scene identity");
    }
    const auto after = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(after.tree_revision == before.tree_revision && after.selection.has_value() == before.selection.has_value(),
        "tree reads cannot mutate revision or selection");
    UI::CefEditorApiEndpoint cef;
    const auto* spec = UI::EditorApiRegistry::instance().find("SceneTools.list_scene_tree");
    require(spec && std::string(spec->js_wrapper).empty() &&
        !cef.invoke("SceneTools.list_scene_tree", Json::array({before.scene}), {}).success,
        "the old Web tree route must remain retired");
}

void test_one_native_transform_authority() {
    UI::SceneRequest write{UI::SceneMethod::SetActorTransform};
    write.archive_session = "camera-test-archive";
    write.transform.scene = "scene.ini";
    write.transform.actor = "transform-probe";
    write.transform.position = std::array<float,3>{0,8,-3};
    const auto first = UI::execute_scene_operation(write);
    require(first.error_code.empty() && first.transform.actor_guid == "transform-probe" &&
            first.transform.actor_version == 2 && first.transform.position == *write.transform.position &&
            first.transform.rotation == std::array<float,3>{4,5,6} && first.transform.scale == std::array<float,3>{1,1,1},
            "one typed transform patch must preserve omitted fields and advance the native Actor version once");
    auto bad = write; bad.archive_session = "old";
    require(UI::execute_scene_operation(bad).error_code == "PROJECT_CONTEXT_CHANGED", "old transform archive must fail");
    bad = write; bad.transform.scene = "other.ini";
    require(UI::execute_scene_operation(bad).error_code == "PROJECT_CONTEXT_CHANGED", "wrong transform scene must fail");
    bad = write; bad.transform.actor = "missing";
    require(UI::execute_scene_operation(bad).error_code == "ACTOR_NOT_FOUND", "missing transform target must fail");
    bad = write; bad.transform.rotation = std::array<float,3>{0,std::numeric_limits<float>::infinity(),0};
    require(UI::execute_scene_operation(bad).error_code == "INVALID_ARGUMENT", "invalid mixed patch cannot partially apply");
    const auto read = UI::NativeApiRegistry::instance().dispatch({"SceneTools","read_actor",{
        {{"archive_session","camera-test-archive"},{"actor","transform-probe"}}}},{});
    require(read && read->success && read->data["actor_version"] == 2 && read->data["geometry"]["position"] == Json({0,8,-3}),
            "unmigrated readers must observe the same geometry and version without any invalid write");
    const auto* spec = UI::EditorApiRegistry::instance().find("SceneTools.set_actor_transform");
    require(spec && std::string(spec->js_wrapper).empty(), "old transform Web wrapper must be removed");
    const Json args = {"scene.ini","transform-probe",{{"archive_session","camera-test-archive"},{"rotation",{7,8,9}}}};
    require(!UI::CefEditorApiEndpoint{}.invoke("SceneTools.set_actor_transform",args,{}).success,
            "the old CEF transform route must be unavailable");
    const auto script = UI::EditorApiRegistry::instance().invoke({"SceneTools.set_actor_transform",args,
        UI::EditorApiCaller::ScriptRuntime},{});
    require(script.success && script.data["actor"]["actor_version"] == 3 &&
            script.data["actor"]["geometry"]["position"] == Json({0,8,-3}) &&
            script.data["actor"]["geometry"]["rotation"] == Json({7,8,9}),
            "ScriptRuntime must share the same typed transform authority and retain its non-Web snapshot");
}

void test_one_native_property_authority() {
    UI::SceneRequest write{UI::SceneMethod::SetActorProperties};
    write.archive_session = "camera-test-archive";
    auto& patch = write.properties;
    patch.scene = "scene.ini"; patch.actor = "transform-probe";
    patch.visible = true; patch.follow_camera = true;
    UI::ActorMechanicsPatch mechanics;
    mechanics.mass = 7.0f; mechanics.restitution = 0.25f; mechanics.damping = 0.5f;
    mechanics.body_type = "kinematic";
    mechanics.linear_lock = std::array<bool,3>{true,false,true};
    mechanics.angular_lock = std::array<bool,3>{false,true,false};
    patch.mechanics = mechanics;
    patch.camera_lock = UI::ActorCameraLockPatch{true, std::array<float,3>{1,2,3}, std::array<float,3>{4,5,6}};
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    const auto applied = UI::execute_scene_operation(write);
    require(applied.error_code.empty() && applied.properties.actor_version == 4 &&
        applied.properties.visible == true && applied.properties.follow_camera &&
        applied.properties.mechanics->mass == 7 && applied.properties.mechanics->body_type == "kinematic" &&
        applied.properties.mechanics->linear_lock == *mechanics.linear_lock &&
        applied.properties.mechanics->angular_lock == *mechanics.angular_lock &&
        applied.properties.camera_lock.enabled && applied.properties.camera_lock.position_offset == std::array<float,3>{1,2,3},
        "one mixed native property patch must update all groups and advance Actor version only once");
    require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == before.tree_revision + 1,
        "one mixed patch advances tree revision once");
    UI::SceneRequest property_read{UI::SceneMethod::ReadActorProperties};
    property_read.archive_session = write.archive_session; property_read.read_scene = patch.scene; property_read.read_actor = patch.actor;
    const auto projected = UI::execute_scene_operation(property_read).actor_read.properties;
    require(projected.actor_version == applied.properties.actor_version && projected.visible == applied.properties.visible &&
        projected.follow_camera == applied.properties.follow_camera && projected.mechanics->mass == applied.properties.mechanics->mass &&
        projected.camera_lock.position_offset == applied.properties.camera_lock.position_offset,
        "property reads and mutation replies share one native property snapshot");
    for (int kind = 0; kind < 6; ++kind) {
        auto invalid = write;
        if (kind == 0) invalid.archive_session = "old";
        if (kind == 1) invalid.properties.scene = "other.ini";
        if (kind == 2) invalid.properties.actor = "missing";
        if (kind == 3) invalid.properties.mechanics->mass = std::numeric_limits<float>::infinity();
        if (kind == 4) invalid.properties.camera_lock->position_offset = std::array<float,3>{0,std::numeric_limits<float>::infinity(),0};
        if (kind == 5) invalid.properties.animation_sources = std::vector<std::string>{"same.glb", "./same.glb"};
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid mixed properties must be rejected");
    }
    const auto read = UI::NativeApiRegistry::instance().dispatch({"SceneTools","read_actor",{
        {{"archive_session","camera-test-archive"},{"actor","transform-probe"}}}},{});
    require(read && read->success && read->data["actor_version"] == 4 && read->data["visible"] == true &&
        read->data["mechanics"]["mass"] == 7 &&
        UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == before.tree_revision + 1,
        "invalid mixed property patches must not change values or either revision");
    UI::SceneRequest partial{UI::SceneMethod::SetActorProperties};
    partial.archive_session = write.archive_session;
    partial.properties.scene = patch.scene; partial.properties.actor = patch.actor;
    partial.properties.visible = false;
    partial.properties.camera_lock = UI::ActorCameraLockPatch{false};
    const auto updated = UI::execute_scene_operation(partial);
    require(updated.error_code.empty() && updated.properties.actor_version == 5 && updated.properties.visible == false &&
        updated.properties.follow_camera && updated.properties.mechanics->mass == 7 &&
        !updated.properties.camera_lock.enabled && updated.properties.camera_lock.position_offset == std::array<float,3>{1,2,3},
        "partial patches preserve omitted groups and offsets while accepting explicit false");
    for (const auto* method : {"SceneTools.set_actor_state", "SceneTools.set_actor_physics", "SceneTools.set_actor_camera_lock"}) {
        const auto* spec = UI::EditorApiRegistry::instance().find(method);
        require(spec && std::string(spec->js_wrapper).empty() && !UI::CefEditorApiEndpoint{}.invoke(method, Json::array(), {}).success,
            "all three redundant Web property routes and wrappers must be retired");
    }
    const auto invoke = [](const char* method, Json value, UI::EditorApiCaller caller) {
        return UI::EditorApiRegistry::instance().invoke({method, {"scene.ini", "transform-probe", value}, caller}, {});
    };
    const auto state = invoke("SceneTools.set_actor_state", {{"visible",true}}, UI::EditorApiCaller::ScriptRuntime);
    require(state.success && state.data["actor"]["actor_version"] == 6 && state.data["actor"]["visible"] == true,
        "ScriptRuntime state adapter shares the property authority");
    const auto physics = invoke("SceneTools.set_actor_physics", {{"mass",2}}, UI::EditorApiCaller::ScriptRuntime);
    require(physics.success && physics.data["actor"]["actor_version"] == 7 && physics.data["actor"]["mechanics"]["mass"] == 2,
        "ScriptRuntime physics adapter shares the property authority");
    const auto lock = invoke("SceneTools.set_actor_camera_lock", {{"enabled",true}}, UI::EditorApiCaller::PythonScript);
    require(lock.success && lock.data["actor"]["actor_version"] == 8 &&
        lock.data["actor"]["camera_lock"]["position_offset"] == Json({0,-8,-2}),
        "Python camera-lock adapter shares authority and preserves default derived-offset behavior");
    require(!invoke("SceneTools.set_actor_camera_lock", {{"enabled",false}}, UI::EditorApiCaller::ScriptRuntime).success,
        "consolidation cannot widen legacy caller permissions");
}

void test_one_native_environment_authority() {
    UI::SceneRequest read{UI::SceneMethod::GetEnvironment};
    read.archive_session = "camera-test-archive";
    read.environment.scene = "scene.ini";
    const auto before = UI::execute_scene_operation(read);
    require(before.error_code.empty() && before.environment.archive_session == *read.archive_session,
        "environment reads return the scoped live native state");
    const auto tree = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision;
    auto write = read;
    write.method = UI::SceneMethod::SetEnvironment;
    write.environment.sun = UI::EnvironmentSunPatch{false, std::array<float,3>{2,3,4}};
    write.environment.grid_enabled = false;
    UI::EnvironmentPhysicsPatch physics;
    physics.gravity = std::array<float,3>{1,-8,2};
    physics.floor_y = 4.0f; physics.floor_restitution = 0.3f; physics.fixed_dt = 0.02f;
    write.environment.physics = physics;
    const auto applied = UI::execute_scene_operation(write);
    require(applied.error_code.empty() && !applied.environment.sun_enabled && !applied.environment.grid_enabled &&
        applied.environment.sun_direction == std::array<float,3>{2,3,4} &&
        applied.environment.physics.gravity == *physics.gravity && applied.environment.physics.floor_y == 4 &&
        applied.environment.physics.floor_restitution == *physics.floor_restitution && applied.environment.physics.fixed_dt == *physics.fixed_dt,
        "one mixed patch updates sun, grid and physics including explicit false");
    for (int kind = 0; kind < 6; ++kind) {
        auto invalid = write;
        invalid.environment.grid_enabled = true;
        if (kind == 0) invalid.archive_session = "old";
        if (kind == 1) invalid.environment.scene = "other.ini";
        if (kind == 2) invalid.environment.sun->direction = std::array<float,3>{0,std::numeric_limits<float>::infinity(),0};
        if (kind == 3) invalid.environment.physics->floor_y = std::numeric_limits<float>::quiet_NaN();
        if (kind == 4) invalid.environment.physics = UI::EnvironmentPhysicsPatch{};
        if (kind == 5) invalid.environment.sun = UI::EnvironmentSunPatch{};
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid mixed environment patch must be rejected before mutation");
        const auto current = UI::execute_scene_operation(read).environment;
        require(!current.grid_enabled && !current.sun_enabled && current.physics.gravity == *physics.gravity && current.physics.floor_y == 4,
            "invalid environment patches cannot partly write any group");
    }
    auto partial = read;
    partial.method = UI::SceneMethod::SetEnvironment;
    partial.environment.grid_enabled = true;
    const auto changed = UI::execute_scene_operation(partial).environment;
    require(changed.grid_enabled && !changed.sun_enabled && changed.sun_direction == applied.environment.sun_direction &&
        changed.physics.gravity == applied.environment.physics.gravity && changed.physics.fixed_dt == applied.environment.physics.fixed_dt,
        "a partial environment patch preserves all omitted fields");
    partial.environment.grid_enabled.reset();
    partial.environment.sun = UI::EnvironmentSunPatch{true, std::array<float,3>{0,0,0}};
    require(UI::execute_scene_operation(partial).environment.sun_direction == std::array<float,3>{1,1,1},
        "enabling a zero sun vector preserves the established fallback direction");
    require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == tree,
        "environment edits preserve the existing tree revision policy");
    for (const auto* method : {"get_environment", "set_environment", "sun_direction", "floor_grid", "get_physics_params", "set_physics_params"}) {
        const auto name = std::string("SceneTools.") + method;
        const auto* spec = UI::EditorApiRegistry::instance().find(name);
        require(spec && std::string(spec->js_wrapper).empty() && !UI::CefEditorApiEndpoint{}.invoke(name, Json::array(), {}).success,
            "all six redundant environment Web routes must be retired");
    }
    const auto invoke = [](const char* method, Json args, UI::EditorApiCaller caller) {
        return UI::EditorApiRegistry::instance().invoke({method, args, caller}, {});
    };
    const auto script = invoke("SceneTools.set_environment", {"scene.ini", {{"sun", {{"enabled",false}}}, {"physics",{{"floor_y",7}}}}}, UI::EditorApiCaller::ScriptRuntime);
    require(script.success && script.data["physics"]["floor_y"] == 7 && script.data.contains("authored_lighting") &&
        !UI::execute_scene_operation(read).environment.sun_enabled,
        "ScriptRuntime retains its full non-Web payload while sharing the native authority");
    const auto python = invoke("SceneTools.set_physics_params", {"scene.ini", nullptr, 8.0}, UI::EditorApiCaller::PythonScript);
    require(python.success && python.data["floor_y"] == 8 && UI::execute_scene_operation(read).environment.physics.floor_y == 8,
        "Python physics positional adapter shares the same typed write and preserves omitted gravity");
    require(invoke("SceneTools.floor_grid", {"scene.ini",false}, UI::EditorApiCaller::PythonScript).success &&
        !UI::execute_scene_operation(read).environment.grid_enabled,
        "Python grid adapter shares the native environment write");
    require(invoke("SceneTools.sun_direction", {"scene.ini",true,{3,2,1}}, UI::EditorApiCaller::PythonScript).success &&
        UI::execute_scene_operation(read).environment.sun_direction == std::array<float,3>{3,2,1},
        "Python sun adapter shares the native environment write");
    require(invoke("SceneTools.get_physics_params", {"scene.ini"}, UI::EditorApiCaller::PythonScript).data["floor_y"] == 8 &&
        invoke("SceneTools.get_environment", {"scene.ini"}, UI::EditorApiCaller::ScriptRuntime).data["physics"]["floor_y"] == 8,
        "both non-Web read shapes share the native environment snapshot");
    require(!invoke("SceneTools.set_environment", {"scene.ini", {{"grid",{{"enabled",true}}}}}, UI::EditorApiCaller::PythonScript).success &&
        !invoke("SceneTools.set_physics_params", {"scene.ini",{1,2,3}}, UI::EditorApiCaller::ScriptRuntime).success,
        "environment consolidation must not widen non-Web caller permissions");
}

void test_one_scoped_native_focus_application() {
    const auto read_cameras = [] {
        return UI::NativeApiRegistry::instance().dispatch({"SceneTools", "list_camera_views", {"scene.ini"}}, {})->data;
    };
    const auto before = read_cameras();
    const auto version = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision;
    UI::SceneRequest request{UI::SceneMethod::FocusActor};
    request.archive_session = "camera-test-archive";
    request.focus = {"scene.ini", "transform-probe", "primary"};
    const auto result = UI::execute_scene_operation(request);
    require(result.error_code.empty() && result.focus.scene == "scene.ini" &&
        result.focus.archive_session == *request.archive_session && result.focus.actor_guid == "transform-probe",
        "focus applies through the scoped native Actor identity");
    require(result.focus.camera.camera_id == "primary" && result.focus.camera.handle != 0 && result.focus.distance >= 1 &&
        result.focus.camera.position[0] == result.focus.center[0] && result.focus.camera.position[1] == result.focus.center[1] &&
        result.focus.camera.position[2] == result.focus.center[2] - result.focus.distance,
        "typed focus returns the actual applied camera pose and native handle");
    const auto after = read_cameras();
    require(after["cameras"][0]["position"] == Json(result.focus.camera.position) &&
        after["cameras"][0]["fov"] == before["cameras"][0]["fov"] && after["cameras"][1] == before["cameras"][1],
        "focus updates just the identified camera and preserves its FOV");
    for (int kind = 0; kind < 5; ++kind) {
        auto invalid = request;
        if (kind == 0) invalid.archive_session = "old-archive";
        if (kind == 1) invalid.focus.scene = "wrong.ini";
        if (kind == 2) invalid.focus.actor = "missing";
        if (kind == 3) invalid.focus.camera_id = "Primary camera";
        if (kind == 4) invalid.focus.camera_id = std::to_string(result.focus.camera.handle);
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid focus identity must reject");
        require(read_cameras() == after, "rejected focus cannot mutate any camera");
    }
    auto& registry = UI::EditorApiRegistry::instance();
    const auto* spec = registry.find("SceneTools.focus_actor");
    require(spec && std::string(spec->js_wrapper).empty(), "focus has no old Web wrapper");
    require(!UI::CefEditorApiEndpoint{}.invoke("SceneTools.focus_actor", {"scene.ini", "Transform probe", "primary"}, {}).success,
        "CEF cannot use the old focus route");
    require(registry.find("ResourceSearch.focus_actor") == nullptr, "orphan ResourceSearch focus contract must be deleted");
    const auto python = registry.invoke({"SceneTools.focus_actor", {"scene.ini", "Transform probe", "Primary camera"},
        UI::EditorApiCaller::PythonScript}, {});
    require(python.success && python.data["camera"]["position"] == after["cameras"][0]["position"] &&
        python.data["camera"]["handle"].is_number_unsigned(), "Python focus shares the same operation and keeps its own numeric adapter");
    require(!registry.invoke({"SceneTools.focus_actor", {"scene.ini", "Transform probe", "primary"},
        UI::EditorApiCaller::ScriptRuntime}, {}).success, "focus migration does not widen ScriptRuntime permissions");
    require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == version,
        "camera focus preserves the existing scene-tree revision policy");
}

void test_camera_settings_cannot_change_window_lifecycle() {
    auto& hub = Corona::SharedDataHub::instance();
    hub.drain_camera_state_updates();
    const auto active = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest request{UI::SceneMethod::SetCameraProperties};
    request.archive_session = active.archive_session;
    request.camera_properties.scene = "scene.ini";
    request.camera_properties.camera_id = "primary";
    request.camera_properties.move_speed = 0.75f;
    require(UI::execute_scene_operation(request).error_code.empty(), "camera settings must remain editable");
    const auto updates = hub.drain_camera_state_updates();
    require(updates.size() == 1 && !Corona::has_camera_state_field(updates[0].fields, Corona::CameraStateUpdateField::ViewState),
        "ordinary camera settings must not enqueue a window open/close state write");
    for (const bool open : {true, false}) {
        hub.enqueue_camera_state_update({.camera_handle = updates[0].camera_handle,
            .fields = Corona::CameraStateUpdateField::ViewState, .view_open = open});
        require(UI::execute_scene_operation(request).error_code.empty(), "settings can follow a queued lifecycle write");
        const auto merged = hub.drain_camera_state_updates();
        require(merged.size() == 1 && merged[0].view_open == open && merged[0].move_speed == 0.75f,
            "settings coalescing must preserve the accepted lifecycle state in either direction");
    }
    auto invalid = request;
    invalid.camera_properties.view_open = true;
    invalid.camera_properties.width = 123;
    require(UI::execute_scene_operation(invalid).error_code == "INVALID_ARGUMENT",
        "typed editor properties must reject lifecycle mutation atomically");
    require(hub.drain_camera_state_updates().empty(), "rejected lifecycle patch must not partially enqueue width or settings");
    auto& registry = UI::EditorApiRegistry::instance();
    for (const auto* name : {"SceneTools.open_camera_view", "SceneTools.close_camera_view"}) {
        require(registry.find(name) && std::string(registry.find(name)->js_wrapper).empty(),
            "old camera lifecycle methods must have no Web wrapper");
        require(!UI::CefEditorApiEndpoint{}.invoke(name, {"scene.ini", "primary"}, {}).success,
            "old CEF state-only camera lifecycle routes must reject");
        require(registry.invoke({name, {"scene.ini", "Primary camera"}, UI::EditorApiCaller::PythonScript}, {}).success,
            "retiring Web lifecycle routes must preserve the existing Python contract");
    }
    hub.drain_camera_state_updates();
}

void test_window_owner_publishes_camera_open_state() {
    const auto active = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::WindowRequest request{UI::WindowMethod::CreateCameraView, 0};
    request.archive_session = active.archive_session;
    request.scene_id = "scene.ini"; request.camera_id = "primary";
    require(UI::prepare_camera_window_request(request).error_code.empty(), "window state needs exact native camera authority");
    auto& views = UI::CameraViewportManager::instance();
    const auto opened = [&] {
        const auto result = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "list_camera_views", {"scene.ini"}}, {});
        require(result && result->success, "camera state projection must remain readable");
        return result->data["cameras"][0].value("view_open", false);
    };
    require(views.register_view(request.scene_id, request.camera_id, request.camera_handle, 6060, request.camera_state, request.camera_authority),
        "window owner must be able to register a camera once");
    require(opened(), "accepted window registration must publish open state without a Web toggle");
    auto& hub = Corona::SharedDataHub::instance();
    hub.drain_camera_viewport_updates();
    require(views.bind_surface(6060, reinterpret_cast<void*>(std::uintptr_t{0x1234}), 0, 0, 320, 240),
        "CameraView surface binds before queuing its update");
    const auto pending = hub.drain_camera_viewport_updates();
    require(pending.size() == 1 && pending[0].authority == request.camera_authority,
        "CameraView surface updates retain the resolved camera lifetime through the queue");
    require(views.unregister_view(6060, false) && !opened(), "native close must publish closed state without a Web toggle");
    require(views.register_view(request.scene_id, request.camera_id, request.camera_handle, 6060, request.camera_state),
        "closed camera can be reopened");
    require(views.unregister_view(6060, true) && opened(), "world retirement preserves the explicit restore preference");
    require(!views.unregister_view(6060, false) && opened(), "duplicate retirement must not change the saved open preference");
}

void test_typed_camera_creation_has_no_window_side_effect() {
    const auto active = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::WindowRequest survivor{UI::WindowMethod::ConfigureViewport, 1};
    survivor.archive_session = active.archive_session; survivor.scene_id = "scene.ini"; survivor.camera_id = "primary";
    require(UI::prepare_camera_window_request(survivor).error_code.empty(), "surviving camera authority resolves before vector growth");
    UI::SceneRequest request{UI::SceneMethod::CreateCamera};
    request.archive_session = active.archive_session;
    request.camera_create = {"scene.ini", "Typed camera probe"};
    for (int kind = 0; kind < 3; ++kind) {
        auto invalid = request;
        if (kind == 0) invalid.archive_session.reset();
        if (kind == 1) invalid.archive_session = "retired";
        if (kind == 2) invalid.camera_create.scene = "other.ini";
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid creation identity must reject");
        require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == active.tree_revision,
            "invalid camera creation must not partially change the scene");
    }
    const auto created = UI::execute_scene_operation(request);
    require(created.error_code.empty() && created.camera.archive_session == active.archive_session &&
        created.camera.scene == "scene.ini" && created.camera.name == "Typed camera probe" &&
        !created.camera.camera_id.empty() && created.camera.handle != 0 && !created.camera.view_open,
        "typed creation returns an owned camera identity without claiming its window is open");
    require(!UI::CameraViewportManager::instance().find_by_camera("scene.ini", created.camera.camera_id),
        "creating a scene object must not allocate a window surface");
    require(survivor.camera_authority->active, "camera vector growth must preserve surviving viewport authority");
    auto retiring = survivor; retiring.camera_id = created.camera.camera_id;
    require(UI::prepare_camera_window_request(retiring).error_code.empty(), "new camera authority resolves before deletion");
    request.camera_create.name = "Moved camera survivor";
    const auto later = UI::execute_scene_operation(request);
    require(later.error_code.empty(), "second temporary camera creates a vector erase-move fixture");
    auto moved = survivor; moved.camera_id = later.camera.camera_id;
    require(UI::prepare_camera_window_request(moved).error_code.empty(), "later camera authority resolves before erase-move");
    require(!UI::CefEditorApiEndpoint{}.invoke("SceneTools.create_camera_view", {"scene.ini", "legacy"}, {}).success,
        "old create-camera Web route must reject");
    const auto removed = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "delete_camera", {"scene.ini", created.camera.camera_id}}, {});
    require(removed && removed->success, "test-owned camera must be removed after creation verification");
    require(!retiring.camera_authority->active && survivor.camera_authority->active,
        "deleting one camera must retire only its commands and preserve other open viewports");
    require(moved.camera_authority->active, "vector erase-move must preserve the surviving camera's authority");
    const auto removed_later = UI::NativeApiRegistry::instance().dispatch({"SceneTools", "delete_camera", {"scene.ini", later.camera.camera_id}}, {});
    require(removed_later && removed_later->success && !moved.camera_authority->active && survivor.camera_authority->active,
        "deleting a relocated camera retires its own authority without invalidating the main viewport");
}

void test_scoped_screenshot_rejects_invalid_identity_without_mutation() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest request{UI::SceneMethod::CaptureScreenshot};
    request.archive_session = before.archive_session;
    request.screenshot = {"scene.ini", "primary", "unused.png"};
    for (int kind = 0; kind < 7; ++kind) {
        auto invalid = request;
        if (kind == 0) invalid.archive_session = "retired";
        if (kind == 1) invalid.archive_session.reset();
        if (kind == 2) invalid.screenshot.scene = "wrong.ini";
        if (kind == 3) invalid.screenshot.camera_id = "Primary camera";
        if (kind == 4) invalid.screenshot.camera_id = "missing";
        if (kind == 5) invalid.screenshot.path = "";
        if (kind == 6) invalid.screenshot.path = std::string("unsafe\0.png", 11);
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid screenshot must reject before capture or dialog");
    }
    const auto after = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    require(after.tree_revision == before.tree_revision && after.archive_session == before.archive_session,
        "rejected screenshots must not alter the scene");
    auto& registry = UI::EditorApiRegistry::instance();
    for (const auto* name : {"SceneTools.save_screenshot", "SceneTools.select_screenshot_path", "SceneTools.capture_viewport"}) {
        const auto* spec = registry.find(name);
        require(spec && std::string(spec->js_wrapper).empty(), "screenshot must not expose old Web wrappers");
        require(!UI::CefEditorApiEndpoint{}.invoke(name, Json::array(), {}).success, "CEF old screenshot routes must reject");
        require(!registry.invoke({name, Json::array(), UI::EditorApiCaller::ScriptRuntime}, {}).success,
            "screenshot migration must not widen ScriptRuntime permissions");
    }
}

void test_camera_properties_share_pose_view_and_native_authority() {
    auto& registry = UI::EditorApiRegistry::instance();
    const auto read = [&] { return registry.invoke({"SceneTools.list_camera_views", {"scene.ini"},
        UI::EditorApiCaller::PythonScript}, {}).data["cameras"]; };
    const auto before = read();
    const auto active = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest request{UI::SceneMethod::SetCameraProperties};
    request.archive_session = active.archive_session;
    auto& patch = request.camera_properties;
    patch.scene = "scene.ini"; patch.camera_id = "primary";
    patch.position = std::array<float, 3>{7, 8, -9};
    patch.fov = 58; patch.width = 320; patch.height = 240;
    patch.move_speed = 0.5f;
    const auto applied = UI::execute_scene_operation(request);
    require(applied.error_code.empty() && applied.camera.archive_session == active.archive_session &&
        applied.camera.scene == "scene.ini" && applied.camera.camera_id == "primary" &&
        applied.camera.position == *patch.position && applied.camera.fov == 58 && applied.camera.width == 320 &&
        applied.camera.height == 240 && applied.camera.move_speed == 0.5f,
        "one typed camera patch must apply pose and view fields and report actual values");
    const auto after = read();
    require(after[1] == before[1] && after[0]["forward"] == before[0]["forward"] &&
        after[0]["world_up"] == before[0]["world_up"] && after[0]["output_mode"] == before[0]["output_mode"],
        "camera patch preserves omitted pose fields, output mode and other cameras");
    for (int kind = 0; kind < 8; ++kind) {
        auto invalid = request;
        if (kind == 0) invalid.archive_session = "retired";
        if (kind == 1) invalid.archive_session.reset();
        if (kind == 2) invalid.camera_properties.scene = "wrong.ini";
        if (kind == 3) invalid.camera_properties.camera_id = "Primary camera";
        if (kind == 4) invalid.camera_properties.width = 0;
        if (kind == 5) invalid.camera_properties.move_speed = std::numeric_limits<float>::infinity();
        if (kind == 6) invalid.camera_properties.position = std::array<float, 3>{0, NAN, 0};
        if (kind == 7) invalid.camera_properties.fov = std::numeric_limits<float>::infinity();
        require(!UI::execute_scene_operation(invalid).error_code.empty(), "invalid camera patch must reject");
        require(read() == after, "invalid mixed camera patch must not partially modify pose or settings");
    }
    Corona::API::set_editor_camera_input_enabled(false);
    const auto locked = UI::execute_scene_operation(request);
    UI::SceneRequest settings{UI::SceneMethod::SetCameraProperties};
    settings.archive_session = active.archive_session;
    settings.camera_properties.scene = "scene.ini"; settings.camera_properties.camera_id = "primary";
    settings.camera_properties.move_speed = 0.75f;
    const auto configured = UI::execute_scene_operation(settings);
    const auto script = registry.invoke({"SceneTools.set_camera_pose", {"scene.ini", "Primary camera", {{"position", {1, 2, 3}}}},
        UI::EditorApiCaller::ScriptRuntime}, {});
    Corona::API::set_editor_camera_input_enabled(true);
    require(locked.error_code == "EDITOR_INPUT_LOCKED", "native runtime input ownership must reject editor pose writes");
    require(configured.error_code.empty() && configured.camera.move_speed == 0.75f,
        "window configuration remains available while runtime owns camera input");
    require(script.success && script.data["camera"]["position"] == Json::array({1, 2, 3}),
        "ScriptRuntime pose adapter must share authority without acquiring editor input ownership");
    require(!registry.invoke({"SceneTools.set_camera_pose", {"scene.ini", "primary", Json::object()},
        UI::EditorApiCaller::PythonScript}, {}).success, "pose migration must preserve ScriptRuntime-only non-Web permissions");
    const auto python = registry.invoke({"SceneTools.update_camera_view", {"scene.ini", "Primary camera", {{"width", 400}, {"view_open", false}}},
        UI::EditorApiCaller::PythonScript}, {});
    require(python.success && python.data["camera"]["width"] == 400, "Python view adapter shares camera properties");
    for (const auto* name : {"SceneTools.set_camera_pose", "SceneTools.update_camera_view"}) {
        require(std::string(registry.find(name)->js_wrapper).empty(), "old camera Web wrappers must be removed");
        require(!UI::CefEditorApiEndpoint{}.invoke(name, {"scene.ini", "primary", Json::object()}, {}).success,
            "old camera CEF routes must reject rather than forward to V8");
    }
    require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == active.tree_revision,
        "camera property writes preserve the existing tree revision policy");
}

void test_viewport_configuration_shares_identity_and_applies_atomically() {
    UI::WindowRequest request{UI::WindowMethod::ConfigureViewport, 1};
    request.archive_session = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.archive_session;
    request.scene_id = "scene.ini"; request.camera_id = "primary";
    require(UI::prepare_camera_window_request(request).error_code.empty(), "viewport resolves the exact native camera");
    request.viewport.emplace();
    request.viewport->mode = "stereo3d";
    request.viewport->calibration = UI::ViewportLenticularCalibration{18, 0.2, 4, 2};
    request.viewport->bounds = UI::ViewportBounds{10, 20, 800, 600, 1600, 1200};
    UI::BrowserTab tab;
    tab.open = true;
    const auto applied = UI::apply_viewport_configuration(request, tab);
    require(applied.error_code.empty() && applied.viewport && applied.viewport->mode == "stereo3d" &&
        tab.web_camera_handle == request.camera_handle && tab.web_camera_x == 10 && tab.web_camera_width == 800 &&
        tab.web_camera_render_width == 1600, "one viewport patch configures all three former Fast operations");
    const auto state = Corona::SharedDataHub::instance().viewport_ui_state(request.camera_handle);
    require(state.mode == Corona::ViewportUiMode::Stereo3D && state.calibration.lenticular_pitch == 6 &&
        std::abs(state.calibration.slant_angle_radians + 0.2f) < 1e-6f && state.calibration.parallax_scale == 2,
        "native lenticular conversion preserves the existing warp convention");
    for (int kind = 0; kind < 7; ++kind) {
        auto invalid = request;
        invalid.viewport->mode = "flat2d";
        if (kind == 0) invalid.viewport->bounds->width = 0;
        if (kind == 1) invalid.viewport->bounds->x = -1;
        if (kind == 2) invalid.viewport->calibration->pe = 0;
        if (kind == 3) invalid.viewport->calibration->angle = NAN;
        if (kind == 4) invalid.viewport->calibration->offset = std::numeric_limits<double>::max();
        if (kind == 5) invalid.camera_authority.reset();
        if (kind == 6) invalid.viewport->mode = "invalid";
        require(!UI::apply_viewport_configuration(invalid, tab).error_code.empty(), "invalid viewport patch rejects");
        const auto after = Corona::SharedDataHub::instance().viewport_ui_state(request.camera_handle);
        require(after.mode == state.mode && after.calibration.lenticular_pitch == state.calibration.lenticular_pitch &&
            tab.web_camera_width == 800, "mixed invalid patches cannot partially change mode, calibration or bounds");
    }
    auto partial = request;
    partial.viewport->mode = "flat2d"; partial.viewport->calibration.reset(); partial.viewport->bounds.reset();
    require(UI::apply_viewport_configuration(partial, tab).error_code.empty() && tab.web_camera_width == 800 &&
        Corona::SharedDataHub::instance().viewport_ui_state(request.camera_handle).calibration.lenticular_pitch == 6,
        "partial viewport patches preserve omitted configuration");
    auto retired = request;
    retired.camera_authority = std::make_shared<UI::CameraWindowAuthority>(); retired.camera_authority->active = false;
    require(UI::apply_viewport_configuration(retired, tab).error_code == "PROJECT_CONTEXT_CHANGED",
        "late viewport operations cannot access a retired archive");
    tab.open = false;
    require(UI::apply_viewport_configuration(request, tab).error_code == "WINDOW_CLOSED", "closed viewport cannot mutate state");
}

void test_pointer_configuration_owns_one_state_and_retires_with_its_context() {
    UI::WindowRequest request{UI::WindowMethod::ConfigureViewport, 1};
    request.archive_session = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.archive_session;
    request.scene_id = "scene.ini"; request.camera_id = "primary"; request.context_id = 101;
    require(UI::prepare_camera_window_request(request).error_code.empty(), "pointer resolves exact scoped camera");
    request.viewport.emplace(); request.viewport->mode = "stereo3d";
    request.viewport->bounds = UI::ViewportBounds{0, 0, 800, 600, 800, 600};
    request.viewport->pointer = UI::ViewportPointer{12, 34, "grab"};
    UI::BrowserTab tab; tab.open = true; tab.docking_pos = "main";
    auto& hub = Corona::SharedDataHub::instance();
    auto result = UI::apply_viewport_configuration(request, tab);
    require(result.error_code.empty() && result.viewport->pointer.cursor == "grab" && result.viewport->system_cursor_hidden,
        "pointer and system cursor share one actual accepted configuration");
    const auto state = hub.viewport_ui_state(request.camera_handle);
    require(state.pointer.x == 12 && state.pointer.y == 34 && state.pointer.owner_context == 101 &&
        state.pointer.cursor_shape == Corona::ViewportUiCursorShape::Grab && tab.hide_system_cursor.load(),
        "rendering reads the typed pointer state directly without another command queue");
    for (int kind = 0; kind < 4; ++kind) {
        auto invalid = request; invalid.viewport->mode = "flat2d";
        if (kind == 0) invalid.viewport->pointer->x = NAN;
        if (kind == 1) invalid.viewport->pointer->y = -1;
        if (kind == 2) invalid.viewport->pointer->cursor = "invalid";
        if (kind == 3) invalid.context_id = 0;
        require(!UI::apply_viewport_configuration(invalid, tab).error_code.empty(), "invalid pointer rejects entire patch");
        const auto after = hub.viewport_ui_state(request.camera_handle);
        require(after.mode == state.mode && after.pointer.x == 12 && tab.hide_system_cursor.load(), "invalid pointer has no partial side effects");
    }
    auto replacement = request; replacement.context_id = 202; replacement.viewport->pointer->cursor = "crosshair";
    require(UI::apply_viewport_configuration(replacement, tab).error_code.empty(), "new context can own its pointer");
    UI::release_viewport_pointer(tab, 101);
    require(tab.hide_system_cursor.load() && hub.viewport_ui_state(request.camera_handle).pointer.owner_context == 202,
        "late old-context cleanup cannot clear a newer pointer in the same tab");
    UI::BrowserTab peer; peer.open = true; peer.docking_pos = "main";
    auto other = replacement; other.context_id = 303;
    require(UI::apply_viewport_configuration(other, peer).error_code.empty(), "second surface takes its own pointer ownership");
    UI::refresh_viewport_state(tab);
    require(!tab.hide_system_cursor.load() && peer.hide_system_cursor.load() &&
        hub.viewport_ui_state(request.camera_handle).pointer.owner_context == 303, "old surface cleanup preserves the new surface cursor");
    auto retired = std::make_shared<UI::CameraWindowAuthority>(); retired->active = false;
    peer.viewport_pointer_authority = retired;
    UI::refresh_viewport_state(peer);
    require(!peer.hide_system_cursor.load() && hub.viewport_ui_state(request.camera_handle).pointer.cursor_shape == Corona::ViewportUiCursorShape::Hidden,
        "archive retirement restores the system cursor and removes stale pointer state");
    auto flat = request; flat.viewport->mode = "flat2d";
    require(UI::apply_viewport_configuration(flat, tab).error_code.empty() && !tab.hide_system_cursor.load(), "2D UI never hides the system cursor");
    UI::release_viewport_pointer(tab, 101);
}

void test_owned_pick_requests_and_coordinate_mapping() {
    const auto mapped = UI::map_viewport_pick_point(30, 20, 100, 50, 800, 600);
    require(mapped.valid && mapped.inside && mapped.x == 240 && mapped.y == 240, "one pixel mapping must serve Web and Python");
    require(!UI::map_viewport_pick_point(0, 0, std::numeric_limits<double>::infinity(), 50, 800, 600).valid,
        "non-finite dimensions must reject before casts");
    for (const double x : {-0.01, 100.0}) require(!UI::map_viewport_pick_point(x, 0, 100, 50, 800, 600).inside,
        "negative and right-edge coordinates must miss without truncation");
    UI::ViewportPickRequests requests;
    UI::ViewportPickRequest request;
    request.camera.camera_handle = 19; request.camera.scene_id = "scene.ini";
    request.camera.browser_id = 10; request.camera.context_id = 100;
    const auto now = UI::ViewportPickRequests::Clock::now();
    int called = 0;
    const auto done = [&](const auto&, const auto&) { ++called; };
    const auto first = requests.add(request, done, now);
    const auto second = requests.add(request, done, now);
    require(first != second, "native request IDs must be unique even in one context");
    require(!requests.complete({20, "scene.ini", first, "success"}) && !requests.complete({19, "other.ini", first, "success"}),
        "foreign camera/scene completion must not consume the request");
    require(requests.complete({19, "scene.ini", first, "miss"}) && called == 1, "typed completion must deliver exactly once");
    require(!requests.complete({19, "scene.ini", first, "miss"}) && called == 1, "duplicate GPU result must not redeliver");
    requests.expire(now + UI::ViewportPickRequests::timeout);
    require(called == 2 && requests.pending_count() == 0, "deadline must complete remaining request once");
    require(!requests.complete({19, "scene.ini", second, "success"}), "late GPU result after timeout must be ignored");
    const auto old = requests.add(request, done);
    request.camera.context_id = 101;
    const auto peer = requests.add(request, done);
    requests.cancel_context(100);
    require(requests.pending_count() == 1 && !requests.complete({19, "scene.ini", old, "success"}), "reload must cancel only its own requests");
    require(requests.complete({19, "scene.ini", peer, "success"}) && called == 3, "new context must retain its request");
    requests.add(request, done);
    requests.cancel_browser(10);
    require(requests.pending_count() == 0 && called == 3, "window close must release pending readbacks without callbacks");
}

void test_pick_results_resolve_authoritative_actor_and_scope() {
    UI::ViewportPickRequest request;
    request.camera.archive_session = "camera-test-archive";
    request.camera.scene_id = "scene.ini"; request.camera.camera_id = "secondary";
    require(UI::prepare_camera_window_request(request.camera).error_code.empty(), "pick camera must resolve exact identity");
    UI::NativeRequest tree; tree.module = "SceneTools"; tree.function = "list_scene_tree"; tree.args = {"scene.ini"};
    const auto values = UI::NativeApiRegistry::instance().dispatch(tree, {});
    require(values && values->success && !values->data["actors"].empty(), "pick fixture must have a real actor");
    const auto handle = values->data["actors"][0]["handle"].get<std::uintptr_t>();
    Corona::ActorPickCompletion hit{request.camera.camera_handle, "scene.ini", "native-id", "success", handle, 7, 8, UINT32_MAX - 1};
    const auto result = UI::finish_viewport_pick(request, hit);
    require(result.error_code.empty() && result.status == "success" && result.actor_handle == std::to_string(handle) &&
        result.actor_guid == "transform-probe" && result.actor_name == "Transform probe" && result.node_index == UINT32_MAX - 1,
        "GPU handle must resolve authoritative Actor identity without Number or tree lookup in Web");
    hit.actor_handle = 0;
    require(UI::finish_viewport_pick(request, hit).status == "miss", "empty GPU success is a miss");
    hit.status = "timeout";
    require(UI::finish_viewport_pick(request, hit).error_code == "PICK_TIMEOUT", "timeout must reject the Promise");
    request.camera.archive_session = "old";
    require(UI::finish_viewport_pick(request, hit).error_code == "PROJECT_CONTEXT_CHANGED", "late result cannot enter a new archive");
    UI::CefEditorApiEndpoint cef;
    require(!cef.invoke("SceneTools.pick_actor_at_pixel", {"scene.ini",0,0,100,100}, {}).success,
        "old synchronous pick must not remain a second Web route");
    require(!UI::find_editor_api_event("SceneTools.actorPickResult"), "retired pick event must not remain subscribable");
}

void test_script_input_queue_has_one_owner_and_drops_retired_scopes() {
    UI::drain_input_events();
    bool active = true;
    UI::InputEvent key{}; key.type = 0; key.arg0 = "KeyJ"; key.arg2 = "d";
    UI::enqueue_input_event(key);
    UI::InputEvent mouse{}; mouse.type = 2; mouse.arg0 = "click"; mouse.arg1 = "LeftButton";
    mouse.arg3 = 70; mouse.arg4 = 80; mouse.viewport_x = 5; mouse.viewport_y = 6;
    mouse.viewport_width = 400; mouse.viewport_height = 200; mouse.picked_actor = "Picked";
    UI::enqueue_input_event(mouse, [&] { return active; });
    const auto events = UI::drain_input_events();
    require(events.size() == 2 && events[0].arg0 == "KeyJ" && events[0].arg2 == "d", "SDL and Web share ordered typed input");
    require(events[1].picked_actor == "Picked" && events[1].viewport_x == 5 && events[1].viewport_height == 200,
        "mouse viewport/picked Actor values must survive the native queue");
    require(UI::drain_input_events().empty(), "input must be consumed once");
    UI::enqueue_input_event(mouse, [&] { return active; }); active = false;
    UI::enqueue_input_event(key);
    const auto after_retirement = UI::drain_input_events();
    require(after_retirement.size() == 1 && after_retirement[0].type == 0, "retired Web work must be dropped without dropping SDL input");
    UI::BrowserInputHandler keyboard;
    SDL_Event native_key{};
    native_key.type = SDL_EVENT_KEY_DOWN; native_key.key.key = SDLK_D;
    native_key.key.scancode = SDL_SCANCODE_D; native_key.key.mod = SDL_KMOD_SHIFT;
    keyboard.process_sdl_key_event(native_key);
    native_key.type = SDL_EVENT_KEY_UP;
    keyboard.process_sdl_key_event(native_key);
    const auto sdl = UI::drain_input_events();
    require(sdl.size() == 2 && sdl[0].type == 0 && sdl[1].type == 1 && sdl[0].arg0 == "KeyD" && sdl[1].arg0 == "KeyD",
        "real SDL keyboard processing must enqueue exactly one down and one up without a Web owner");
    UI::CefEditorApiEndpoint endpoint;
    for (const auto* method : {"Scripts.key_event", "Scripts.key_release", "Scripts.mouse_event"})
        require(!endpoint.invoke(method, Json::array(), {}).success, "retired manifest input must be unavailable");
}

int main() {
    try {
        test_identity_lifetime();
        test_script_input_queue_has_one_owner_and_drops_retired_scopes();
        auto& kernel = Corona::Kernel::KernelContext::instance();
        require(kernel.initialize(), "kernel initialization failed");
        UI::register_builtin_native_api_handlers();
        UI::load_native_editor_scene_for_test(snapshot());
        test_resource_import_results_and_modal_retirement();
        test_audio_adapters_publish_once_with_exact_identity();
        test_native_scene_values_and_non_web_adapters();
        test_owned_actor_index_projection();
        test_owned_actor_property_projection();
        test_owned_camera_projection();
        test_owned_scene_tree_projection();
        test_one_native_transform_authority();
        test_one_native_property_authority();
        test_one_native_environment_authority();
        test_one_scoped_native_focus_application();
        test_camera_settings_cannot_change_window_lifecycle();
        test_window_owner_publishes_camera_open_state();
        test_scoped_screenshot_rejects_invalid_identity_without_mutation();
        test_camera_properties_share_pose_view_and_native_authority();
        test_viewport_configuration_shares_identity_and_applies_atomically();
        test_pointer_configuration_owns_one_state_and_retires_with_its_context();
        test_camera_release_discards_queued_viewport_updates();
        test_deferred_viewports_use_the_original_camera_authority();
        test_cached_web_viewport_binding_retires_with_its_camera();
        test_owned_pick_requests_and_coordinate_mapping();
        test_pick_results_resolve_authoritative_actor_and_scope();
        test_typed_camera_creation_has_no_window_side_effect();
        UI::WindowRequest request{UI::WindowMethod::CreateCameraView, 1};
        request.archive_session = "camera-test-archive";
        request.scene_id = "scene.ini";
        request.camera_id = "secondary";
        require(UI::prepare_camera_window_request(request).error_code.empty(), "native camera identity must resolve");
        require(request.camera_handle != 0 && request.camera_authority && request.camera_authority->active,
                "camera authority must contain the native handle and an active lifetime lease");
        const auto original_authority = request.camera_authority;
        auto invalid = request;
        invalid.archive_session = "old-world";
        require(UI::prepare_camera_window_request(invalid).error_code == "PROJECT_CONTEXT_CHANGED", "old archive must be rejected");
        invalid = request; invalid.scene_id = "other.ini";
        require(UI::prepare_camera_window_request(invalid).error_code == "PROJECT_CONTEXT_CHANGED", "wrong scene must be rejected");
        invalid = request; invalid.camera_id = "Secondary camera";
        require(UI::prepare_camera_window_request(invalid).error_code == "CAMERA_NOT_FOUND", "camera name must not substitute for exact identity");
        invalid.camera_id = std::to_string(request.camera_handle);
        require(UI::prepare_camera_window_request(invalid).error_code == "CAMERA_NOT_FOUND", "camera handle must not substitute for identity");
        const auto cameras = UI::NativeApiRegistry::instance().dispatch({"SceneTools","list_camera_views",{"scene.ini"}},{});
        require(cameras && cameras->success, "legacy scene adapter must still provide camera metadata");
        require(cameras->data["cameras"][0].value("archive_session", "") == request.archive_session,
                "all camera snapshot producers must provide an archive identity for window creation");
        const auto removed = UI::NativeApiRegistry::instance().dispatch({"SceneTools","delete_camera",{"scene.ini","secondary"}},{});
        require(removed && removed->success && !original_authority->active, "camera removal must retire previously resolved work");
        require(UI::prepare_camera_window_request(request).error_code == "CAMERA_NOT_FOUND", "deleted camera must not resolve");
        request.camera_id = "primary";
        require(UI::prepare_camera_window_request(request).error_code.empty(), "remaining camera must resolve with fresh authority");
        const auto before_reload = request.camera_authority;
        UI::load_native_editor_scene_for_test(snapshot());
        require(!before_reload->active, "scene replacement must retire even a reused archive/session identity");
        require(UI::prepare_camera_window_request(request).error_code.empty() && request.camera_authority != before_reload,
                "recreated scene must have a fresh native lifetime authority");
        UI::reset_native_editor_scene_for_test();
        require(!request.camera_authority->active, "scene shutdown must retire queued camera work");
        require(UI::prepare_camera_window_request(request).error_code == "PROJECT_CONTEXT_CHANGED", "absent scene must be rejected");
        UI::SceneRequest empty_read{UI::SceneMethod::ReadActorIndex};
        empty_read.read_scene = "scene.ini"; empty_read.archive_session = "camera-test-archive";
        require(UI::execute_scene_operation(empty_read).error_code == "PROJECT_CONTEXT_CHANGED",
                "an actor index cannot create an absent editor scene");
        empty_read.method = UI::SceneMethod::ReadActorProperties; empty_read.read_actor = "transform-probe";
        require(UI::execute_scene_operation(empty_read).error_code == "PROJECT_CONTEXT_CHANGED",
                "Actor property reads cannot create an absent editor scene");
        empty_read.method = UI::SceneMethod::ReadCameras;
        require(UI::execute_scene_operation(empty_read).error_code == "PROJECT_CONTEXT_CHANGED",
                "camera reads cannot create an absent editor scene");
        empty_read.method = UI::SceneMethod::ReadTree;
        require(UI::execute_scene_operation(empty_read).error_code == "PROJECT_CONTEXT_CHANGED",
                "tree reads cannot create an absent editor scene");
        require(UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.archive_session.empty(),
                "an empty scene read cannot lazily create an editor scene");
        kernel.shutdown();
        std::cout << "CameraWindowIdentityTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CameraWindowIdentityTests failed: " << error.what() << '\n';
        return 1;
    }
}
