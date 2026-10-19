#include <corona/kernel/core/kernel_context.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/network/network_system.h>

#include "cef/cef_editor_native_api_registry.h"
#include "collaborative_editor_runtime.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/scene_folder.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

namespace {

using Corona::Systems::NetworkSystem;
using Corona::Systems::UI::NativeContext;
using Corona::Systems::UI::NativeRequest;
using Corona::Systems::UI::NativeResult;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "CollaborativeEditorLoopbackTests failed: " << message << '\n';
    std::exit(1);
}

struct TempProject {
    std::filesystem::path root;

    TempProject() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("corona_collaborative_editor_loopback_" + std::to_string(nonce));
        std::filesystem::remove_all(root);
        require(Corona::Systems::UI::SceneFolders::create_scene_folder(root, "Loopback").has_value(),
                "portable scene fixture creation failed");
        std::ofstream model(root / "remote.obj", std::ios::binary | std::ios::trunc);
        model << "o triangle\n"
                 "v 0 0 0\n"
                 "v 1 0 0\n"
                 "v 0 1 0\n"
                 "f 1 2 3\n";
        require(static_cast<bool>(model), "model fixture creation failed");
    }

    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

nlohmann::json empty_archive_snapshot(const std::filesystem::path& root) {
    return {
        {"schema_version", 1},
        {"archive_type", "portable_scene"},
        {"project_root", root.string()},
        {"scene", {
            {"route", "scene.ini"},
            {"name", "Loopback"},
            {"core_version", "1"},
            {"scripts", nlohmann::json::object()},
            {"terrain", nlohmann::json::object()},
            {"environment", nlohmann::json::object()},
            {"vision", nlohmann::json::object()},
            {"actors", nlohmann::json::array()},
            {"cameras", nlohmann::json::array({{
                {"id", "camera-main"},
                {"name", "Main Camera"},
                {"position", {0.0, 0.0, -5.0}},
                {"forward", {0.0, 0.0, 1.0}},
                {"world_up", {0.0, 1.0, 0.0}},
                {"fov", 45.0},
                {"move_speed", 1.0},
                {"width", 640},
                {"height", 480},
                {"view_width", 640},
                {"view_height", 480},
            }})},
            {"active_camera_id", "camera-main"},
        }},
    };
}

NativeResult invoke(const std::string& module,
                    const std::string& function,
                    nlohmann::json args = nlohmann::json::array()) {
    const auto result = Corona::Systems::UI::NativeApiRegistry::instance().dispatch(
        NativeRequest{module, function, std::move(args)}, NativeContext{});
    require(result.has_value(), module + "." + function + " was not registered");
    require(result->success, module + "." + function + " failed: " + result->error);
    return *result;
}

void pump(NetworkSystem& host, NetworkSystem& client,
          std::chrono::milliseconds duration = std::chrono::milliseconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    do {
        host.update();
        client.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
}

template <typename Predicate>
void wait_until(NetworkSystem& host, NetworkSystem& client,
                Predicate&& predicate, const std::string& message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(host, client);
        if (predicate()) return;
    }
    require(false, message);
}

nlohmann::json scene_snapshot();
nlohmann::json actor_by_guid(const nlohmann::json& snapshot, const std::string& guid);

void tick_until_actor_created(NetworkSystem& host, NetworkSystem& client,
                              const std::string& guid) {
    wait_until(host, client, [&] {
        Corona::Systems::UI::tick_collaborative_editor_runtime();
        return actor_by_guid(scene_snapshot(), guid).is_object();
    }, "collaborative runtime did not apply remote Actor create");
}

nlohmann::json scene_snapshot() {
    return invoke("SceneTools", "get_scene_snapshot", nlohmann::json::array({"scene.ini"})).data;
}

nlohmann::json actor_by_guid(const nlohmann::json& snapshot, const std::string& guid) {
    for (const auto& actor : snapshot.value("actors", nlohmann::json::array())) {
        if (actor.value("actor_guid", std::string{}) == guid) return actor;
    }
    return nullptr;
}

bool approximately_equal(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.0001;
}

uint16_t start_host_on_available_port(NetworkSystem& host) {
    for (uint16_t port = 39120; port < 39220; ++port) {
        if (host.start_session("loopback-host", 1, port, NetworkSystem::SessionRole::Host)) {
            return port;
        }
    }
    require(false, "no loopback UDP port was available");
    return 0;
}

void check_restir_camera_preferences(const std::filesystem::path& root) {
    const auto dispatch_settings = [](const nlohmann::json& settings) {
        return Corona::Systems::UI::NativeApiRegistry::instance().dispatch(
            NativeRequest{"SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", settings}},
            NativeContext{});
    };
    const auto snapshot = scene_snapshot();
    const auto handle = snapshot.at("cameras").at(0).at("handle").get<std::uintptr_t>();
    auto& hub = Corona::SharedDataHub::instance();
    const auto commit_restir_requests = [&] {
        for (const auto& update : hub.drain_camera_state_updates()) {
            if (!Corona::has_camera_state_field(update.fields, Corona::CameraStateUpdateField::VisionRestirSettings)) continue;
            {
                auto camera = hub.camera_storage().acquire_write(update.camera_handle);
                camera->vision_restir_settings = update.vision_restir_settings;
            }
            hub.acknowledge_camera_vision_restir_settings(update.camera_handle, update.sequence);
        }
    };
    const auto accepted = dispatch_settings({{"direct", {{"temporal", {{"open", false}}}}}});
    require(accepted && accepted->success && accepted->data.value("pending", false),
            "ReSTIR native API must acknowledge nested settings before the render thread applies them");
    require(hub.requested_camera_vision_restir_settings(handle) ==
                std::optional<std::string>{R"({"direct":{"temporal":{"open":false}}})"},
            "ReSTIR pending preference must remain available after native acceptance");

    const auto defaults = nlohmann::json::parse(R"({
        "max_depth":15,"min_depth":5,"rr_threshold":1,
        "direct":{"open":true,"M_light":10,"M_bsdf":1,"max_age":30,"max_recursion":5,
            "diffuse_factor":0.3,"pairwise":true,
            "temporal":{"open":true,"history_limit":5,"theta":35,"depth":0.3,"radius":8},
            "spatial":{"open":true,"theta":35,"depth":0.3,"radius":30}},
        "indirect":{"open":true,"max_age":30,"diffuse_factor":0.3,
            "temporal":{"open":true,"history_limit":5,"theta":35,"depth":0.3,"radius":8},
            "spatial":{"open":true,"theta":35,"depth":0.3,"radius":12}}})");
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        require(static_cast<bool>(camera), "native fixture camera must exist in storage");
        camera->vision_restir_effective_settings = defaults.dump();
    }
    auto feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(!feedback.value("ready", true) && feedback["settings"] == defaults,
            "pending ReSTIR overrides must not be merged into old effective feedback");
    commit_restir_requests();
    require(!hub.requested_camera_vision_restir_settings(handle).has_value() &&
                !invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data.value("ready", true),
            "committing camera preferences must not mark unapplied renderer feedback ready");
    auto effective = defaults;
    effective["direct"]["temporal"]["open"] = false;
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->vision_restir_effective_settings = effective.dump();
        camera->vision_restir_applied_settings = R"( { "direct": { "temporal": { "open": false } } } )";
    }
    feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(feedback.value("ready", false) && feedback["settings"] == effective,
            "applied ReSTIR feedback must match overrides by JSON structure rather than formatting");

    const nlohmann::json previous = {{"direct", {{"M_light", 50}, {"M_bsdf", 20}}}};
    invoke("SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", previous});
    commit_restir_requests();
    effective = defaults;
    effective["direct"]["M_light"] = 50;
    effective["direct"]["M_bsdf"] = 20;
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->vision_restir_effective_settings = effective.dump();
        camera->vision_restir_applied_settings = previous.dump();
    }
    const nlohmann::json replacement = {{"direct", {{"M_light", 12}}}};
    invoke("SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", replacement});
    feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(!feedback.value("ready", true) && feedback["settings"] == effective,
            "replacement overrides must not falsely combine new M_light with old M_bsdf feedback");
    commit_restir_requests();
    require(!invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data.value("ready", true),
            "replacement overrides remain unready until the renderer applies them");
    effective = defaults;
    effective["direct"]["M_light"] = 12;
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->vision_restir_effective_settings = effective.dump();
        camera->vision_restir_applied_settings = replacement.dump();
    }
    feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(feedback.value("ready", false) && feedback["settings"]["direct"]["M_bsdf"] == 1,
            "applying replacement overrides must restore omitted sibling values to scene defaults");
    invoke("SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", nlohmann::json::object()});
    feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(!feedback.value("ready", true) && feedback["settings"] == effective,
            "empty reset overrides must not mark the previous effective feedback ready");
    commit_restir_requests();
    require(!invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data.value("ready", true),
            "committed reset overrides remain unready until the renderer restores scene defaults");
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->vision_restir_effective_settings = defaults.dump();
        camera->vision_restir_applied_settings = "{}";
    }
    feedback = invoke("SceneTools", "get_restir_settings", {"scene.ini", "Main Camera"}).data;
    require(feedback.value("ready", false) && feedback["settings"] == defaults,
            "applied empty reset overrides must publish only scene default feedback");

    struct Range {
        const char* path;
        double minimum;
        double maximum;
        bool integer{false};
    };
    const Range ranges[] = {
        {"/max_depth", 0, 128, true}, {"/min_depth", 0, 20, true}, {"/rr_threshold", 0, 1},
        {"/direct/M_light", 0, 100, true}, {"/direct/M_bsdf", 0, 100, true},
        {"/direct/max_age", 0, 100, true}, {"/direct/max_recursion", 1, 100, true},
        {"/direct/diffuse_factor", 0, 1}, {"/indirect/max_age", 0, 100, true},
        {"/indirect/diffuse_factor", 0, 1},
        {"/direct/temporal/history_limit", 0, 50, true},
        {"/direct/temporal/theta", 0, 90}, {"/direct/temporal/depth", 0, 1},
        {"/direct/temporal/radius", 0, 50}, {"/direct/spatial/theta", 0, 90},
        {"/direct/spatial/depth", 0, 1}, {"/direct/spatial/radius", 0, 50},
        {"/indirect/temporal/history_limit", 0, 50, true},
        {"/indirect/temporal/theta", 0, 90}, {"/indirect/temporal/depth", 0, 1},
        {"/indirect/temporal/radius", 0, 50}, {"/indirect/spatial/theta", 0, 90},
        {"/indirect/spatial/depth", 0, 1}, {"/indirect/spatial/radius", 0, 50},
    };
    for (const auto& range : ranges) {
        for (const double endpoint : {range.minimum, range.maximum}) {
            nlohmann::json settings = nlohmann::json::object();
            settings[nlohmann::json::json_pointer(range.path)] = range.integer
                ? nlohmann::json(static_cast<int>(endpoint)) : nlohmann::json(endpoint);
            const auto result = dispatch_settings(settings);
            require(result && result->success, std::string("ReSTIR endpoint rejected: ") + range.path);
        }
        for (const auto& value : {nlohmann::json(range.minimum - 1), nlohmann::json(range.maximum + 1),
                                 nlohmann::json(true), nlohmann::json("1"), nlohmann::json(nullptr),
                                 nlohmann::json(std::numeric_limits<double>::infinity()),
                                 nlohmann::json(std::numeric_limits<double>::quiet_NaN())}) {
            nlohmann::json settings = nlohmann::json::object();
            settings[nlohmann::json::json_pointer(range.path)] = value;
            const auto result = dispatch_settings(settings);
            require(result && !result->success, std::string("invalid ReSTIR value accepted: ") + range.path);
        }
        if (range.integer) {
            nlohmann::json settings = nlohmann::json::object();
            settings[nlohmann::json::json_pointer(range.path)] = range.minimum + 0.5;
            const auto result = dispatch_settings(settings);
            require(result && !result->success, std::string("fractional ReSTIR integer accepted: ") + range.path);
        }
    }
    for (const auto* path : {"/direct/open", "/direct/pairwise", "/direct/temporal/open", "/direct/spatial/open",
                             "/indirect/open", "/indirect/temporal/open", "/indirect/spatial/open"}) {
        for (const auto& value : {nlohmann::json(true), nlohmann::json(false), nlohmann::json(1),
                                 nlohmann::json("false"), nlohmann::json(nullptr)}) {
            nlohmann::json settings = nlohmann::json::object();
            settings[nlohmann::json::json_pointer(path)] = value;
            const auto result = dispatch_settings(settings);
            require(result && result->success == value.is_boolean(),
                    std::string("ReSTIR boolean validation mismatch: ") + path);
        }
    }
    for (const auto& invalid : {
             nlohmann::json::array(), nlohmann::json(nullptr),
             nlohmann::json{{"direct", false}}, nlohmann::json{{"indirect", {{"temporal", 1}}}},
             nlohmann::json{{"unknown", 1}}, nlohmann::json{{"direct", {{"unknown", 1}}}},
             nlohmann::json{{"direct.M_light", 7}}, nlohmann::json{{"direct", {{"temporal.radius", 4}}}},
             nlohmann::json{{"indirect.temporal", {{"open", false}}}},
             nlohmann::json{{"indirect", {{"ratio", 1}}}}, nlohmann::json{{"stable_planes", false}}}) {
        const auto result = dispatch_settings(invalid);
        require(result && !result->success, "ReSTIR must reject unknown fields and malformed settings objects");
    }

    const nlohmann::json requested = {{"max_depth", 8}, {"direct", {{"M_light", 12},
        {"temporal", {{"open", false}}}}}, {"indirect", {{"spatial", {{"radius", 12.5}}}}}};
    invoke("SceneTools", "set_pt_settings", {"scene.ini", "Main Camera", {{"max_depth", 6}}});
    invoke("SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", requested});
    require(scene_snapshot()["cameras"][0]["vision_restir_settings"] == requested &&
                scene_snapshot()["cameras"][0]["vision_pt_settings"] == nlohmann::json{{"max_depth", 6}},
            "native snapshots must preserve independent pending PT and ReSTIR overrides");
    require(invoke("MainView", "scene_save", {"scene.ini"}).data.value("ok", false),
            "native scene save failed while ReSTIR settings were pending");
    std::ifstream saved(root / "scene.ini");
    std::string line;
    bool found = false;
    const std::string prefix = "camera0.vision_restir_settings = ";
    while (std::getline(saved, line)) {
        require(line.find("vision_restir_effective_settings") == std::string::npos &&
                    line.find("vision_restir_applied_settings") == std::string::npos,
                "effective ReSTIR feedback and applied markers must not be serialized");
        if (line.starts_with(prefix)) {
            require(nlohmann::json::parse(line.substr(prefix.size())) == requested,
                    "native scene save must serialize pending nested ReSTIR overrides");
            found = true;
        }
    }
    require(found, "native scene save omitted ReSTIR camera preferences");

    auto restored = empty_archive_snapshot(root);
    restored["scene"]["cameras"][0]["vision_restir_settings"] = requested;
    Corona::Systems::UI::load_native_editor_scene_for_test(restored);
    require(scene_snapshot()["cameras"][0]["vision_restir_settings"] == requested,
            "camera snapshot materialization must restore nested ReSTIR overrides");
    invoke("SceneTools", "set_restir_settings", {"scene.ini", "Main Camera", nlohmann::json::object()});
    require(scene_snapshot()["cameras"][0]["vision_restir_settings"].empty(),
            "empty ReSTIR overrides must queue a reset to scene defaults");
    Corona::Systems::UI::load_native_editor_scene_for_test(empty_archive_snapshot(root));
    require(scene_snapshot()["cameras"][0]["vision_restir_settings"].empty(),
            "legacy camera snapshots must default to empty ReSTIR overrides");
}

void run_loopback_test() {
    TempProject project;
    auto& kernel = Corona::Kernel::KernelContext::instance();
    require(kernel.initialize(), "KernelContext initialization failed");

    auto receiver = std::make_shared<NetworkSystem>();
    kernel.system_manager()->register_system(receiver);
    require(kernel.system_manager()->initialize_all(), "receiver NetworkSystem initialization failed");

    NetworkSystem host;
    require(host.initialize(nullptr), "host NetworkSystem initialization failed");
    host.set_project_root(project.root.string());
    receiver->set_project_root(project.root.string());

    Corona::Systems::UI::register_builtin_native_api_handlers();
    Corona::Systems::UI::load_native_editor_scene_for_test(empty_archive_snapshot(project.root));
    check_restir_camera_preferences(project.root);

    const auto port = start_host_on_available_port(host);
    require(receiver->start_session("loopback-client", 1, 0,
                                    NetworkSystem::SessionRole::Client),
            "client session failed to start");
    require(receiver->connect_to_peer("127.0.0.1", port, "loopback-host"),
            "client could not initiate loopback connection");
    wait_until(host, *receiver,
               [&] { return host.peer_count() == 1 && receiver->peer_count() == 1; },
               "loopback peers did not complete HELLO");

    constexpr const char* guid = "actor-loopback-1";
    float create_transform[9] = {1.0f, 2.0f, 3.0f, 0.1f, 0.2f, 0.3f, 1.0f, 1.0f, 1.0f};
    Corona::Network::ActorCreatePacked packed{};
    packed.visible = true;
    packed.bEnableLighting = true;
    packed.diffuse[0] = 0.2f;
    packed.diffuse[1] = 0.4f;
    packed.diffuse[2] = 0.6f;
    packed.roughness = 0.25f;
    packed.specular = 0.35f;
    packed.shininess = 20.0f;
    const nlohmann::json create_state = {
        {"actor_guid", guid},
        {"name", "Remote Cube"},
        {"actor_type", "model"},
        {"route", "remote.obj"},
        {"visible", true},
        {"follow_camera", false},
        {"geometry", {
            {"position", {1.0, 2.0, 3.0}},
            {"rotation", {0.1, 0.2, 0.3}},
            {"scale", {1.0, 1.0, 1.0}},
        }},
        {"optics", {
            {"diffuse", {0.2, 0.4, 0.6}},
            {"metallic", 0.15},
            {"roughness", 0.25},
            {"specular", 0.35},
            {"shininess", 20.0},
        }},
        {"mechanics", {
            {"mass", 2.0},
            {"restitution", 0.4},
            {"damping", 0.8},
            {"body_type", "dynamic"},
            {"linear_lock", {false, false, false}},
            {"angular_lock", {false, false, false}},
        }},
        {"camera_lock", {
            {"enabled", false},
            {"position_offset", {0.0, 0.0, 2.0}},
            {"rotation_offset", {0.0, 0.0, 0.0}},
        }},
    };
    host.broadcast_actor_create(guid, "scene.ini", "remote.obj", {}, create_transform,
                                &packed, sizeof(packed), create_state.dump());
    tick_until_actor_created(host, *receiver, guid);

    auto actor = actor_by_guid(scene_snapshot(), guid);
    require(actor.is_object(), "remote create did not add the Actor to the native scene");
    require(actor.value("name", std::string{}) == "Remote Cube", "create name mismatch");
    require(approximately_equal(actor["geometry"]["position"][1].get<double>(), 2.0),
            "create transform mismatch");
    require(approximately_equal(actor["optics"]["roughness"].get<double>(), 0.25),
            "create optics mismatch");
    require(approximately_equal(actor["mechanics"]["mass"].get<double>(), 2.0),
            "create mechanics mismatch");

    auto state = actor;
    state["name"] = "Remote Renamed";
    state["visible"] = false;
    state["optics"]["metallic"] = 0.75;
    state["mechanics"]["mass"] = 5.0;
    state["mechanics"]["linear_lock"] = {true, false, true};
    host.broadcast_actor_state_update(guid, "scene.ini", state.dump());
    wait_until(host, *receiver, [&] {
        Corona::Systems::UI::tick_collaborative_editor_runtime();
        const auto current = actor_by_guid(scene_snapshot(), guid);
        return current.value("name", std::string{}) == "Remote Renamed";
    }, "collaborative runtime did not apply remote Actor state");

    actor = actor_by_guid(scene_snapshot(), guid);
    require(actor.value("name", std::string{}) == "Remote Renamed", "remote rename did not apply");
    require(!actor.value("visible", true), "remote visibility did not apply");
    require(approximately_equal(actor["optics"]["metallic"].get<double>(), 0.75),
            "remote optics state did not apply");
    require(approximately_equal(actor["mechanics"]["mass"].get<double>(), 5.0),
            "remote mechanics state did not apply");
    require(actor["mechanics"]["linear_lock"] == nlohmann::json::array({true, false, true}),
            "remote mechanics lock did not apply");

    float moved[9] = {8.0f, 9.0f, 10.0f, 0.4f, 0.5f, 0.6f, 2.0f, 3.0f, 4.0f};
    host.broadcast_actor_transform_update(guid, "scene.ini", moved, "loopback-user", "move-1");
    wait_until(host, *receiver, [&] {
        Corona::Systems::UI::tick_collaborative_editor_runtime();
        const auto current = actor_by_guid(scene_snapshot(), guid);
        return current.value("geometry", nlohmann::json::object())
                   .value("position", nlohmann::json::array()) ==
               nlohmann::json::array({8.0, 9.0, 10.0});
    }, "collaborative runtime did not apply remote Actor transform");
    actor = actor_by_guid(scene_snapshot(), guid);
    require(actor["geometry"]["position"] == nlohmann::json::array({8.0, 9.0, 10.0}),
            "remote position did not apply");
    require(actor["geometry"]["scale"] == nlohmann::json::array({2.0, 3.0, 4.0}),
            "remote scale did not apply");

    host.broadcast_actor_delete(guid, "scene.ini", "Remote Renamed");
    wait_until(host, *receiver, [&] {
        Corona::Systems::UI::tick_collaborative_editor_runtime();
        return actor_by_guid(scene_snapshot(), guid).is_null();
    }, "collaborative runtime did not apply remote Actor delete");
    require(actor_by_guid(scene_snapshot(), guid).is_null(),
            "remote delete did not remove the Actor from the native scene");

    const std::string stale_guid = "loopback-stale-project-actor";
    host.broadcast_actor_create(stale_guid, "scene.ini", "remote.obj", {}, create_transform,
                                &packed, sizeof(packed), create_state.dump());
    wait_until(host, *receiver, [&] {
        return receiver->has_pending_transfers();
    }, "remote Actor create was not queued before project switch");
    const auto switched_root = project.root / "switched-project";
    std::filesystem::create_directories(switched_root);
    receiver->set_project_root(switched_root.string());
    require(!receiver->has_pending_transfers(),
            "project switch retained pending collaborative network work");
    Corona::Systems::UI::tick_collaborative_editor_runtime();
    require(actor_by_guid(scene_snapshot(), stale_guid).is_null(),
            "stale Actor create applied after project switch");

    host.stop_session();
    receiver->stop_session();
    host.shutdown();
    kernel.system_manager()->shutdown_all();
    Corona::Systems::UI::reset_native_editor_scene_for_test();
    kernel.shutdown();
}

}  // namespace

int main() {
    run_loopback_test();
    return 0;
}
