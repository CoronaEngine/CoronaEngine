#include <corona/kernel/core/kernel_context.h>
#include <cstdlib>
#include <functional>
#include <corona/events/acoustics_system_events.h>
#include <corona/engine/engine_runtime_api.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/optics/optics_commands.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/script/python_runtime_coordinator.h>
#include "cef/cef_editor_native_api_registry.h"
#include "cef/script_runtime_api.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/cef_window_operations.h"
#include "cef/cef_scene_operations.h"
#include "cef/scene_tree_actor.h"
#include "cef/cef_viewport_pick.h"
#include "cef/viewport_image_input.h"
#include "cef/cef_script_input.h"
#include "cef/browser_ui.h"
#include "editor_api/cef_editor_api.h"
#include "editor_api/editor_dispatch.h"
#include "optics/camera_viewport_updates.h"

#include <filesystem>
#include <future>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace UI = Corona::Systems::UI;
using Json = nlohmann::json;


void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_native_script_service_wait_does_not_hold_editor_state(bool queued_before_owner_wait) {
    namespace Dispatch = UI::Detail;
    namespace Python = Corona::Script::Python;
    using namespace std::chrono_literals;
    const auto owner = std::this_thread::get_id();
    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::deque<std::function<void()>> tasks;
    std::atomic<bool> request_seen{false};
    std::atomic<bool> consumer_done{false};
    Dispatch::set_editor_dispatch_hooks({
        .is_owner_thread = [owner] { return std::this_thread::get_id() == owner; },
        .post_to_owner = [&](std::function<void()> task) {
            { std::lock_guard lock(queue_mutex); tasks.push_back(std::move(task)); }
            queue_changed.notify_all();
            return true;
        }});
    require(Dispatch::on_editor_owner_thread(), "dispatch regression must establish its owner");
    Python::PythonRuntimeCoordinator coordinator;
    auto* previous = Python::active_python_runtime_coordinator();
    Python::install_active_python_runtime_coordinator(&coordinator);
    std::thread consumer([&] {
        if (auto request = coordinator.wait_pop(5s)) {
            request_seen = true;
            queue_changed.notify_all();
            // FileScripts restoration calls back into the actual scene registry.
            // A Native handler holding editor_state_mutex while waiting for this
            // consumer prevents the callback from completing until service timeout.
            auto callback = UI::script_scene_read_active_scene(
                UI::NativeRequest{}, UI::NativeContext{UI::EditorApiCaller::ScriptRuntime});
            auto response = callback.success ? Python::PythonRuntimeResponse::success()
                                             : Python::PythonRuntimeResponse::failure(callback.error);
            response.service_result = std::make_shared<const Json>(
                Json{{"status", "stopped"}, {"restored", true}});
            coordinator.complete(request->request_id, std::move(response));
        }
        consumer_done = true;
    });
    std::future<UI::NativeResult> native;
    const auto start_native = [&] {
        native = std::async(std::launch::async, [&] {
            // What a Native caller used to do through the manifest: dispatch to the
            // editor owner thread first, and never take the Python-callback handoff.
            const auto route = Dispatch::decide_editor_dispatch_route(
                Dispatch::on_editor_owner_thread(),
                Dispatch::editor_thread_waiting_for_python(),
                Dispatch::editor_owner_thread_seen());
            Dispatch::EditorDispatchReport report;
            auto outcome = Dispatch::call_editor_dispatch(
                [] {
                    return UI::invoke_python_script_service(
                        {"FileScripts", "stop_file",
                         Json::array({Json{{"runId", "dispatch-regression"}}})});
                },
                route, 5s, &report, /*allow_python_handoff=*/false);
            return outcome ? *outcome : UI::native_failure("dispatch did not run", 503);
        });
    };
    if (queued_before_owner_wait) {
        start_native();
        std::unique_lock lock(queue_mutex);
        queue_changed.wait_for(lock, 1s, [&] { return !tasks.empty(); });
    }
    bool overtook_owner_wait = false;
    {
        Dispatch::EditorWaitingForPythonScope unrelated_owner_request;
        if (!queued_before_owner_wait) start_native();
        std::unique_lock lock(queue_mutex);
        queue_changed.wait_for(lock, 1s, [&] { return !tasks.empty() || request_seen.load(); });
        // Also cover a request already in the owner queue when its Python wait
        // starts: the pending Native request must not use the callback takeover.
        if (queued_before_owner_wait) queue_changed.wait_for(lock, 100ms, [&] { return request_seen.load(); });
        overtook_owner_wait = request_seen;
    }
    const auto deadline = std::chrono::steady_clock::now() + 6s;
    while ((!consumer_done || native.wait_for(0ms) != std::future_status::ready) &&
           std::chrono::steady_clock::now() < deadline) {
        std::function<void()> task;
        {
            std::lock_guard lock(queue_mutex);
            if (!tasks.empty()) { task = std::move(tasks.front()); tasks.pop_front(); }
        }
        if (task) task();
        else std::this_thread::sleep_for(1ms);
    }
    consumer.join();
    auto result = native.get();
    // Flush a queued task already completed through the callback handoff.
    for (auto& task : tasks) task();
    Python::install_active_python_runtime_coordinator(previous);
    Dispatch::clear_editor_dispatch_hooks();
    require(!overtook_owner_wait, "Native service request must remain queued during an unrelated owner Python wait");
    require(result.success && result.data.value("restored", false),
            "Native Scripts.stop must finish its Python scene callback without holding the editor state lock");
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
    require(plays.size() == 1 && stops.size() == 1, "one event per typed action");
    for (const auto& event : plays) require(event.resource_id == request.audio.resource_id && event.loop && !event.acoustics_handle,
        "play adapters preserve full uint64 resource ID and loop flag");
    for (const auto& event : stops) require(event.resource_id == request.audio.resource_id && !event.acoustics_handle,
        "stop adapters address the same global resource");
    request.audio.resource_id = 0;
    require(UI::execute_scene_operation(request).error_code == "INVALID_ARGUMENT", "typed audio rejects zero");
    bus->unsubscribe(playing); bus->unsubscribe(stopping);
}

void test_owned_actor_index_projection() {
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest read{UI::SceneMethod::ReadScene};
    read.scene_read = {UI::ActorProjection::Index, false};
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
    const auto read = UI::script_scene_read_actor(
        UI::NativeRequest{{}, {},
                          Json::array({Json{{"archive_session", "camera-test-archive"},
                                            {"actor", "transform-probe"}}})},
        UI::NativeContext{});
    require(read.success && read.data["actor_version"] == 2 && read.data["geometry"]["position"] == Json({0,8,-3}),
            "unmigrated readers must observe the same geometry and version without any invalid write");
    const Json args = {"scene.ini","transform-probe",{{"archive_session","camera-test-archive"},{"rotation",{7,8,9}}}};
    const auto script = UI::script_scene_set_actor_transform(
        UI::NativeRequest{{}, {}, args}, UI::NativeContext{UI::EditorApiCaller::ScriptRuntime});
    require(script.success && script.data["actor"]["actor_version"] == 3 &&
            script.data["actor"]["geometry"]["position"] == Json({0,8,-3}) &&
            script.data["actor"]["geometry"]["rotation"] == Json({7,8,9}),
            "ScriptRuntime must share the same typed transform authority and retain its non-Web snapshot");
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
    request.viewport->pointer = UI::ViewportPointer{400, 300, "grab"};
    UI::BrowserTab tab; tab.open = true; tab.docking_pos = "main";
    auto& hub = Corona::SharedDataHub::instance();
    std::optional<UI::InputDetail::ImagePoint> image_point;
    {
        const auto camera = hub.camera_storage().acquire_read(request.camera_handle);
        image_point = UI::InputDetail::map_viewport_image_point(*camera, 400, 300, 800, 600);
    }
    require(image_point.has_value(), "center pointer lies inside the current camera image");
    auto result = UI::apply_viewport_configuration(request, tab);
    require(result.error_code.empty() && result.viewport->pointer.cursor == "grab" && result.viewport->system_cursor_hidden,
        "pointer and system cursor share one actual accepted configuration");
    const auto state = hub.viewport_ui_state(request.camera_handle);
    require(state.pointer.x == static_cast<float>(image_point->x) && state.pointer.y == static_cast<float>(image_point->y) && state.pointer.owner_context == 101 &&
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
        require(after.mode == state.mode && after.pointer.x == state.pointer.x && tab.hide_system_cursor.load(), "invalid pointer has no partial side effects");
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
    Corona::CameraDevice embedded;
    embedded.width = 1920; embedded.height = 1080;
    embedded.viewport_rect_active = true; embedded.view_open = false;
    embedded.view_width = 1536; embedded.view_height = 825;
    const auto edge = UI::map_viewport_pick_point(34, 0, 1536, 825, embedded);
    require(edge.valid && edge.inside && edge.x == 0 && edge.y == 0,
        "live pick coordinates must start at the compositor image edge");
    require(!UI::map_viewport_pick_point(33, 400, 1536, 825, embedded).inside,
        "embedded image black bars must not submit GPU picks");
    Corona::CameraDevice camera;
    camera.width = 800; camera.height = 600;
    const auto mapped = UI::map_viewport_pick_point(30, 20, 100, 50, camera);
    require(mapped.valid && mapped.inside && mapped.x == 240 && mapped.y == 240, "one pixel mapping must serve Web and Python");
    require(!UI::map_viewport_pick_point(0, 0, std::numeric_limits<double>::infinity(), 50, camera).valid,
        "non-finite dimensions must reject before casts");
    for (const double x : {-0.01, 100.0}) require(!UI::map_viewport_pick_point(x, 0, 100, 50, camera).inside,
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
}

void test_optics_queue_merges_independent_fields() {
    Corona::OpticsCommandChannel::Buffer commands;
    Corona::OpticsUpdateCommand material{};
    material.optics_handle = 1;
    material.fields = Corona::OpticsField::Metallic | Corona::OpticsField::Visible;
    material.metallic = 0.9f;
    material.visible = false;
    commands.push(material);
    Corona::OpticsUpdateCommand roughness{};
    roughness.optics_handle = 1;
    roughness.fields = Corona::OpticsField::Roughness;
    roughness.roughness = 0.25f;
    commands.push(roughness);
    material.metallic = 0.95f;
    commands.push(material);
    const auto merged = commands.drain();
    require(merged.size() == 1 && merged[0].metallic == 0.95f &&
                merged[0].roughness == 0.25f && !merged[0].visible &&
                Corona::has_optics_field(merged[0].fields, Corona::OpticsField::Roughness),
            "independent Optics setters must retain all fields and use the latest value per field");
    roughness.geometry_handle = 17;
    commands.push(roughness);
    material.geometry_handle = 18;
    commands.push(material);
    const auto rebound = commands.drain();
    require(rebound.size() == 1 && rebound[0].metallic == 0.95f &&
                rebound[0].roughness == 0.25f && !rebound[0].visible &&
                Corona::has_optics_field(rebound[0].fields, Corona::OpticsField::Roughness),
            "independent scalar fields must survive a change of geometry guard");

    const auto previous_overrides = std::make_shared<const Corona::Materials::Overrides>();
    const auto previous_principled = std::make_shared<const Corona::Materials::Principled>();
    const auto next_principled = std::make_shared<const Corona::Materials::Principled>();
    Corona::OpticsUpdateCommand previous{};
    previous.optics_handle = 1; previous.geometry_handle = 17;
    previous.fields = Corona::OpticsField::MaterialOverrides | Corona::OpticsField::Principled;
    previous.material_overrides = previous_overrides; previous.principled = previous_principled;
    Corona::OpticsUpdateCommand next{};
    next.optics_handle = 1; next.geometry_handle = 18;
    next.fields = Corona::OpticsField::Principled; next.principled = next_principled;
    commands.push(roughness); commands.push(previous); commands.push(next);
    const auto owned = commands.drain();
    require(owned.size() == 1 && owned[0].geometry_handle == 18 &&
                owned[0].principled == next_principled && !owned[0].material_overrides &&
                !Corona::has_optics_field(owned[0].fields, Corona::OpticsField::MaterialOverrides) &&
                Corona::has_optics_field(owned[0].fields, Corona::OpticsField::Roughness),
            "new material ownership must discard old payloads while preserving independent scalars");
    Corona::OpticsDevice device;
    device.geometry_handle = 17; device.principled = previous_principled;
    Corona::apply_optics_update(device, owned[0]);
    require(device.principled == previous_principled && device.roughness == 0.25f,
            "a mismatched material guard must reject payloads without dropping independent scalars");
    device.geometry_handle = 18;
    Corona::apply_optics_update(device, owned[0]);
    require(device.principled == next_principled, "matching geometry must receive its material payload");
    next.principled.reset(); commands.push(next);
    const auto cleared = commands.drain();
    Corona::apply_optics_update(device, cleared.front());
    require(!device.principled, "a flagged null payload must explicitly clear the current material");
}

void test_render_settings_queue_owns_values() {
    auto& hub = Corona::SharedDataHub::instance();
    hub.drain_camera_state_updates();
    for (const auto* family : {"pt", "restir", "reblur", "relax"}) {
        UI::SceneRequest request{UI::SceneMethod::SetCameraRenderSettings};
        request.archive_session = "camera-test-archive";
        request.camera_render_settings.scene = "scene.ini";
        request.camera_render_settings.camera_id = "primary";
        request.camera_render_settings.family = family;
        const Json accepted{{"max_history", 12}, {"enabled", false}};
        request.camera_render_settings.settings = accepted;
        const auto reply = UI::execute_scene_operation(request);
        require(reply.error_code.empty() && reply.camera_render_settings.pending, "render settings accept a scoped object patch");
        request.camera_render_settings.settings["max_history"] = 99;
        const auto commands = hub.drain_camera_state_updates();
        require(commands.size() == 1, "render settings enqueue once");
        const auto& command = commands.front();
        const Json value = std::string_view(family) == "pt" ? Json(command.vision_pt_settings) :
            std::string_view(family) == "restir" ? Json(command.vision_restir_settings) :
            std::string_view(family) == "reblur" ? Json(command.vision_reblur_settings) : Json(command.vision_relax_settings);
        require(value.is_object() && value == accepted,
            "render settings command must own the original object without JSON string encoding");
        request.method = UI::SceneMethod::ReadCameraRenderSettings;
        require(UI::execute_scene_operation(request).camera_render_settings.settings == accepted,
            "requested render settings remain readable after drain and caller mutation");
    }
}

void test_consolidated_queries_and_atomic_names() {
    UI::load_native_editor_scene_for_test(snapshot());
    const auto before = UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active;
    UI::SceneRequest query{UI::SceneMethod::ReadScene};
    query.archive_session = before.archive_session;
    query.read_scene = before.scene;
    query.scene_read = {UI::ActorProjection::Index, true};
    const auto combined = UI::execute_scene_operation(query);
    require(combined.error_code.empty() && combined.actor_index.actors.size() == 1 &&
            combined.camera_list.cameras.size() == 2 && combined.active.tree_revision == before.tree_revision,
            "one scoped scene query must return the requested actor index and cameras");
    query.scene_read = {UI::ActorProjection::None, true};
    const auto cameras_only = UI::execute_scene_operation(query);
    require(cameras_only.error_code.empty() && cameras_only.actor_index.actors.empty() &&
            cameras_only.actor_context.actors.empty() && cameras_only.tree.actors.empty(),
            "camera-only queries must not build an Actor projection");
    query.scene_read = {UI::ActorProjection::Context, false};
    const auto context = UI::execute_scene_operation(query);
    require(context.error_code.empty() && context.actor_context.actors.size() == 1 &&
            context.camera_list.cameras.empty(), "context queries must not load camera state");
    query.scene_read = {};
    require(UI::execute_scene_operation(query).error_code == "INVALID_ARGUMENT", "empty projection is invalid");

    UI::SceneRequest actor{UI::SceneMethod::UpdateActor};
    actor.archive_session = before.archive_session;
    actor.properties.scene = before.scene;
    actor.properties.actor = "transform-probe";
    actor.properties.name = "Renamed atomically";
    actor.properties.follow_camera = true;
    actor.properties.mechanics.emplace().mass = std::numeric_limits<float>::infinity();
    require(UI::execute_scene_operation(actor).error_code == "INVALID_ARGUMENT", "invalid properties reject a mixed name update");
    UI::SceneRequest read{UI::SceneMethod::ReadActorProperties};
    read.archive_session = before.archive_session;
    read.read_scene = before.scene;
    read.read_actor = actor.properties.actor;
    const auto unchanged = UI::execute_scene_operation(read).actor_read.properties;
    require(unchanged.actor == "Transform probe" && !unchanged.follow_camera &&
            UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == before.tree_revision,
            "a rejected mixed patch must preserve name, properties and tree revision");
    actor.properties.mechanics.reset();
    const auto updated = UI::execute_scene_operation(actor);
    require(updated.error_code.empty() && updated.properties.actor == "Renamed atomically" &&
            updated.properties.actor_guid == "transform-probe" && updated.properties.follow_camera &&
            updated.properties.actor_version == unchanged.actor_version + 1 &&
            UI::execute_scene_operation({UI::SceneMethod::ReadActive}).active.tree_revision == before.tree_revision + 1,
            "one combined name/property write must preserve GUID and advance revisions once");

    UI::SceneRequest camera{UI::SceneMethod::UpdateCamera};
    camera.archive_session = before.archive_session;
    camera.camera_properties.scene = before.scene;
    camera.camera_properties.camera_id = "primary";
    camera.camera_properties.name = "Renamed primary";
    camera.camera_properties.fov = 200;
    require(UI::execute_scene_operation(camera).error_code == "INVALID_ARGUMENT", "invalid camera pose rejects a mixed name update");
    query.scene_read = {UI::ActorProjection::None, true};
    const auto original_camera = UI::execute_scene_operation(query).camera_list.cameras.front().properties;
    require(original_camera.name == combined.camera_list.cameras.front().properties.name,
            "invalid camera patches must not rename the camera");
    camera.camera_properties.fov = 60;
    const auto renamed = UI::execute_scene_operation(camera);
    require(renamed.error_code.empty() && renamed.camera.name == "Renamed primary" && renamed.camera.fov == 60,
            "camera name and base properties share a single update");
    camera.camera_properties.name = combined.camera_list.cameras.back().properties.name;
    camera.camera_properties.fov = 80;
    require(UI::execute_scene_operation(camera).error_code == "DUPLICATE_CAMERA_NAME", "camera names remain unique");
    require(UI::execute_scene_operation(query).camera_list.cameras.front().properties.fov == 60,
            "a conflicting camera name must not partially apply the pose");
    UI::load_native_editor_scene_for_test(snapshot());
}

int main() {
    auto& kernel = Corona::Kernel::KernelContext::instance();
    int failures = 0;
    // Each group runs isolated: `require` throws on a group's first failing assertion, and the
    // group is recorded and skipped so the remaining groups still run. Without this the suite
    // stopped at the first failure and every later camera/viewport/pick assertion — the coverage
    // that has to gate the interface refactor — never executed at all.
    const auto run = [&failures](const char* name, const std::function<void()>& body) {
        try {
            body();
        } catch (const std::exception& error) {
            std::cerr << "  FAILED " << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    try {
        run("identity_lifetime", test_identity_lifetime);
        run("script_input_queue_owner", test_script_input_queue_has_one_owner_and_drops_retired_scopes);
        require(kernel.initialize(), "kernel initialization failed");
        UI::load_native_editor_scene_for_test(snapshot());
        run("optics_queue", test_optics_queue_merges_independent_fields);
        run("render_settings_queue", test_render_settings_queue_owns_values);
        run("resource_import", test_resource_import_results_and_modal_retirement);
        run("audio_adapters", test_audio_adapters_publish_once_with_exact_identity);
        run("actor_index_projection", test_owned_actor_index_projection);
        run("actor_property_projection", test_owned_actor_property_projection);
        run("transform_authority", test_one_native_transform_authority);
        run("viewport_configuration", test_viewport_configuration_shares_identity_and_applies_atomically);
        run("pointer_configuration", test_pointer_configuration_owns_one_state_and_retires_with_its_context);
        run("camera_release", test_camera_release_discards_queued_viewport_updates);
        run("deferred_viewports", test_deferred_viewports_use_the_original_camera_authority);
        run("cached_viewport_binding", test_cached_web_viewport_binding_retires_with_its_camera);
        run("pick_requests", test_owned_pick_requests_and_coordinate_mapping);
        run("consolidated_queries", test_consolidated_queries_and_atomic_names);
        run("script_service_wait_uncontended",
            [&] { test_native_script_service_wait_does_not_hold_editor_state(false); });
        run("script_service_wait_contended",
            [&] { test_native_script_service_wait_does_not_hold_editor_state(true); });
        if (failures != 0) {
            std::cerr << "CameraWindowIdentityTests: " << failures << " group(s) failed\n";
            std::_Exit(1);
        }
        kernel.shutdown();
        std::cout << "CameraWindowIdentityTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CameraWindowIdentityTests failed: " << error.what() << '\n';
        // `require` throws, so a failing setup step skips the kernel.shutdown() at the end of
        // this function. Tearing the kernel down from that half-mutated state crashes in static
        // destruction, which reports "SegFault" and hides the assertion that actually failed.
        std::_Exit(1);
    }
}
