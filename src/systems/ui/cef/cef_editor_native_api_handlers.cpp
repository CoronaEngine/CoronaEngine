#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SDL3/SDL_events.h>
#include <corona/systems/optics/optics_commands.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shobjidl.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "browser_manager.h"
#include <corona/utils/runtime_paths.h>
#include "actor_selection_routing.h"
#include "cef_operations.h"
#include "archive_load_policy.h"
#include "../collaborative_editor_runtime.h"
#include "cef_client.h"
#include <corona/systems/ui/file_script_api.h>
#include "python_service_bridge.h"
#include "native_api_types.h"
#include "native_editor_state.h"
#include "cef_editor_native_api_test_support.h"
#include "cef_window_operations.h"
#include "cef_scene_operations.h"
#include "editor_api_services.h"
#include "cef_process.h"
#include "cef_viewport_pick.h"
#include "cef_editor_native_api_handlers.h"
#include "editor_dispatch.h"
#include <cef_task.h>
#include "scene_folder.h"
#include "ik_chain_config.h"
#include "animation_config.h"
#include "model_node_config.h"
#include "native_material_config.h"
#include "model_node_page.h"
#include "scene_tree_actor.h"
#include "ui_resource_read.h"
#include "../../common/vision_restir_settings.h"

#include <corona/events/acoustics_system_events.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/network/network_commands.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <corona/engine/engine_runtime_api.h>
#include <corona/utils/path_utils.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Corona::Systems::UI {

namespace {

using SceneFolders::SceneAssetStore;

std::shared_ptr<Corona::Systems::NetworkSystem> get_network_system() {
    auto sys_mgr = Corona::Kernel::KernelContext::instance().system_manager();
    if (!sys_mgr) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<Corona::Systems::NetworkSystem>(
        sys_mgr->get_system("Network"));
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looks_like_wlan_adapter(const std::string& adapter_name) {
    const std::string name = to_lower_ascii(adapter_name);
    return name.find("wlan") != std::string::npos ||
           name.find("wi-fi") != std::string::npos ||
           name.find("wifi") != std::string::npos ||
           name.find("wireless") != std::string::npos ||
           name.find("无线") != std::string::npos;
}

bool is_usable_ipv4(const std::string& ip) {
    return !ip.empty() && ip.rfind("127.", 0) != 0 && ip != "0.0.0.0";
}

std::string trim_ascii(std::string value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::filesystem::path path_from_utf8(const std::string& value) {
    return Corona::Utils::utf8_to_path(value);
}

std::string path_to_utf8(const std::filesystem::path& value) {
    auto text = Corona::Utils::path_to_utf8(value);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

std::string stem_utf8(const std::string& route) {
    return path_to_utf8(path_from_utf8(route).stem());
}

std::string normalize_route(std::string route) {
    route = trim_ascii(std::move(route));
    std::replace(route.begin(), route.end(), '\\', '/');
    return route;
}

std::filesystem::path resolve_project_path(const std::filesystem::path& project_root,
                                           const std::string& route) {
    const auto route_path = path_from_utf8(route);
    if (route_path.is_absolute()) {
        return route_path;
    }
    return project_root / route_path;
}

using IniSection = std::unordered_map<std::string, std::string>;
std::array<float, 3> parse_float3(const std::string& value,
                                  std::array<float, 3> fallback) {
    std::stringstream input(value);
    std::string item;
    std::array<float, 3> result = fallback;
    for (size_t index = 0; index < 3; ++index) {
        if (!std::getline(input, item, ',')) {
            return fallback;
        }
        try {
            result[index] = std::stof(trim_ascii(item));
        } catch (...) {
            return fallback;
        }
    }
    return result;
}

bool parse_bool(std::string value, bool fallback = false) {
    value = to_lower_ascii(trim_ascii(std::move(value)));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

bool legacy_svgf_mode(std::string mode) {
    mode = to_lower_ascii(trim_ascii(std::move(mode)));
    std::replace(mode.begin(), mode.end(), '-', '_');
    return mode == "svgf" || mode == "vision_svgf";
}

bool legacy_progressive_mode(std::string mode) {
    mode = to_lower_ascii(trim_ascii(std::move(mode)));
    std::replace(mode.begin(), mode.end(), '-', '_');
    return mode == "progressive_path_tracing";
}

bool json_bool_value(const nlohmann::json& object, const char* key, bool fallback);



const char* actor_load_status_name(ActorLoadStatus status) {
    switch (status) {
        case ActorLoadStatus::Loaded: return "loaded";
        case ActorLoadStatus::MissingResource: return "missing_resource";
        case ActorLoadStatus::DecodeFailed: return "decode_failed";
        case ActorLoadStatus::UnsupportedResource: return "unsupported_resource";
    }
    return "decode_failed";
}

std::vector<Corona::API::ModelNodeOverride> parse_native_model_node_overrides(const nlohmann::json& data) {
    std::vector<Corona::API::ModelNodeOverride> result;
    for (const auto& item : ModelNodeConfig::normalize(data)) {
        Corona::API::ModelNodeOverride node;
        node.node_index = item.at("node_index").get<std::uint32_t>();
        node.node_path = item.at("node_path").get<std::string>();
        if (item.contains("position")) node.position = item.at("position").get<std::array<float, 3>>();
        if (item.contains("rotation")) node.rotation = item.at("rotation").get<std::array<float, 3>>();
        if (item.contains("scale")) node.scale = item.at("scale").get<std::array<float, 3>>();
        if (item.contains("visible")) node.visible = item.at("visible").get<bool>();
        result.push_back(std::move(node));
    }
    return result;
}

void restore_native_model_nodes(NativeEditorActor& actor) {
    if (!actor.geometry || actor.persisted_model_node_overrides.empty()) return;
    // Resource replacement must not apply the old model's edits to unrelated nodes.
    if (actor.model_node_resource != actor.model_node_current_resource) return;
    if (!actor.geometry->set_model_node_overrides(
            parse_native_model_node_overrides(actor.persisted_model_node_overrides))) {
        CFW_LOG_WARNING("Model node overrides could not be restored for {}", actor.name);
    }
}

nlohmann::json native_actor_ik_configuration(const NativeEditorActor& actor) {
    if (!actor.geometry) return actor.persisted_ik_chains;
    std::vector<Corona::Resource::IkChain> chains;
    {
        auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_read(
            actor.geometry->get_handle());
        if (!geometry) return actor.persisted_ik_chains;
        chains = geometry->ik_chains;
    }
    if (chains.empty()) return nlohmann::json::array();
    const auto model_id = actor.geometry->get_model_id();
    if (model_id != 0) {
        auto resource = read_resource_for_ui<Corona::Resource::Scene>(model_id);
        if (resource.valid() && resource->data.skeleton) {
            return IkChainConfig::serialize(chains, &*resource->data.skeleton);
        }
    }
    return IkChainConfig::serialize(chains);
}

std::shared_ptr<Corona::Resource::AnimationPlayer> actor_animation_player(const NativeEditorActor& actor) {
    if (!actor.geometry) return {};
    auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_read(actor.geometry->get_handle());
    return geometry ? geometry->animation_player : nullptr;
}

std::string body_type_name(const Corona::API::Mechanics& mechanics) {
    return mechanics.get_state().body_type;
}

std::string normalize_body_type(std::string value) {
    if (value == "dynamic" || value == "kinematic" || value == "static" || value == "phantom") return value;
    // 向后兼容旧碰撞形状字段映射
    if (value == "none") return "phantom";
    if (value == "box" || value == "mesh") return "dynamic";
    CFW_LOG_WARNING("Invalid body type '{}'; using dynamic", value);
    return "dynamic";
}

bool is_valid_project_dir(const std::filesystem::path& project_dir);
std::filesystem::path canonical_project_dir_for_settings(const std::filesystem::path& project_dir);
std::string safe_project_dir_name(std::string name, const std::string& fallback);
void restore_native_camera_lock_target(NativeEditorScene& scene);
NativeResult project_archive_call(const char* method, nlohmann::json request);
std::filesystem::path editor_ini_path();
std::string settings_value(const std::string& section, const std::string& key, const std::string& fallback);

std::string read_last_project_from_editor_ini() {
    const auto raw = settings_value("General", "last_project", "");
    return raw.empty() ? std::string{} : normalize_route(path_to_utf8(canonical_project_dir_for_settings(path_from_utf8(raw))));
}

std::string resolve_active_project_path(const nlohmann::json& args) {
    auto project_path = normalize_route(arg_string(args, 0));
    if (!project_path.empty()) {
        const auto canonical = canonical_project_dir_for_settings(path_from_utf8(project_path));
        if (is_valid_project_dir(canonical)) {
            return path_to_utf8(canonical);
        }
    }
    auto& state = native_editor_state();
    if (!state.project_path.empty()) {
        const auto canonical = canonical_project_dir_for_settings(path_from_utf8(state.project_path));
        if (is_valid_project_dir(canonical)) {
            return path_to_utf8(canonical);
        }
    }
    return read_last_project_from_editor_ini();
}


std::string actor_file_extension(const std::string& route) {
    auto ext = path_from_utf8(route).extension().string();
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    return ext;
}

bool path_is_inside_project(const std::filesystem::path& relative_path) {
    if (relative_path.empty()) {
        return false;
    }
    for (const auto& part : relative_path) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string route_for_project_storage(const std::filesystem::path& project_root,
                                      const std::string& raw_path) {
    const auto normalized = normalize_route(raw_path);
    if (normalized.empty()) {
        return {};
    }
    const auto source_path = path_from_utf8(normalized);
    if (!source_path.is_absolute()) {
        return normalized;
    }

    std::error_code ec;
    const auto relative = std::filesystem::relative(source_path, project_root, ec);
    if (!ec && path_is_inside_project(relative)) {
        return normalize_route(path_to_utf8(relative));
    }
    return normalize_route(path_to_utf8(source_path));
}


std::string unique_actor_name(const NativeEditorScene& scene, const std::string& preferred_name) {
    const std::string base = trim_ascii(preferred_name).empty() ? "Actor" : trim_ascii(preferred_name);
    auto exists = [&](const std::string& candidate) {
        return std::any_of(scene.actors.begin(), scene.actors.end(), [&](const NativeEditorActor& actor) {
            return actor.name == candidate;
        });
    };
    if (!exists(base)) {
        return base;
    }
    for (int index = 1; index < 10000; ++index) {
        const auto candidate = base + "_" + std::to_string(index);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return base + "_" + std::to_string(scene.actors.size() + 1);
}

std::string make_actor_guid(const std::string& scene_route,
                            const std::string& actor_name,
                            size_t index) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream out;
    out << "native-" << std::hex << std::hash<std::string>{}(scene_route + ":" + actor_name)
        << "-" << index << "-" << nonce;
    return out.str();
}

void apply_native_scene_environment(NativeEditorScene& scene) {
    if (!scene.environment) {
        return;
    }

    scene.environment->set_properties({.sun_direction = scene.sun_direction,
                                       .sun_intensity = scene.sun_enabled ? 10.0f : 0.0f,
                                       .sky_intensity = scene.sun_enabled ? 20.0f : 0.0f,
                                       .floor_grid = scene.floor_grid_enabled});
    scene.environment->set_authored_lighting(scene.authored_lighting.is_null() ? "" : scene.authored_lighting.dump());
}



nlohmann::json read_actor_descriptor(const NativeEditorScene& scene, const std::filesystem::path& source) {
    auto data = SceneFolders::archive_asset_request({{"op", "descriptor"}, {"root", path_to_utf8(scene.project_root)},
                                                     {"source", path_to_utf8(source)}});
    if (!data.contains("model")) throw std::runtime_error("Invalid actor descriptor: " + data.dump());
    return data;
}

std::filesystem::path resolve_native_actor_asset_path(const NativeEditorScene& scene, NativeEditorActor& item) {
    const auto source = resolve_project_path(scene.project_root, item.route);
    if (item.actor_type != "actor" || to_lower_ascii(actor_file_extension(item.route)) != "actor") return source;
    return path_from_utf8(read_actor_descriptor(scene, source).at("model").get<std::string>());
}


void apply_native_actor_optics_state(NativeEditorActor& item) {
    if (!item.optics) {
        return;
    }
    if (item.persisted_optics.diffuse) {
        item.optics->set_properties({.diffuse = *item.persisted_optics.diffuse});
    }
    if (item.persisted_optics.metallic) {
        item.optics->set_properties({.metallic = *item.persisted_optics.metallic});
    }
    if (item.persisted_optics.roughness) {
        item.optics->set_properties({.roughness = *item.persisted_optics.roughness});
    }
    if (item.persisted_optics.specular) {
        item.optics->set_properties({.specular = *item.persisted_optics.specular});
    }
    if (item.persisted_optics.shininess) {
        item.optics->set_properties({.shininess = *item.persisted_optics.shininess});
    }
    auto overrides = std::make_shared<const Corona::Materials::Overrides>(
        NativeMaterialConfig::parse(NativeMaterialConfig::resolve(item.persisted_optics)));
    auto principled = NativeMaterialConfig::principled(item.persisted_optics.material);
    auto& hub = Corona::SharedDataHub::instance();
    if (item.engine_actor) {
        if (auto actor = hub.actor_storage().acquire_read(item.engine_actor->get_handle())) {
            for (const auto handle : actor->profile_handles) {
                auto profile = hub.profile_storage().acquire_read(handle);
                if (!profile || !profile->optics_handle) continue;
                // P5：改走命令通道。geometry 归属校验随命令下发，
                // 由 DAG 侧在应用时比对（原先是 UI 线程直接持写锁比���）。
                Corona::OpticsUpdateCommand command{};
                command.optics_handle = profile->optics_handle;
                command.geometry_handle = item.geometry->get_handle();
                command.fields = Corona::OpticsField::MaterialOverrides |
                                 Corona::OpticsField::Principled;
                command.material_overrides = overrides;
                command.principled = principled;
                Corona::OpticsCommandChannel::instance().commands().push(std::move(command));
            }
        }
    }

}

NativeEditorActor& add_native_actor_to_scene(NativeEditorScene& scene,
                                             NativeEditorActor item,
                                             const std::filesystem::path& asset_path) {
    if (!asset_path.empty()) item.resolved_asset_path = path_to_utf8(asset_path);
    // Include the resolved model, not the mutable .actor wrapper. Cache this
    // signature once per resource binding; node UI requests do no disk IO.
    std::error_code node_identity_error;
    const auto node_file_size = std::filesystem::file_size(asset_path, node_identity_error);
    if (!node_identity_error) {
        const auto modified = std::filesystem::last_write_time(asset_path, node_identity_error);
        if (!node_identity_error) {
            const auto relative = asset_path.lexically_normal().lexically_relative(scene.project_root.lexically_normal());
            item.model_node_current_resource = path_to_utf8(relative.empty() ? asset_path : relative) + ":" +
                std::to_string(node_file_size) + ":" + std::to_string(modified.time_since_epoch().count());
        }
    }
    if (item.actor_type == "ui_image") {
        auto image_geometry = Corona::API::Geometry::from_image(path_to_utf8(asset_path));
        item.geometry = std::make_unique<Corona::API::Geometry>(std::move(image_geometry));
    } else if (item.actor_type == "audio") {
        // 音频物体：无网格，纯 transform 几何（空路径）。
        item.geometry = std::make_unique<Corona::API::Geometry>(std::string{});
    } else {
        item.geometry = std::make_unique<Corona::API::Geometry>(path_to_utf8(asset_path), item.import_options);
    }
    item.geometry->set_transform(item.position, item.rotation, item.scale);
    if (item.authored_matrix) item.geometry->set_matrix(*item.authored_matrix);
    if (!item.persisted_animation_sources.empty()) {
        std::vector<std::filesystem::path> paths;
        for (const auto& source : item.persisted_animation_sources) {
            auto path = path_from_utf8(source);
            paths.push_back(path.is_absolute() ? path : scene.project_root / path);
        }
        actor_animation_player(item)->set_sources(item.persisted_animation_sources, std::move(paths));
    }
    restore_native_model_nodes(item);
    if (!item.persisted_ik_chains.empty()) {
        auto chains = IkChainConfig::parse(item.persisted_ik_chains);
        auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_write(
            item.geometry->get_handle());
        if (!geometry) throw std::runtime_error("Actor IK geometry storage is unavailable");
        geometry->ik_chains = std::move(chains);
        ++geometry->ik_chains_revision;
    }

    item.optics = std::make_unique<Corona::API::Optics>(*item.geometry);
    item.mechanics = std::make_unique<Corona::API::Mechanics>(*item.geometry);
    item.acoustics = std::make_unique<Corona::API::Acoustics>(*item.geometry);
    if (item.actor_type == "ui_image") {
        item.optics->set_properties({.lighting_enabled = false});
        item.mechanics->set_properties({.body_type = "phantom"});
        item.follow_camera = true;
    } else if (item.actor_type == "audio") {
        // 音频物体不渲染、不参与物理；绑定音频资源。
        item.optics->set_properties({.visible = false});
        item.mechanics->set_properties({.body_type = "phantom"});
        if (item.audio_resource_id != 0) {
            item.acoustics->set_properties({.audio_resource = item.audio_resource_id});
        }
    }

    item.engine_actor = std::make_unique<Corona::API::Actor>();
    Corona::API::Actor::Profile profile{};
    profile.geometry = item.geometry.get();
    profile.optics = item.optics.get();
    profile.mechanics = item.mechanics.get();
    profile.acoustics = item.acoustics.get();
    auto* profile_ref = item.engine_actor->add_profile(profile);
    item.engine_actor->set_active_profile(profile_ref);
    apply_native_actor_optics_state(item);
    item.engine_actor->set_actor_guid(item.actor_guid);
    item.engine_actor->set_follow_camera(item.follow_camera);
    scene.engine_scene->add_actor(item.engine_actor.get());
    scene.actors.push_back(std::move(item));
    return scene.actors.back();
}


EnvironmentSnapshot native_environment_snapshot(const NativeEditorScene& scene) {
    if (!scene.environment) {
        throw std::runtime_error("Native scene environment is unavailable");
    }
    const auto physics = scene.environment->get_state();
    return {scene.archive_session, scene.route, scene.sun_enabled, scene.floor_grid_enabled, scene.sun_direction,
        {physics.gravity, physics.floor_y, physics.floor_restitution, physics.fixed_dt}};
}

nlohmann::json native_scene_environment_payload(const NativeEditorScene& scene) {
    const auto value = native_environment_snapshot(scene);
    return {
        {"status", "success"},
        {"scene", value.scene},
        {"sun", {
            {"enabled", value.sun_enabled},
            {"direction", value.sun_direction},
        }},
        {"authored_lighting", scene.authored_lighting},
        {"grid", {{"enabled", value.grid_enabled}}},
        {"physics", {
            {"gravity", value.physics.gravity},
            {"floor_y", value.physics.floor_y},
            {"floor_restitution", value.physics.floor_restitution},
            {"fixed_dt", value.physics.fixed_dt},
        }},
    };
}

std::array<float, 3> snapshot_float3(const nlohmann::json& value,
                                     std::array<float, 3> fallback) {
    if (!value.is_array() || value.size() != 3) return fallback;
    std::array<float, 3> result = fallback;
    for (std::size_t index = 0; index < 3; ++index) {
        if (!value[index].is_number()) return fallback;
        const auto number = value[index].get<double>();
        if (!std::isfinite(number)) return fallback;
        result[index] = static_cast<float>(number);
    }
    return result;
}

void validate_snapshot_float3(const nlohmann::json& value, const char* field) {
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error(std::string{"ArchiveSnapshot "} + field +
                                 " must be a three-number array");
    }
    for (const auto& component : value) {
        if (!component.is_number() || !std::isfinite(component.get<double>())) {
            throw std::runtime_error(std::string{"ArchiveSnapshot "} + field +
                                     " contains a non-finite number");
        }
    }
}

void validate_snapshot_finite_number(const nlohmann::json& object,
                                     const char* key,
                                     const char* field) {
    if (!object.contains(key)) return;
    const auto& value = object.at(key);
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
        throw std::runtime_error(std::string{"ArchiveSnapshot "} + field +
                                 " must be a finite number");
    }
}

void validate_archive_snapshot(const nlohmann::json& snapshot) {
    if (!snapshot.is_object() || snapshot.value("schema_version", 0) != 1) {
        throw std::runtime_error("Unsupported or malformed ArchiveSnapshot schema_version");
    }
    if (!snapshot.contains("project_root") || !snapshot["project_root"].is_string() ||
        !snapshot.contains("scene") || !snapshot["scene"].is_object()) {
        throw std::runtime_error("ArchiveSnapshot requires project_root and scene");
    }
    if (!path_from_utf8(snapshot["project_root"].get<std::string>()).is_absolute()) {
        throw std::runtime_error("ArchiveSnapshot project_root must be absolute");
    }
    const auto& scene = snapshot["scene"];
    if (!scene.contains("route") || !scene["route"].is_string() ||
        !scene.contains("actors") || !scene["actors"].is_array() ||
        !scene.contains("cameras") || !scene["cameras"].is_array()) {
        throw std::runtime_error("ArchiveSnapshot scene contract is incomplete");
    }
    constexpr std::size_t kMaxSnapshotItems = 100000;
    if (scene["actors"].size() > kMaxSnapshotItems ||
        scene["cameras"].size() > kMaxSnapshotItems) {
        throw std::runtime_error("ArchiveSnapshot exceeds the item safety limit");
    }

    std::unordered_set<std::string> actor_guids;
    for (const auto& actor : scene["actors"]) {
        if (!actor.is_object() || !actor.contains("actor_guid") ||
            !actor["actor_guid"].is_string() || actor["actor_guid"].get<std::string>().empty()) {
            throw std::runtime_error("ArchiveSnapshot actor requires a non-empty actor_guid");
        }
        if (!actor_guids.insert(actor["actor_guid"].get<std::string>()).second) {
            throw std::runtime_error("Duplicate actor_guid in ArchiveSnapshot");
        }
        IkChainConfig::parse(actor.value("ik_chains", nlohmann::json::array()));
        AnimationConfig::parse_sources(actor.value("animation_sources", nlohmann::json::array()));
        ModelNodeConfig::normalize(actor.value("model_node_overrides", nlohmann::json::array()));
        const auto history = actor.value("model_node_unmatched", nlohmann::json::array());
        if (!history.is_array()) throw std::runtime_error("Model node unmatched history must be an array");
        for (const auto& group : history) {
            if (!group.is_object() || !group.contains("resource") || !group.at("resource").is_string())
                throw std::runtime_error("Model node unmatched history requires resource identity");
            ModelNodeConfig::normalize(group.value("overrides", nlohmann::json::array()));
        }
        if (!actor.contains("route") || !actor["route"].is_string() ||
            !actor.contains("asset_path") || !actor["asset_path"].is_string() ||
            !actor.contains("transform") || !actor["transform"].is_object()) {
            throw std::runtime_error("ArchiveSnapshot actor contract is incomplete");
        }
        const auto asset_path = actor["asset_path"].get<std::string>();
        if (!asset_path.empty() && !path_from_utf8(asset_path).is_absolute()) {
            throw std::runtime_error("ArchiveSnapshot actor asset_path must be absolute");
        }
        const auto& transform = actor["transform"];
        if (transform.contains("matrix")) {
            const auto& matrix = transform.at("matrix");
            if (!matrix.is_array() || matrix.size() != 16)
                throw std::runtime_error("Actor matrix requires 16 values");
            for (const auto& value : matrix)
                if (!value.is_number() || !std::isfinite(value.get<float>()))
                    throw std::runtime_error("Actor matrix requires finite float values");
            for (int i : {3, 7, 11, 15})
                if (std::abs(matrix[i].get<float>() - (i == 15 ? 1.0f : 0.0f)) > 1e-6f)
                    throw std::runtime_error("Actor matrix must be affine");
        }
        validate_snapshot_float3(transform.value("position", nlohmann::json()),
                                 "actor.transform.position");
        validate_snapshot_float3(transform.value("rotation", nlohmann::json()),
                                 "actor.transform.rotation");
        validate_snapshot_float3(transform.value("scale", nlohmann::json()),
                                 "actor.transform.scale");
        if (actor.contains("camera_lock")) {
            const auto& lock = actor.at("camera_lock");
            if (!lock.is_object() || (lock.contains("enabled") && !lock["enabled"].is_boolean()))
                throw std::runtime_error("ArchiveSnapshot actor camera_lock is invalid");
            for (const auto* field : {"position_offset", "rotation_offset"}) {
                if (lock.contains(field)) validate_snapshot_float3(lock[field], field);
            }
        }
        if (actor.contains("mechanics") && actor["mechanics"].is_object()) {
            const auto collision = actor["mechanics"].value("collision_type", std::string{"box"});
            if (collision != "box" && collision != "mesh" && collision != "none") {
                throw std::runtime_error("ArchiveSnapshot actor collision_type is invalid");
            }
        }
        if (actor.contains("optics")) {
            if (!actor["optics"].is_object()) {
                throw std::runtime_error("ArchiveSnapshot actor optics must be an object");
            }
            const auto& optics = actor["optics"];
            if (optics.contains("diffuse")) {
                validate_snapshot_float3(optics["diffuse"], "actor.optics.diffuse");
            }
            if (optics.contains("emission")) {
                validate_snapshot_float3(optics["emission"], "actor.optics.emission");
            }
            for (const auto* field : {"metallic", "roughness", "specular", "shininess"}) {
                validate_snapshot_finite_number(optics, field, field);
            }
        }
    }

    std::unordered_set<std::string> camera_ids;
    for (const auto& camera : scene["cameras"]) {
        if (!camera.is_object() || !camera.contains("id") || !camera["id"].is_string() ||
            camera["id"].get<std::string>().empty()) {
            throw std::runtime_error("ArchiveSnapshot camera requires a non-empty id");
        }
        if (!camera_ids.insert(camera["id"].get<std::string>()).second) {
            throw std::runtime_error("Duplicate camera id in ArchiveSnapshot");
        }
        validate_snapshot_float3(camera.value("position", nlohmann::json()), "camera.position");
        validate_snapshot_float3(camera.value("forward", nlohmann::json()), "camera.forward");
        validate_snapshot_float3(camera.value("world_up", nlohmann::json()), "camera.world_up");
        validate_snapshot_finite_number(camera, "fov", "camera.fov");
        validate_snapshot_finite_number(camera, "move_speed", "camera.move_speed");
        const auto x_sign = camera.value("projection_x_sign", 1.0f);
        const auto near_plane = camera.value("near_plane", 0.1f);
        const auto far_plane = camera.value("far_plane", 100.0f);
        if (!std::isfinite(x_sign) || std::abs(x_sign) != 1.0f ||
            !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
            near_plane <= 0.0f || far_plane <= near_plane) {
            throw std::runtime_error("ArchiveSnapshot camera projection is invalid");
        }
        for (const auto* dimension : {"width", "height", "view_width", "view_height"}) {
            if (!camera.contains(dimension) || !camera[dimension].is_number_integer() ||
                camera[dimension].get<std::int64_t>() <= 0 ||
                camera[dimension].get<std::int64_t>() > 32768) {
                throw std::runtime_error(std::string{"ArchiveSnapshot camera."} + dimension +
                                         " is outside the safety range");
            }
        }
    }

    if (scene.contains("active_camera_id") && !scene["active_camera_id"].is_null()) {
        if (!scene["active_camera_id"].is_string() ||
            (!scene["active_camera_id"].get<std::string>().empty() &&
             !camera_ids.contains(scene["active_camera_id"].get<std::string>()))) {
            throw std::runtime_error("ArchiveSnapshot active_camera_id is invalid");
        }
    }
    if (scene.contains("environment")) {
        if (!scene["environment"].is_object()) {
            throw std::runtime_error("ArchiveSnapshot environment must be an object");
        }
        if (scene["environment"].contains("sun_direction")) {
            validate_snapshot_float3(scene["environment"]["sun_direction"],
                                     "environment.sun_direction");
        }
        if (scene["environment"].contains("physics")) {
            const auto& physics = scene["environment"]["physics"];
            if (!physics.is_object()) throw std::runtime_error("ArchiveSnapshot physics must be an object");
            if (physics.contains("gravity")) validate_snapshot_float3(physics["gravity"], "physics.gravity");
            for (const auto* field : {"floor_y", "floor_restitution", "fixed_dt"})
                validate_snapshot_finite_number(physics, field, field);
            if ((physics.contains("fixed_dt") && physics["fixed_dt"].get<float>() <= 0.0f) ||
                (physics.contains("floor_restitution") &&
                 (physics["floor_restitution"].get<float>() < 0.0f || physics["floor_restitution"].get<float>() > 1.0f)))
                throw std::runtime_error("ArchiveSnapshot physics range is invalid");
        }
    }
}

NativeEditorActor native_actor_from_snapshot(const nlohmann::json& actor_data) {
    NativeEditorActor item;
    item.persisted_snapshot = actor_data;
    item.persisted_animation_sources = AnimationConfig::parse_sources(actor_data.value("animation_sources", nlohmann::json::array()));
    item.persisted_model_node_overrides = ModelNodeConfig::normalize(
        actor_data.value("model_node_overrides", nlohmann::json::array()));
    item.model_node_resource = actor_data.value("model_node_resource", std::string{});
    item.model_node_unmatched = actor_data.value("model_node_unmatched", nlohmann::json::array());
    item.persisted_ik_chains = IkChainConfig::serialize(
        IkChainConfig::parse(actor_data.value("ik_chains", nlohmann::json::array())));
    item.name = actor_data.value("name", std::string{"Actor"});
    item.actor_guid = actor_data.value("actor_guid", std::string{});
    item.route = normalize_route(actor_data.value("route", std::string{}));
    item.actor_type = actor_data.value("actor_type", std::string{"actor"});
    item.runtime_entity_id = actor_data.value("runtime_entity_id", std::string{});
    item.asset_id = actor_data.value("asset_id", std::string{});
    item.model_ref = actor_data.value("model_ref", std::string{});
    item.entity_type = actor_data.value("entity_type", std::string{});
    item.semantic_role = actor_data.value("semantic_role", std::string{});
    item.source_plan_id = actor_data.value("source_plan_id", std::string{});
    item.source_batch_id = actor_data.value("source_batch_id", std::string{});
    item.source_scene_version = std::max(actor_data.value("source_scene_version", 1), 1);
    item.actor_version = std::max(actor_data.value("actor_version", 1), 1);
    item.follow_camera = actor_data.value("follow_camera", false);
    const auto camera_lock = actor_data.value("camera_lock", nlohmann::json::object());
    if (camera_lock.is_object()) {
        item.camera_lock_enabled = camera_lock.value("enabled",
                                                       camera_lock.value("lock_to_camera", false));
        item.camera_lock_position_offset = snapshot_float3(
            camera_lock.value("position_offset", nlohmann::json::array()),
            {0.0f, 0.0f, 2.0f});
        item.camera_lock_rotation_offset = snapshot_float3(
            camera_lock.value("rotation_offset", nlohmann::json::array()),
            {0.0f, 0.0f, 0.0f});
    }
    item.resolved_asset_path = actor_data.value("asset_path", std::string{});
    const auto transform = actor_data.value("transform", nlohmann::json::object());
    if (transform.contains("matrix")) item.authored_matrix = transform.at("matrix").get<std::array<float, 16>>();
    const auto options = actor_data.value("import_options", nlohmann::json::object());
    item.import_options.scene_geometry = options.value("scene_geometry", false);
    item.import_options.flip_uvs = options.value("flip_uvs", false);
    item.import_options.flip_normals = options.value("flip_normals", false);
    item.import_options.face_normals = options.value("face_normals", false);
    item.position = snapshot_float3(transform.value("position", nlohmann::json::array()),
                                    {0.0f, 0.0f, 0.0f});
    item.rotation = snapshot_float3(transform.value("rotation", nlohmann::json::array()),
                                    {0.0f, 0.0f, 0.0f});
    item.scale = snapshot_float3(transform.value("scale", nlohmann::json::array()),
                                 {1.0f, 1.0f, 1.0f});
    item.persisted_visible = actor_data.value("visible", true);
    const auto mechanics = actor_data.value("mechanics", nlohmann::json::object());
    item.persisted_body_type = normalize_body_type(
        mechanics.value("body_type",
            // 向后兼容：优先旧 collision_type，再看 physics_enabled bool
            mechanics.contains("collision_type") ? mechanics.value("collision_type", std::string{"dynamic"})
            : (mechanics.value("physics_enabled", true) ? std::string{"dynamic"} : std::string{"static"})));
    const auto optics = actor_data.value("optics", nlohmann::json::object());
    if (optics.contains("diffuse")) {
        item.persisted_optics.diffuse = snapshot_float3(optics["diffuse"], {0.8f, 0.8f, 0.8f});
    }
    const auto optional_float = [&](const char* key, std::optional<float>& target) {
        if (optics.contains(key) && optics[key].is_number()) {
            const auto value = optics[key].get<double>();
            if (std::isfinite(value)) target = static_cast<float>(value);
        }
    };
    optional_float("metallic", item.persisted_optics.metallic);
    optional_float("roughness", item.persisted_optics.roughness);
    optional_float("specular", item.persisted_optics.specular);
    optional_float("shininess", item.persisted_optics.shininess);
    if (optics.contains("emission")) {
        item.persisted_optics.emission = snapshot_float3(optics["emission"], {0.0f, 0.0f, 0.0f});
    }
    item.persisted_optics.texture = optics.value("texture", std::string{});
    item.persisted_optics.material = actor_data.value("material", nlohmann::json::object());
    (void)NativeMaterialConfig::resolve(item.persisted_optics);
    const auto audio_id = actor_data.value("audio_resource_id", std::string{});
    if (!audio_id.empty()) {
        try { item.audio_resource_id = std::stoull(audio_id); } catch (...) { item.audio_resource_id = 0; }
    }
    return item;
}

void append_actor_materialization_diagnostic(nlohmann::json& diagnostics,
                                             const NativeEditorActor& actor,
                                             const std::string& code,
                                             const std::string& message) {
    const bool already_reported = std::any_of(
        diagnostics.begin(), diagnostics.end(), [&](const nlohmann::json& diagnostic) {
            return diagnostic.value("actor_guid", std::string{}) == actor.actor_guid &&
                   diagnostic.value("code", std::string{}) == code;
        });
    if (already_reported) return;
    diagnostics.push_back({
        {"severity", "error"},
        {"recoverable", true},
        {"stage", "actor_materialization"},
        {"code", code},
        {"message", message},
        {"actor_guid", actor.actor_guid},
        {"actor_name", actor.name},
        {"resource_path", actor.route},
        {"path", actor.resolved_asset_path},
    });
}

bool snapshot_actor_resource_supported(const NativeEditorActor& actor,
                                       const std::filesystem::path& asset_path);

void preflight_scene_snapshot(const nlohmann::json& snapshot,
                              nlohmann::json& diagnostics) {
    validate_archive_snapshot(snapshot);
    for (const auto& actor_data : snapshot.at("scene").at("actors")) {
        const auto actor = native_actor_from_snapshot(actor_data);
        if (actor.actor_type == "audio") continue;
        const auto asset_path = path_from_utf8(actor.resolved_asset_path);
        const bool missing = actor.resolved_asset_path.empty() ||
                             !std::filesystem::is_regular_file(asset_path);
        if (missing) {
            append_actor_materialization_diagnostic(
                diagnostics,
                actor,
                "RESOURCE_NOT_FOUND",
                "Actor resource does not exist: " + actor.route);
        } else if (!snapshot_actor_resource_supported(actor, asset_path)) {
            append_actor_materialization_diagnostic(
                diagnostics,
                actor,
                "UNSUPPORTED_RESOURCE_TYPE",
                "Actor resource type is unsupported: " + actor.route);
        }
    }
}

bool snapshot_actor_resource_supported(const NativeEditorActor& actor,
                                       const std::filesystem::path& asset_path) {
    if (actor.actor_type == "audio") return true;
    const auto extension = to_lower_ascii(asset_path.extension().string());
    if (actor.actor_type == "ui_image") {
        static const std::set<std::string> image_extensions{
            ".bmp", ".hdr", ".jpeg", ".jpg", ".png", ".tga"};
        return image_extensions.contains(extension);
    }
    static const std::set<std::string> model_extensions{
        ".actor", ".dae", ".fbx", ".glb", ".gltf", ".obj", ".ply", ".usd", ".usda", ".usdc"};
    return model_extensions.contains(extension);
}

void materialize_actor_snapshot(NativeEditorScene& scene,
                                const nlohmann::json& actor_data,
                                bool allow_degraded,
    nlohmann::json& diagnostics) {
    auto item = native_actor_from_snapshot(actor_data);
    const auto asset_path = path_from_utf8(item.resolved_asset_path);
    const bool missing = item.actor_type != "audio" &&
                         (item.resolved_asset_path.empty() ||
                          !std::filesystem::is_regular_file(asset_path));
    const bool unsupported = !missing && !snapshot_actor_resource_supported(item, asset_path);
    if (unsupported) {
        item.load_status = ActorLoadStatus::UnsupportedResource;
        item.load_error_code = "UNSUPPORTED_RESOURCE_TYPE";
        item.load_error_message = "Actor resource type is unsupported: " + item.route;
        append_actor_materialization_diagnostic(
            diagnostics, item, item.load_error_code, item.load_error_message);
    } else if (!missing) {
        try {
            auto& actor = add_native_actor_to_scene(
                scene, std::move(item), asset_path);
            if (actor.optics) actor.optics->set_properties({.visible = actor.persisted_visible});
            if (actor.mechanics && actor.actor_type != "ui_image") {
                actor.mechanics->set_properties({.body_type = actor.persisted_body_type});
            }
            return;
        } catch (const std::exception& error) {
            item = native_actor_from_snapshot(actor_data);
            item.load_status = ActorLoadStatus::DecodeFailed;
            item.load_error_code = "MODEL_DECODE_FAILED";
            item.load_error_message = error.what();
            append_actor_materialization_diagnostic(
                diagnostics, item, item.load_error_code, item.load_error_message);
        }
    } else {
        item.load_status = ActorLoadStatus::MissingResource;
        item.load_error_code = "RESOURCE_NOT_FOUND";
        item.load_error_message = "Actor resource does not exist: " + item.route;
        append_actor_materialization_diagnostic(
            diagnostics, item, item.load_error_code, item.load_error_message);
    }

    if (!allow_degraded) return;
    const auto original_type = item.actor_type;
    item.actor_type = "actor";
    auto& placeholder = add_native_actor_to_scene(scene, std::move(item), std::filesystem::path{});
    placeholder.actor_type = original_type;
    if (placeholder.optics) placeholder.optics->set_properties({.visible = false});
    if (placeholder.mechanics) {
        placeholder.mechanics->set_properties({.body_type = "phantom"});
    }
}

NativeEditorCamera materialize_camera_snapshot(NativeEditorScene& scene,
                                                const nlohmann::json& camera_data,
                                                std::size_t index) {
    NativeEditorCamera item;
    item.camera_id = camera_data.value("id", scene.route + "#camera" + std::to_string(index));
    item.name = camera_data.value("name", index == 0 ? scene.name + "_MainCamera"
                                                     : scene.name + "_Camera" + std::to_string(index));
    item.deletable = camera_data.value("deletable", index != 0);
    item.width = camera_data.value("width", 1920);
    item.height = camera_data.value("height", 1080);
    item.view_open = camera_data.value("view_open", false);
    item.view_x = camera_data.value("view_x", 120);
    item.view_y = camera_data.value("view_y", 120);
    item.view_width = camera_data.value("view_width", 960);
    item.view_height = camera_data.value("view_height", 540);
    item.move_speed = camera_data.value("move_speed", 1.0f);
    item.vision_spp = camera_data.value("vision_spp", std::string{});
    item.vision_max_depth = camera_data.value("vision_max_depth", std::string{});
    const auto position = snapshot_float3(camera_data.value("position", nlohmann::json::array()),
                                          {0.0f, 0.0f, -5.0f});
    const auto forward = snapshot_float3(camera_data.value("forward", nlohmann::json::array()),
                                         {0.0f, 0.0f, 1.0f});
    const auto world_up = snapshot_float3(camera_data.value("world_up", nlohmann::json::array()),
                                          {0.0f, 1.0f, 0.0f});
    item.engine_camera = std::make_unique<Corona::API::Camera>(
        position, forward, world_up, camera_data.value("fov", 45.0f));
    item.engine_camera->set_projection(camera_data.value("projection_x_sign", 1.0f),
        camera_data.value("near_plane", 0.1f), camera_data.value("far_plane", 100.0f));
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode(camera_data.value("output_mode", std::string{"final_color"}));
    item.engine_camera->set_render_backend(camera_data.value("render_backend", std::string{"native"}));
    const auto vision_mode = camera_data.value("vision_render_mode", std::string{"path_tracing"});
    item.engine_camera->set_vision_render_mode(vision_mode);
    item.engine_camera->set_vision_denoiser(camera_data.value("vision_denoiser", std::string{legacy_svgf_mode(vision_mode) ? "svgf" : "reblur"}));
    API::set_vision_settings(API::VisionSettingsFamily::Reblur, camera_data.value("vision_reblur_settings", nlohmann::json::object()), item.engine_camera->get_handle());
    API::set_vision_settings(API::VisionSettingsFamily::Pt, camera_data.value("vision_pt_settings", nlohmann::json::object()), item.engine_camera->get_handle());
    API::set_vision_settings(API::VisionSettingsFamily::Restir, camera_data.value("vision_restir_settings", nlohmann::json::object()), item.engine_camera->get_handle());
    API::set_vision_settings(API::VisionSettingsFamily::Relax, camera_data.value("vision_relax_settings", nlohmann::json::object()), item.engine_camera->get_handle());
    item.engine_camera->set_vision_denoise(
        json_bool_value(camera_data, "vision_denoise", true));
    item.engine_camera->set_vision_accumulation(
        json_bool_value(camera_data, "vision_accumulation", legacy_progressive_mode(vision_mode)));
    item.engine_camera->set_vision_stable_planes(
        json_bool_value(camera_data, "vision_stable_planes", true));
    item.engine_camera->set_view({item.view_open, item.view_x, item.view_y,
                                  item.view_width, item.view_height, item.move_speed});
    if (index > 0) item.engine_camera->set_offscreen_capture_mode(true);
    return item;
}

NativeEditorScene& materialize_scene_snapshot_into_state(
    NativeEditorState& state,
    const nlohmann::json& snapshot,
    bool allow_degraded,
    nlohmann::json& diagnostics) {
    validate_archive_snapshot(snapshot);
    const auto& data = snapshot.at("scene");
    state.scene.reset();
    state.project_path.clear();
    state.scene = std::make_unique<NativeEditorScene>();
    auto& scene = *state.scene;
    scene.archive_session = snapshot.value("archive_session", std::string{});
    scene.read_only = snapshot.value("read_only", false);
    scene.frame_when_ready = scene.read_only && data.at("cameras").empty();
    scene.project_root = path_from_utf8(snapshot.at("project_root").get<std::string>());
    scene.route = data.value("route", std::string{});
    scene.name = data.value("name", stem_utf8(scene.route));
    scene.core_version = data.value("core_version", std::string{});
    const auto scripts = data.value("scripts", nlohmann::json::object());
    const auto terrain = data.value("terrain", nlohmann::json::object());
    const auto environment = data.value("environment", nlohmann::json::object());
    scene.script_path = scripts.value("path", std::string{});
    scene.terrain_type = terrain.value("type", std::string{});
    scene.terrain_path = terrain.value("path", std::string{});
    scene.sun_enabled = environment.value("sun_enabled", true);
    scene.authored_lighting = environment.value("authored_lighting",nlohmann::json{});
    scene.sun_direction = snapshot_float3(
        environment.value("sun_direction", nlohmann::json::array()), {1.0f, 1.0f, 1.0f});
    scene.floor_grid_enabled = environment.value("floor_grid_enabled", true);
    try {
        scene.engine_scene = std::make_unique<Corona::API::Scene>();
        scene.environment = std::make_unique<Corona::API::Environment>();
        apply_native_scene_environment(scene);
        const auto physics = environment.value("physics", nlohmann::json::object());
        Corona::API::EnvironmentProperties stored_physics;
        if (physics.contains("gravity")) stored_physics.gravity = physics["gravity"].get<std::array<float, 3>>();
        if (physics.contains("floor_y")) stored_physics.floor_y = physics["floor_y"].get<float>();
        if (physics.contains("floor_restitution")) stored_physics.floor_restitution = physics["floor_restitution"].get<float>();
        if (physics.contains("fixed_dt")) stored_physics.fixed_dt = physics["fixed_dt"].get<float>();
        scene.environment->set_properties(stored_physics);
        scene.engine_scene->set_environment(scene.environment.get());
        scene.engine_scene->set_simulation_enabled(false);

        std::size_t camera_index = 0;
        for (const auto& camera_data : data.at("cameras")) {
            scene.cameras.push_back(materialize_camera_snapshot(scene, camera_data, camera_index++));
            scene.engine_scene->add_camera(scene.cameras.back().engine_camera.get());
        }
        if (scene.cameras.empty()) {
            scene.cameras.push_back(
                materialize_camera_snapshot(scene, nlohmann::json::object(), 0));
            scene.engine_scene->add_camera(scene.cameras.back().engine_camera.get());
        }
        const auto active_id = data.value("active_camera_id", scene.cameras.front().camera_id);
        scene.active_camera_index = 0;
        for (std::size_t index = 0; index < scene.cameras.size(); ++index) {
            if (scene.cameras[index].camera_id == active_id) scene.active_camera_index = index;
        }
        scene.engine_scene->set_active_camera(
            scene.cameras[scene.active_camera_index].engine_camera.get());

        scene.engine_scene->set_enabled(true);

        for (const auto& actor_data : data.at("actors")) {
            materialize_actor_snapshot(scene, actor_data, allow_degraded, diagnostics);
        }
        scene.engine_scene->set_simulation_enabled(data.value("simulation_enabled", true));

        restore_native_camera_lock_target(scene);
        scene.load_diagnostics = diagnostics;
        state.project_path = scene.read_only ? snapshot.at("source_path").get<std::string>()
                                            : path_to_utf8(scene.project_root);
        return scene;
    } catch (...) {
        state.scene.reset();
        state.project_path.clear();
        throw;
    }
}

NativeResult invoke_project_archive_parser(const std::string& path,
                                           const std::string& load_policy) {
    NativeRequest parser_request;
    parser_request.module = "ProjectArchive";
    parser_request.function = "parse";
    parser_request.args = nlohmann::json::array({{
        {"path", path},
        {"load_policy", load_policy},
    }});
    return invoke_python_script_service(parser_request, "python-archive", std::chrono::seconds(300));
}

struct PreparedArchiveLoad {
    bool service_ok{false};
    bool ready{false};
    std::string status{"invalid_archive"};
    std::string error;
    nlohmann::json snapshot = nlohmann::json::object();
    nlohmann::json diagnostics = nlohmann::json::array();
};

PreparedArchiveLoad prepare_archive_load(const std::string& path,
                                         const std::string& load_policy) {
    PreparedArchiveLoad prepared;
    auto parser_result = invoke_project_archive_parser(path, load_policy);
    if (!parser_result.success) {
        prepared.error = parser_result.error;
        return prepared;
    }
    prepared.service_ok = true;
    const auto& data = parser_result.data;
    prepared.status = data.value("status", std::string{"invalid_archive"});
    for (const auto& diagnostic : data.value("diagnostics", nlohmann::json::array()))
        prepared.diagnostics.push_back(diagnostic);
    if (!data.value("ok", false) || prepared.status == "invalid_archive") {
        return prepared;
    }
    if (!data.contains("snapshot") || !data["snapshot"].is_object()) {
        prepared.status = "invalid_archive";
        prepared.diagnostics.push_back({
            {"severity", "error"}, {"recoverable", false},
            {"stage", "snapshot_validation"}, {"code", "SNAPSHOT_CONTRACT_INVALID"},
            {"message", "Python archive parser returned no ArchiveSnapshot"},
            {"path", path},
        });
        return prepared;
    }
    prepared.snapshot = data["snapshot"];
    if (prepared.status == "decision_required" && load_policy != "degraded") {
        return prepared;
    }
    try {
        preflight_scene_snapshot(prepared.snapshot, prepared.diagnostics);
    } catch (const std::exception& error) {
        prepared.status = "invalid_archive";
        prepared.diagnostics.push_back({
            {"severity", "error"}, {"recoverable", false},
            {"stage", "snapshot_validation"}, {"code", "SNAPSHOT_CONTRACT_INVALID"},
            {"message", error.what()}, {"path", path},
        });
        return prepared;
    }
    const bool requires_confirmation = archive_diagnostics_require_confirmation(prepared.diagnostics);
    if (requires_confirmation && load_policy != "degraded") {
        prepared.status = "decision_required";
    } else {
        prepared.status = requires_confirmation ? "opened_degraded" : "opened";
        prepared.ready = true;
    }
    return prepared;
}

NativeEditorScene* ensure_native_editor_scene(const std::string& project_path_arg = {}) {
    auto& state = native_editor_state();
    (void)project_path_arg;
    if (!state.scene) {
        throw std::runtime_error("No committed native editor scene; open a project first");
    }
    return state.scene.get();
}

std::filesystem::path absolute_normalized_path(const std::filesystem::path& path);
bool is_path_within(const std::filesystem::path& root, const std::filesystem::path& candidate);

std::optional<std::string> stop_before_scene_replacement(const NativeEditorState& state) {
    if (!state.scene) return std::nullopt;
    const auto result = invoke_python_script_service({"FileScripts", "stop_script_execution", {true}});
    if (!result.success) return result.error;
    if (result.data.value("status", "") != "stopped" || result.data.value("snapshotCaptured", false)) {
        return result.data.value("message", std::string{"Script restore incomplete"});
    }
    return std::nullopt;
}

NativeEditorScene* reload_native_editor_scene(const std::string& project_path_arg,
                                              const std::string& scene_route_arg) {
    auto& state = native_editor_state();
    const auto project_path = normalize_route(project_path_arg.empty()
                                                  ? resolve_active_project_path(nlohmann::json::array())
                                                  : project_path_arg);
    if (project_path.empty()) {
        throw std::runtime_error("No active project path for native editor reload");
    }

    auto project_root = path_from_utf8(project_path);
    const bool external = state.scene && state.scene->read_only &&
        (absolute_normalized_path(project_root) == absolute_normalized_path(state.scene->project_root) ||
         absolute_normalized_path(project_root) == absolute_normalized_path(path_from_utf8(state.project_path)));
    const auto archive_path = external ? path_from_utf8(state.project_path) : project_root / "scene.ini";
    project_root = archive_path.parent_path();
    const auto requested_route = normalize_route(scene_route_arg);
    if (!requested_route.empty() && requested_route != path_to_utf8(archive_path.filename()) &&
        absolute_normalized_path(resolve_project_path(project_root, requested_route)) !=
            absolute_normalized_path(archive_path)) {
        throw std::runtime_error("Only the current scene source can be reloaded");
    }

    auto prepared = prepare_archive_load(path_to_utf8(archive_path), "prompt");
    if (!prepared.service_ok) {
        throw std::runtime_error(prepared.error.empty() ? "Archive reload failed" : prepared.error);
    }
    if (!prepared.ready) {
        throw std::runtime_error("Archive reload requires user decision");
    }
    if (auto error = stop_before_scene_replacement(state)) throw std::runtime_error(*error);
    return &materialize_scene_snapshot_into_state(
        state, prepared.snapshot, false, prepared.diagnostics);
}

bool native_scene_request_matches(const NativeEditorScene& scene,
                                  const std::string& scene_route_arg) {
    const auto requested = normalize_route(scene_route_arg);
    if (requested.empty()) {
        return true;
    }

    // Relative requests identify only the current scene. Absolute requests must
    // include the current project root; matching by filename crosses projects.
    const auto requested_path = path_from_utf8(requested);
    if (requested_path.is_absolute() || requested_path.has_parent_path()) {
        return absolute_normalized_path(resolve_project_path(scene.project_root, requested)) ==
               absolute_normalized_path(scene.project_root / path_from_utf8(scene.route));
    }
    return requested == scene.route || requested == scene.name;

}

NativeEditorScene* resolve_native_editor_scene_request(NativeEditorScene* scene,
                                                       const std::string& scene_route_arg) {
    if (scene == nullptr || native_scene_request_matches(*scene, scene_route_arg)) {
        return scene;
    }

    throw std::runtime_error("Request does not identify the current scene: " + scene_route_arg);
}

NativeEditorScene* scene_for_request_route(const NativeRequest& request, std::size_t scene_arg_index = 0) {
    auto* scene = ensure_native_editor_scene();
    const auto scene_route = normalize_route(arg_string(request.args, scene_arg_index));
    return resolve_native_editor_scene_request(scene, scene_route);
}


nlohmann::json project_resource_load_status() {
    const auto& state = native_editor_state();
    if (!state.scene) {
        return {
            {"active", false},
            {"loading", false},
            {"archive_service_ready", python_script_service_dispatcher_registered()},
            {"total", 0},
            {"ready", 0},
            {"failed", 0},
            {"pending", 0},
            {"progress", 0.0},
        };
    }

    std::size_t ready = 0;
    std::size_t failed = 0;
    std::size_t pending = 0;
    for (const auto& actor : state.scene->actors) {
        if (actor.load_status != ActorLoadStatus::Loaded) {
            ++failed;
            continue;
        }
        if (actor.actor_type == "audio") {
            ++ready;
            continue;
        }
        if (!actor.geometry) {
            ++failed;
            continue;
        }
        const auto gpu_state = actor.geometry->get_gpu_build_state();
        if (gpu_state == "Ready") {
            ++ready;
        } else if (gpu_state == "Failed" || gpu_state == "Invalid") {
            ++failed;
        } else {
            ++pending;
        }
    }
    if (state.scene->frame_when_ready && pending == 0) {
        if (ready == 0) state.scene->frame_when_ready = false;
        if (ready > 0 && !state.scene->cameras.empty()) {
            const auto bounds = state.scene->engine_scene->get_aabb();
            std::array<float, 3> center{};
            float radius2 = 0;
            for (int i = 0; i < 3; ++i) {
                center[i] = (bounds[i] + bounds[i+3]) * 0.5f;
                const auto extent = (bounds[i+3] - bounds[i]) * 0.5f;
                radius2 += extent * extent;
            }
            const auto radius = std::sqrt(radius2);
            if (std::isfinite(radius) && radius > 0) {
                state.scene->frame_when_ready = false;
                auto position = center;
                position[2] -= radius * 3.0f;
                auto& camera = state.scene->cameras[state.scene->active_camera_index];
                camera.engine_camera->set(position, {0,0,1}, {0,1,0}, 45.0f);
            }
        }
    }
    const auto total = ready + failed + pending;
    const double progress = total == 0
                                ? 100.0
                                : 100.0 * static_cast<double>(ready + failed) /
                                      static_cast<double>(total);
    return {
        {"active", true},
        {"loading", pending > 0},
        {"archive_service_ready", python_script_service_dispatcher_registered()},
        {"path", state.project_path},
        {"scene", state.scene->route},
        {"total", total},
        {"ready", ready},
        {"failed", failed},
        {"pending", pending},
        {"progress", progress},
    };
}

std::string current_viewport_session() {
    const auto* scene = native_editor_state().scene.get();
    return scene ? scene->archive_session : "";
}

bool requested_camera_view_open(const NativeEditorCamera& camera) {
    const auto state = camera.window_state->requested_open.load();
    return state < 0 ? camera.view_open : state != 0;
}

nlohmann::json camera_to_json(const NativeEditorCamera& camera) {
    nlohmann::json item;
    item["archive_session"] = current_viewport_session();
    item["id"] = camera.camera_id;
    item["camera_id"] = camera.camera_id;
    item["name"] = camera.name;
    item["handle"] = camera.engine_camera ? camera.engine_camera->get_handle() : 0;
    item["opaque_handle"] = std::to_string(camera.engine_camera ? camera.engine_camera->get_handle() : 0);
    item["position"] = camera.engine_camera ? camera.engine_camera->get_pose().position : std::array<float, 3>{0.0f, 0.0f, -5.0f};
    item["forward"] = camera.engine_camera ? camera.engine_camera->get_pose().forward : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    item["world_up"] = camera.engine_camera ? camera.engine_camera->get_pose().world_up : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    item["fov"] = camera.engine_camera ? camera.engine_camera->get_pose().fov : 45.0f;
    const auto projection = camera.engine_camera ? camera.engine_camera->get_projection()
                                                : std::array<float, 3>{1.0f, 0.1f, 100.0f};
    item["projection_x_sign"] = projection[0];
    item["near_plane"] = projection[1];
    item["far_plane"] = projection[2];
    item["width"] = camera.width;
    item["height"] = camera.height;
    item["output_mode"] = camera.engine_camera ? camera.engine_camera->get_output_mode() : "final_color";
    item["render_backend"] = camera.engine_camera ? camera.engine_camera->get_render_backend() : "native";
    item["vision_render_mode"] = camera.engine_camera ? camera.engine_camera->get_vision_render_mode() : "path_tracing";
    item["vision_spp"] = camera.vision_spp;
    item["vision_max_depth"] = camera.vision_max_depth;
    item["vision_denoise"] = camera.engine_camera ? camera.engine_camera->get_vision_denoise() : true;
    item["vision_denoiser"] = camera.engine_camera ? camera.engine_camera->get_vision_denoiser() : "reblur";
    item["vision_reblur_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Reblur, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_pt_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Pt, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_restir_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Restir, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_relax_settings"] = camera.engine_camera
        ? API::get_requested_vision_settings(API::VisionSettingsFamily::Relax, camera.engine_camera->get_handle())
        : nlohmann::json::object();
    item["vision_accumulation"] = camera.engine_camera ? camera.engine_camera->get_vision_accumulation() : false;
    item["vision_stable_planes"] = camera.engine_camera ? camera.engine_camera->get_vision_stable_planes() : true;
    item["move_speed"] = camera.move_speed;
    item["view_open"] = requested_camera_view_open(camera);
    item["view_x"] = camera.view_x;
    item["view_y"] = camera.view_y;
    item["view_width"] = camera.view_width;
    item["view_height"] = camera.view_height;
    item["deletable"] = camera.deletable;
    return item;
}

// The default is the rule, not a per-call decision: on the CEF UI thread - the thread
// that paints the editor and dispatches its input - engine reads are best effort, so a
// handler that forgets to think about it can never freeze the editor.  Other callers
// (the Python script runtime) keep the waiting semantics they rely on.
std::optional<std::array<float, 6>> native_actor_local_aabb(
    const NativeEditorActor& actor, bool best_effort = CefCurrentlyOn(TID_UI));
std::optional<std::array<float, 6>> native_actor_world_aabb(
    const NativeEditorActor& actor, bool best_effort = CefCurrentlyOn(TID_UI));
nlohmann::json aabb_to_json(const std::array<float, 6>& aabb);

nlohmann::json actor_to_json(const NativeEditorScene& scene, const NativeEditorActor& actor,
                            bool best_effort = CefCurrentlyOn(TID_UI)) {
    nlohmann::json item;
    item["name"] = actor.name;
    item["archive_session"] = scene.archive_session;
    item["actor_guid"] = actor.actor_guid;
    item["handle"] = actor.engine_actor ? actor.engine_actor->get_handle() : 0;
    item["path"] = actor.route;
    item["route"] = actor.route;
    item["scene"] = scene.route;
    item["type"] = actor_file_extension(actor.route);
    item["model"] = actor.route;
    item["model_dependencies"] = nlohmann::json::array();
    item["actor_type"] = actor.actor_type;
    if (actor.actor_type == "actor") item["resolved_model"] = actor.resolved_asset_path;
    item["ik_chains"] = native_actor_ik_configuration(actor);
    if (auto player = actor_animation_player(actor)) item.update(AnimationConfig::serialize(player->snapshot()));
    else {
        Corona::Resource::AnimationSnapshot animation;
        animation.sources = actor.persisted_animation_sources;
        animation.requested_sources = animation.sources;
        animation.load_status = "error";
        animation.load_error = "ACTOR_HAS_NO_GEOMETRY";
        item.update(AnimationConfig::serialize(animation));
    }
    item["model_node_overrides"] = actor.persisted_model_node_overrides;
    item["model_node_resource"] = actor.model_node_resource;
    item["model_node_unmatched"] = actor.model_node_unmatched;
    item["load_status"] = actor_load_status_name(actor.load_status);
    if (actor.load_status != ActorLoadStatus::Loaded) {
        item["load_error"] = {
            {"code", actor.load_error_code},
            {"message", actor.load_error_message},
            {"resource_path", actor.route},
            {"path", actor.resolved_asset_path},
        };
    } else {
        item["load_error"] = nullptr;
    }
    item["entity_id"] = actor.runtime_entity_id;
    item["asset_id"] = actor.asset_id;
    item["model_ref"] = actor.model_ref;
    item["entity_type"] = actor.entity_type;
    item["semantic_role"] = actor.semantic_role;
    item["source_plan_id"] = actor.source_plan_id;
    item["source_batch_id"] = actor.source_batch_id;
    item["source_scene_version"] = std::max(actor.source_scene_version, 1);
    item["actor_version"] = std::max(actor.actor_version, 1);
    item["version"] = std::max(actor.actor_version, 1);
    if (actor.actor_type == "audio") {
        item["audio_resource_id"] = std::to_string(actor.audio_resource_id);
    }
    item["body_type"] = actor.load_status == ActorLoadStatus::Loaded && actor.mechanics
                           ? body_type_name(*actor.mechanics)
                           : actor.persisted_body_type;
    item["visible"] = actor.load_status == ActorLoadStatus::Loaded && actor.optics
                          ? actor.optics->get_state().visible
                          : actor.persisted_visible;
    item["script"] = "";
    item["follow_camera"] = actor.follow_camera;
    item["render_space"] = actor.follow_camera ? "ui" : "scene";
    item["geometry"] = {
        {"position", actor.geometry ? actor.geometry->get_position() : actor.position},
        {"rotation", actor.geometry ? actor.geometry->get_rotation() : actor.rotation},
        {"scale", actor.geometry ? actor.geometry->get_scale() : actor.scale},
    };
    const auto local_aabb = native_actor_local_aabb(actor, best_effort);
    const auto world_aabb = native_actor_world_aabb(actor, best_effort);
    item["local_aabb"] = local_aabb ? aabb_to_json(*local_aabb) : nlohmann::json(nullptr);
    item["world_aabb"] = world_aabb ? aabb_to_json(*world_aabb) : nlohmann::json(nullptr);
    item["aabb"] = item["world_aabb"];
    item["bounds_ready"] = static_cast<bool>(world_aabb);
    const auto render_status = actor.geometry
        ? actor.geometry->get_render_status()
        : Corona::API::GeometryRenderStatus{};
    item["render_status_observed"] = render_status.observed;
    item["render_ready"] = render_status.ready;
    item["collision_ready"] = render_status.collision_ready;
    item["render_failed"] = render_status.failed;
    item["gpu_build_state"] = render_status.gpu_build_state;
    item["mesh_count"] = render_status.mesh_count;
    item["renderable_mesh_count"] = render_status.renderable_mesh_count;
    item["invalid_mesh_count"] = render_status.invalid_mesh_count;
    item["size"] = world_aabb
        ? nlohmann::json::array({
            (*world_aabb)[3] - (*world_aabb)[0],
            (*world_aabb)[4] - (*world_aabb)[1],
            (*world_aabb)[5] - (*world_aabb)[2],
        })
        : nlohmann::json::array({0.0f, 0.0f, 0.0f});
    if (actor.mechanics) {
        const auto mechanics_state = actor.mechanics->get_state();
        item["mechanics"] = {
            {"mass", mechanics_state.mass},
            {"restitution", mechanics_state.restitution},
            {"damping", mechanics_state.damping},
            {"body_type", actor.load_status == ActorLoadStatus::Loaded
                             ? mechanics_state.body_type
                             : actor.persisted_body_type},
            {"linear_lock", {mechanics_state.linear_lock[0], mechanics_state.linear_lock[1],
                             mechanics_state.linear_lock[2]}},
            {"angular_lock", {mechanics_state.angular_lock[0], mechanics_state.angular_lock[1],
                              mechanics_state.angular_lock[2]}},
        };
    }
    if (actor.optics) {
        const auto optics_state = actor.optics->get_state();
        item["optics"] = {
            {"diffuse", optics_state.diffuse},
            {"metallic", optics_state.metallic},
            {"roughness", optics_state.roughness},
            {"specular", optics_state.specular},
            {"shininess", optics_state.shininess},
        };
        if (actor.persisted_optics.emission) {
            item["optics"]["emission"] = *actor.persisted_optics.emission;
        }
    }
    item["material"] = {{"source", "model"}, {"overrides", NativeMaterialConfig::resolve(actor.persisted_optics)}};
    if (actor.persisted_optics.material.contains("principled"))
        item["material"]["principled"] = actor.persisted_optics.material["principled"];
    if (!actor.persisted_optics.texture.empty()) item["material"]["texture"] = actor.persisted_optics.texture;
    item["camera_lock"] = {
        {"enabled", actor.camera_lock_enabled},
        {"lock_to_camera", actor.camera_lock_enabled},
        {"position_offset", actor.camera_lock_position_offset},
        {"rotation_offset", actor.camera_lock_rotation_offset},
    };
    return item;
}

nlohmann::json scene_to_json(const NativeEditorScene& scene) {
    nlohmann::json cameras = nlohmann::json::array();
    for (const auto& camera : scene.cameras) {
        cameras.push_back(camera_to_json(camera));
    }
    nlohmann::json actors = nlohmann::json::array();
    for (const auto& actor : scene.actors) {
        actors.push_back(actor_to_json(scene, actor));
    }
    const auto active_index = scene.cameras.empty()
                                  ? 0
                                  : std::min(scene.active_camera_index, scene.cameras.size() - 1);
    const auto active_camera = scene.cameras.empty()
                                   ? nlohmann::json(nullptr)
                                   : camera_to_json(scene.cameras[active_index]);
    return {
        {"id", scene.route},
        {"scene_id", scene.route},
        {"name", scene.name},
        {"active_camera_id", active_camera.is_null() ? "" : active_camera.value("camera_id", "")},
        {"active_camera_name", active_camera.is_null() ? "" : active_camera.value("name", "")},
        {"camera", active_camera},
        {"cameras", cameras},
        {"sun", {{"enabled", scene.sun_enabled}, {"direction", scene.sun_direction}}},
        {"grid", {{"enabled", scene.floor_grid_enabled}}},
        {"terrain", {{"path", scene.terrain_path}, {"type", scene.terrain_type}}},

        {"script", scene.script_path},
        {"actors", actors},
    };
}

// Send current values only. ProjectArchive owns the on-disk schema and atomic commit.
NativeResult project_archive_call(const char* method, nlohmann::json request) {
    return invoke_python_script_service({"ProjectArchive", method, nlohmann::json::array({std::move(request)})},
                                        "python-archive", std::chrono::seconds(300));
}

bool matches_archive_session(const NativeEditorScene& scene, const nlohmann::json& request) {
    return !scene.archive_session.empty() && request.value("archive_session", std::string{}) == scene.archive_session;
}

nlohmann::json archive_scene_values(const NativeEditorScene& scene) {
    auto state = scene_to_json(scene);
    state["core_version"] = scene.core_version;
    state["physics"] = native_scene_environment_payload(scene).at("physics");
    for (size_t i = 0; i < scene.actors.size(); ++i) {
        const auto& actor = scene.actors[i];
        auto& item = state["actors"][i];
        item["animation_sources"] = actor.persisted_animation_sources;
        item["authored_optics"] = nlohmann::json::object();
        auto& optics = item["authored_optics"];
        const auto& values = actor.persisted_optics;
        if (values.diffuse) optics["diffuse"] = *values.diffuse;
        if (values.emission) optics["emission"] = *values.emission;
        if (values.metallic) optics["metallic"] = *values.metallic;
        if (values.roughness) optics["roughness"] = *values.roughness;
        if (values.specular) optics["specular"] = *values.specular;
        if (values.shininess) optics["shininess"] = *values.shininess;
    }
    for (size_t i = 0; i < scene.cameras.size(); ++i) {
        if (const auto& camera = scene.cameras[i].engine_camera) {
            auto& item = state["cameras"][i];
            item["vision_denoise"] = camera->get_requested_vision_denoise();
            item["vision_denoiser"] = camera->get_requested_vision_denoiser();
            item["vision_accumulation"] = camera->get_requested_vision_accumulation();
            item["vision_stable_planes"] = camera->get_requested_vision_stable_planes();
        }
    }
    return state;
}

NativeResult save_editor_scene(const NativeEditorScene& scene, bool actors_only,
                               const nlohmann::json& metadata = nlohmann::json::object()) {
    if (scene.read_only)
        return native_success({{"ok", false}, {"status", "read_only"}, {"message", "External geometry scenes cannot be saved"}});
    auto state = archive_scene_values(scene);
    return project_archive_call("save", {{"project_path", path_to_utf8(scene.project_root)}, {"actors_only", actors_only},
                                         {"scene", std::move(state)}, {"metadata", metadata}});
}

NativeEditorActor* find_native_actor(NativeEditorScene& scene, const std::string& actor_name) {
    for (auto& actor : scene.actors) {
        if (actor.name == actor_name || actor.actor_guid == actor_name ||
            (actor.engine_actor && std::to_string(actor.engine_actor->get_handle()) == actor_name))
        {
            return &actor;
        }
    }
    return nullptr;
}

NativeEditorCamera* find_native_camera(NativeEditorScene& scene, const std::string& camera_name) {
    if (camera_name.empty() && !scene.cameras.empty()) {
        return &scene.cameras[std::min(scene.active_camera_index, scene.cameras.size() - 1)];
    }
    for (auto& camera : scene.cameras) {
        if (camera.name == camera_name || camera.camera_id == camera_name ||
            (camera.engine_camera && std::to_string(camera.engine_camera->get_handle()) == camera_name))
        {
            return &camera;
        }
    }
    return nullptr;
}

std::string unique_camera_name(const NativeEditorScene& scene, const std::string& preferred_name) {
    const auto base = trim_ascii(preferred_name).empty() ? std::string("Camera") : trim_ascii(preferred_name);
    auto exists = [&](const std::string& candidate) {
        return std::any_of(scene.cameras.begin(), scene.cameras.end(), [&](const NativeEditorCamera& camera) {
            return camera.name == candidate || camera.camera_id == candidate;
        });
    };
    if (!exists(base)) {
        return base;
    }
    for (int index = 1; index < 10000; ++index) {
        const auto candidate = base + "_" + std::to_string(index);
        if (!exists(candidate)) {
            return candidate;
        }
    }
    return base + "_" + std::to_string(scene.cameras.size() + 1);
}

NativeEditorCamera& create_native_camera_view(NativeEditorScene& scene, const std::string& requested_name, bool initially_open = true) {
    NativeEditorCamera item;
    ++scene.tree_revision;
    item.name = unique_camera_name(scene, requested_name);
    item.camera_id = make_actor_guid(scene.route, item.name, scene.cameras.size());
    item.deletable = true;
    item.width = 1920;
    item.height = 1080;
    item.view_open = initially_open;
    item.view_x = 120;
    item.view_y = 120;
    item.view_width = 960;
    item.view_height = 540;
    item.move_speed = 1.0f;

    std::array<float, 3> position{0.0f, 0.0f, -5.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
    float fov = 45.0f;
    if (auto* source = find_native_camera(scene, {}); source && source->engine_camera) {
        position = source->engine_camera->get_pose().position;
        forward = source->engine_camera->get_pose().forward;
        world_up = source->engine_camera->get_pose().world_up;
        fov = source->engine_camera->get_pose().fov;
        item.width = source->width;
        item.height = source->height;
        item.move_speed = source->move_speed;
    }

    item.engine_camera = std::make_unique<Corona::API::Camera>(position, forward, world_up, fov);
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode("final_color");
    item.engine_camera->set_render_backend("native");
    item.engine_camera->set_vision_render_mode("path_tracing");
    item.engine_camera->set_view({item.view_open, item.view_x, item.view_y,
                                  item.view_width, item.view_height, item.move_speed});
    item.engine_camera->set_offscreen_capture_mode(true);
    item.engine_camera->set_surface(0);

    scene.cameras.push_back(std::move(item));
    auto& camera = scene.cameras.back();
    if (scene.engine_scene && camera.engine_camera) {
        scene.engine_scene->add_camera(camera.engine_camera.get());
    }
    return camera;
}

NativeResult camera_not_found_result(const std::string& camera_name) {
    return native_failure("Camera not found: " + camera_name, 2);
}

bool aabb_has_usable_extent(const std::array<float, 6>& aabb) {
    bool has_extent = false;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float min_v = aabb[axis];
        const float max_v = aabb[axis + 3];
        if (!std::isfinite(min_v) || !std::isfinite(max_v) || max_v < min_v) {
            return false;
        }
        if (std::abs(max_v - min_v) > 1e-5f) {
            has_extent = true;
        }
    }
    return has_extent;
}

std::optional<std::array<float, 6>> native_actor_local_aabb(const NativeEditorActor& actor,
                                                            bool best_effort) {
    if (!actor.geometry) {
        return std::nullopt;
    }
    // The editor UI thread must never wait on an engine lock: a loader holds a
    // resource entry lock across its whole parse, which used to freeze the editor
    // permanently.  Asking for the AABB best effort means "no bounds this refresh"
    // instead of "no editor".
    std::array<float, 6> result{};
    if (best_effort) {
        const auto value = actor.geometry->try_get_aabb();
        if (!value) {
            return std::nullopt;
        }
        result = *value;
    } else {
        const auto local = actor.geometry->get_aabb();
        if (local.size() < 6) {
            return std::nullopt;
        }
        for (size_t index = 0; index < 6; ++index) {
            result[index] = local[index];
        }
    }
    if (!aabb_has_usable_extent(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::array<float, 6>> native_actor_world_aabb(const NativeEditorActor& actor,
                                                           bool best_effort) {
    const auto local = native_actor_local_aabb(actor, best_effort);
    if (!local || !actor.geometry) {
        return std::nullopt;
    }
    auto& hub = Corona::SharedDataHub::instance();
    auto geometry = hub.geometry_storage().try_acquire_read(actor.geometry->get_handle());
    if (!geometry) return std::nullopt;
    auto transform = hub.model_transform_storage().try_acquire_read(geometry->transform_handle);
    if (!transform) return std::nullopt;
    const auto matrix = transform->compute_matrix();
    const float infinity = std::numeric_limits<float>::infinity();
    std::array<float, 6> result{infinity,infinity,infinity,-infinity,-infinity,-infinity};
    for (unsigned int corner = 0; corner < 8; ++corner) {
        const std::array<float, 4> point{(*local)[(corner&1) ? 3 : 0],
            (*local)[(corner&2) ? 4 : 1], (*local)[(corner&4) ? 5 : 2], 1};
        for (int axis = 0; axis < 3; ++axis) {
            float value = 0;
            for (int c = 0; c < 4; ++c) value += matrix[c][axis] * point[c];
            result[axis] = std::min(result[axis], value);
            result[axis+3] = std::max(result[axis+3], value);
        }
    }
    if (!aabb_has_usable_extent(result)) {
        return std::nullopt;
    }
    return result;
}

nlohmann::json aabb_to_json(const std::array<float, 6>& aabb) {
    return nlohmann::json::array({
        aabb[0], aabb[1], aabb[2], aabb[3], aabb[4], aabb[5],
    });
}

std::optional<std::array<float, 6>> native_scene_world_aabb(
    const NativeEditorScene& scene, bool best_effort = CefCurrentlyOn(TID_UI)) {
    std::optional<std::array<float, 6>> aggregate;
    for (const auto& actor : scene.actors) {
        const auto actor_aabb = native_actor_world_aabb(actor, best_effort);
        if (!actor_aabb) {
            continue;
        }
        if (!aggregate) {
            aggregate = *actor_aabb;
            continue;
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            (*aggregate)[axis] = std::min((*aggregate)[axis], (*actor_aabb)[axis]);
            (*aggregate)[axis + 3] = std::max((*aggregate)[axis + 3], (*actor_aabb)[axis + 3]);
        }
    }
    return aggregate;
}

std::string json_string_value(const nlohmann::json& object,
                              std::initializer_list<const char*> keys) {
    if (!object.is_object()) {
        return {};
    }
    for (const char* key : keys) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            const auto value = trim_ascii(it->get<std::string>());
            if (!value.empty()) {
                return value;
            }
        }
    }
    return {};
}

float json_float_at(const nlohmann::json& value, size_t index, float fallback = 0.0f) {
    if (!value.is_array() || index >= value.size() || !value[index].is_number()) {
        return fallback;
    }
    return value[index].get<float>();
}

std::optional<std::array<float, 3>> json_float3_value(const nlohmann::json& value) {
    if (!value.is_array() || value.size() < 3) {
        return std::nullopt;
    }
    return std::array<float, 3>{
        json_float_at(value, 0, 0.0f),
        json_float_at(value, 1, 0.0f),
        json_float_at(value, 2, 0.0f),
    };
}

std::optional<std::array<float, 3>> actor_data_float3(const nlohmann::json& actor_data,
                                                       const char* key) {
    if (!actor_data.is_object()) {
        return std::nullopt;
    }
    const auto top_it = actor_data.find(key);
    if (top_it != actor_data.end()) {
        if (auto value = json_float3_value(*top_it)) {
            return value;
        }
    }
    const auto geometry_it = actor_data.find("geometry");
    if (geometry_it != actor_data.end() && geometry_it->is_object()) {
        const auto nested_it = geometry_it->find(key);
        if (nested_it != geometry_it->end()) {
            return json_float3_value(*nested_it);
        }
    }
    return std::nullopt;
}

std::optional<float> actor_data_float(const nlohmann::json& actor_data,
                                      std::initializer_list<const char*> keys) {
    if (!actor_data.is_object()) {
        return std::nullopt;
    }
    for (const char* key : keys) {
        const auto it = actor_data.find(key);
        if (it != actor_data.end()) {
            if (it->is_number()) {
                return it->get<float>();
            }
            if (it->is_string()) {
                try {
                    return std::stof(it->get<std::string>());
                } catch (...) {
                    return std::nullopt;
                }
            }
        }
    }
    return std::nullopt;
}

int json_int_value(const nlohmann::json& object, const char* key, int fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}


bool json_bool_value(const nlohmann::json& object, const char* key, bool fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (it->is_boolean()) {
        return it->get<bool>();
    }
    if (it->is_number_integer()) {
        return it->get<int>() != 0;
    }
    if (it->is_string()) {
        return parse_bool(it->get<std::string>(), fallback);
    }
    return fallback;
}


bool json_bool_at(const nlohmann::json& value, size_t index, bool fallback = false) {
    if (!value.is_array() || index >= value.size()) {
        return fallback;
    }
    if (value[index].is_boolean()) {
        return value[index].get<bool>();
    }
    if (value[index].is_number_integer()) {
        return value[index].get<int>() != 0;
    }
    if (value[index].is_string()) {
        return parse_bool(value[index].get<std::string>(), fallback);
    }
    return fallback;
}

std::optional<bool> actor_data_bool(const nlohmann::json& actor_data,
                                    std::initializer_list<const char*> keys) {
    if (!actor_data.is_object()) {
        return std::nullopt;
    }
    for (const char* key : keys) {
        const auto it = actor_data.find(key);
        if (it != actor_data.end()) {
            if (it->is_boolean()) {
                return it->get<bool>();
            }
            if (it->is_number_integer()) {
                return it->get<int>() != 0;
            }
            if (it->is_string()) {
                return parse_bool(it->get<std::string>());
            }
        }
    }
    const auto mechanics_it = actor_data.find("mechanics");
    if (mechanics_it != actor_data.end() && mechanics_it->is_object()) {
        return actor_data_bool(*mechanics_it, keys);
    }
    return std::nullopt;
}




std::vector<std::string> prepare_animation_sources(const NativeEditorScene& scene, const std::vector<std::string>& value, bool memory_only = false) {
    auto sources = AnimationConfig::parse_sources(value);
    if (sources.empty() || scene.read_only || memory_only) return sources;
    SceneFolders::SceneAssetStore store(scene.project_root);
    for (auto& source : sources) {
        const auto path = resolve_project_path(scene.project_root, source);
        const auto imported = store.import_model(path); // also copies glTF buffers and other sidecars
        if (!imported.ok()) throw std::invalid_argument("Animation asset import failed: " + source);
        source = imported.main_route;
    }
    return AnimationConfig::parse_sources(sources); // content-addressed duplicates are invalid too
}

std::vector<std::string> prepare_animation_sources(const NativeEditorScene& scene, const nlohmann::json& value, bool memory_only = false) {
    return prepare_animation_sources(scene, AnimationConfig::parse_sources(value), memory_only);
}

void request_actor_animation_sources(const NativeEditorScene& scene, const NativeEditorActor& actor,
                                     std::vector<std::string> sources) {
    auto player = actor_animation_player(actor);
    if (!player) throw std::invalid_argument("Actor has no geometry");
    std::vector<std::filesystem::path> paths;
    for (const auto& source : sources) {
        auto path = path_from_utf8(source);
        paths.push_back(path.is_absolute() ? path : scene.project_root / path);
    }
    player->set_sources(std::move(sources), std::move(paths));
}

NativeResult set_native_actor_animation_paused(const NativeRequest& request, bool paused) {
    try {
        auto* scene = scene_for_request_route(request);
        auto* actor = find_native_actor(*scene, arg_string(request.args, 1));
        if (!actor) return native_failure("Actor not found", 2);
        auto player = actor_animation_player(*actor);
        if (!player) return native_failure("Actor has no geometry", 2);
        player->set_paused(paused);
        return native_success({{"status", "success"}, {"scene", scene->route},
                               {"actor", actor_to_json(*scene, *actor)}});
    } catch (const std::exception& error) { return native_failure(error.what(), 2); }
}

NativeResult create_native_editor_actor(const std::string& scene_route_arg,
                                        const std::string& source_path,
                                        std::string actor_type,
                                        const nlohmann::json& actor_data, bool memory_only = false) {
    const auto scene_route = normalize_route(scene_route_arg);
    actor_type = normalize_route(actor_type.empty() ? "model" : actor_type);
    if (actor_type.empty()) {
        actor_type = "model";
    }
    if (source_path.empty()) {
        return native_failure("create_actor source path is empty", 2);
    }

    auto* scene = ensure_native_editor_scene();
    scene = resolve_native_editor_scene_request(scene, scene_route);

    auto apply_runtime_metadata = [&](NativeEditorActor& target) {
        const auto entity_id = json_string_value(actor_data, {"entity_id", "runtime_entity_id"});
        const auto asset_id = json_string_value(actor_data, {"asset_id"});
        const auto model_ref = json_string_value(actor_data, {"model_ref"});
        const auto entity_type = json_string_value(actor_data, {"entity_type"});
        const auto semantic_role = json_string_value(actor_data, {"semantic_role"});
        const auto source_plan_id = json_string_value(actor_data, {"source_plan_id", "plan_id"});
        const auto source_batch_id = json_string_value(actor_data, {"source_batch_id", "batch_id"});
        if (!entity_id.empty()) target.runtime_entity_id = entity_id;
        if (!asset_id.empty()) target.asset_id = asset_id;
        if (!model_ref.empty()) target.model_ref = model_ref;
        if (!entity_type.empty()) target.entity_type = entity_type;
        if (!semantic_role.empty()) target.semantic_role = semantic_role;
        if (!source_plan_id.empty()) target.source_plan_id = source_plan_id;
        if (!source_batch_id.empty()) target.source_batch_id = source_batch_id;
        target.source_scene_version = std::max(
            json_int_value(actor_data, "source_scene_version",
                           json_int_value(actor_data, "scene_version", target.source_scene_version)),
            1);
        target.actor_version = std::max(
            json_int_value(actor_data, "actor_version", json_int_value(actor_data, "version", target.actor_version)),
            1);
    };

    auto apply_actor_data_to_existing = [&](NativeEditorActor& target) {
        if (actor_data.contains("animation_sources"))
            request_actor_animation_sources(*scene, target, prepare_animation_sources(*scene, actor_data.at("animation_sources"), memory_only));
        // The actor keeps the authoritative TRS cache; the geometry takes one complete write.
        if (auto position = actor_data_float3(actor_data, "position")) target.position = *position;
        if (auto rotation = actor_data_float3(actor_data, "rotation")) target.rotation = *rotation;
        if (auto scale = actor_data_float3(actor_data, "scale")) target.scale = *scale;
        if (target.geometry) target.geometry->set_transform(target.position, target.rotation, target.scale);
        if (auto follow_camera = actor_data_bool(actor_data, {"follow_camera"})) {
            target.follow_camera = *follow_camera;
        }
        if (auto loaded_bt = json_string_value(actor_data, {"body_type"}); !loaded_bt.empty()) {
            if (target.mechanics) target.mechanics->set_properties({.body_type = normalize_body_type(loaded_bt)});
        } else if (auto physics_enabled = actor_data_bool(actor_data, {"physics_enabled"})) {
            if (target.mechanics)
                target.mechanics->set_properties({.body_type = *physics_enabled ? "dynamic" : "static"});
        }
    };

    const auto preferred_name = json_string_value(
        actor_data, {"actor_name", "name", "alias", "model_name", "object_id", "target"});
    const auto preferred_guid = json_string_value(actor_data, {"actor_guid", "guid"});
    if (actor_data_bool(actor_data, {"skip_if_exists"}).value_or(false)) {
        NativeEditorActor* existing = nullptr;
        if (!preferred_guid.empty()) existing = find_native_actor(*scene, preferred_guid);
        if (existing == nullptr && !preferred_name.empty()) {
            existing = find_native_actor(*scene, preferred_name);
        }
        if (existing != nullptr) {
            if (actor_data_bool(actor_data, {"update_if_exists"}).value_or(false)) {
                apply_actor_data_to_existing(*existing);
                ++scene->tree_revision;
            }
            return native_success({{"status", "success"}, {"scene", scene->route},
                                   {"actor", actor_to_json(*scene, *existing)}, {"existed", true}});
        }
    }

    NativeEditorActor item;
    item.actor_type = actor_type;
    if (scene->read_only) throw std::runtime_error("External scenes are read-only");
    const auto source = resolve_project_path(scene->project_root, source_path);
    if (memory_only) {
        if (!is_path_within(scene->project_root, absolute_normalized_path(source)))
            return native_failure("File runs may only instantiate project assets", 400);
        item.route = route_for_project_storage(scene->project_root, source_path);
    } else {
        SceneAssetStore store(scene->project_root);
        SceneFolders::ImportResult imported;
        if (item.actor_type == "ui_image") imported = store.import_file(source, "Images");
        else if (item.actor_type == "audio") imported = store.import_file(source, "Audio");
        else if (item.actor_type == "actor" && to_lower_ascii(source.extension().string()) == ".actor") {
            const auto data = read_actor_descriptor(*scene, source);
            imported = store.import_actor(source, path_from_utf8(data.at("model").get<std::string>()));
        } else imported = store.import_model(source);
        if (!imported.ok()) {
            if (imported.diagnostics.empty()) {
                return native_failure("Portable asset import failed without diagnostics", 2);
            }
            const auto& diagnostic = imported.diagnostics.front();
            return native_failure("Portable asset import failed: " + diagnostic.message +
                                  " (" + path_to_utf8(diagnostic.path) + ")", 2);
        }
        item.route = imported.main_route;
    }
    item.name = unique_actor_name(*scene, preferred_name.empty() ? stem_utf8(item.route) : preferred_name);
    item.actor_guid = preferred_guid.empty()
        ? make_actor_guid(scene->route, item.name, scene->actors.size())
        : preferred_guid;
    item.follow_camera = item.actor_type == "ui_image";
    item.position = {0.0f, 0.0f, 0.0f};
    item.rotation = {0.0f, 0.0f, 0.0f};
    item.scale = {1.0f, 1.0f, 1.0f};
    apply_runtime_metadata(item);

    if (!memory_only && item.actor_type == "actor" && to_lower_ascii(actor_file_extension(item.route)) == "actor") {
        const auto actor_file = resolve_project_path(scene->project_root, item.route);
        const auto descriptor = read_actor_descriptor(*scene, actor_file);
        const auto geometry = descriptor.at("geometry");
        const auto configured_name = descriptor.value("name", "");
        if (preferred_name.empty() && !configured_name.empty()) {
            item.name = unique_actor_name(*scene, configured_name);
            item.actor_guid = preferred_guid.empty()
                ? make_actor_guid(scene->route, item.name, scene->actors.size())
                : preferred_guid;
        }
        item.follow_camera = parse_bool(descriptor.value("follow_camera", "false"));
        item.position = parse_float3(
            geometry.value("position", "0.0, 0.0, 0.0"),
            {0.0f, 0.0f, 0.0f});
        item.rotation = parse_float3(
            geometry.value("rotation", "0.0, 0.0, 0.0"),
            {0.0f, 0.0f, 0.0f});
        item.scale = parse_float3(
            geometry.value("scale", "1.0, 1.0, 1.0"),
            {1.0f, 1.0f, 1.0f});
    }

    if (auto position = actor_data_float3(actor_data, "position")) {
        item.position = *position;
    }
    if (auto rotation = actor_data_float3(actor_data, "rotation")) {
        item.rotation = *rotation;
    }
    if (auto scale = actor_data_float3(actor_data, "scale")) {
        item.scale = *scale;
    }
    if (auto follow_camera = actor_data_bool(actor_data, {"follow_camera"})) {
        item.follow_camera = *follow_camera;
    }

    // 音频物体：从 actor_data 解析绑定的音频资源 id（JS 以字符串传递，避免精度丢失）。
    {
        const auto rid_str = json_string_value(actor_data, {"audio_resource_id", "resource_id"});
        if (!rid_str.empty()) {
            try {
                item.audio_resource_id = std::stoull(rid_str);
            } catch (const std::exception&) {
                item.audio_resource_id = 0;
            }
        }
    }

    item.persisted_animation_sources = prepare_animation_sources(*scene,
        actor_data.value("animation_sources", nlohmann::json::array()), memory_only);
    std::filesystem::path asset_path;
    if (memory_only) {
        asset_path = source;
        if (to_lower_ascii(source.extension().string()) == ".actor") {
            const auto resolved = path_from_utf8(actor_data.value("resolved_model", std::string{}));
            if (resolved.empty() || !is_path_within(scene->project_root, absolute_normalized_path(resolved)))
                return native_failure("Missing project Actor model binding", 400);
            asset_path = resolved;
        }
    } else asset_path = resolve_native_actor_asset_path(*scene, item);
    if (item.actor_guid.empty()) {
        item.actor_guid = make_actor_guid(scene->route, item.name, scene->actors.size());
    }
    auto& actor = add_native_actor_to_scene(*scene, std::move(item), asset_path);
    if (auto ground_align = actor_data_bool(actor_data, {"ground_align"})) {
        if (*ground_align && actor.geometry) {
            const auto ground_y = actor_data_float(actor_data, {"ground_y"}).value_or(0.0f);
            const auto aabb = actor.geometry->get_aabb();
            auto position = actor.geometry->get_position();
            const auto scale = actor.geometry->get_scale();
            if (aabb.size() >= 6) {
                const auto min_y_world = position[1] + aabb[1] * scale[1];
                position[1] += ground_y - min_y_world;
                actor.position = position;
                actor.geometry->set_transform(actor.position, actor.rotation, actor.scale);
            }
        }
    }
    if (auto loaded_bt = json_string_value(actor_data, {"body_type"}); !loaded_bt.empty()) {
        if (actor.mechanics) actor.mechanics->set_properties({.body_type = normalize_body_type(loaded_bt)});
    } else if (auto physics_enabled = actor_data_bool(actor_data, {"physics_enabled"})) {
        if (actor.mechanics)
            actor.mechanics->set_properties({.body_type = *physics_enabled ? "dynamic" : "static"});
    } else if (actor.mechanics && actor.actor_type != "ui_image" && actor.actor_type != "audio") {
        // 新导入的物体默认为"幽灵"：不参与碰撞、不受力，避免刚导入就掉落或互相弹开。
        // 需要物理时在物体栏手动切换为动态/运动学/静态。
        // ui_image 与 audio 已在 add_native_actor_to_scene 内设为 phantom。
        actor.mechanics->set_properties({.body_type = "phantom"});
    }

    ++scene->tree_revision;
    return native_success({
        {"status", "success"},
        {"scene", scene->route},
        {"actor", actor_to_json(*scene, actor)},
    });
}

std::optional<std::array<float, 3>> transform_float3_value(const nlohmann::json& data,
                                                           const std::string& key,
                                                           const std::string& alias = {}) {
    if (!data.is_object()) {
        return std::nullopt;
    }
    if (data.contains(key)) {
        if (auto value = json_float3_value(data[key])) {
            return value;
        }
    }
    if (!alias.empty() && data.contains(alias)) {
        if (auto value = json_float3_value(data[alias])) {
            return value;
        }
    }
    const auto geo = data.find("geometry");
    if (geo != data.end() && geo->is_object()) {
        if (geo->contains(key)) {
            if (auto value = json_float3_value((*geo)[key])) {
                return value;
            }
        }
        if (!alias.empty() && geo->contains(alias)) {
            if (auto value = json_float3_value((*geo)[alias])) {
                return value;
            }
        }
    }
    return std::nullopt;
}

ActorDeleteSnapshot erase_native_editor_actor(
    NativeEditorScene& scene, std::vector<NativeEditorActor>::iterator actor) {
    ActorDeleteSnapshot snapshot;
    snapshot.archive_session = scene.archive_session;
    snapshot.scene = scene.route;
    snapshot.old_name = actor->name;
    snapshot.actor_guid = actor->actor_guid;
    snapshot.selection_cleared = scene.selection && scene.selection->actor_guid == actor->actor_guid;
    if (scene.engine_scene && actor->engine_actor)
        scene.engine_scene->remove_actor(actor->engine_actor.get());
    if (snapshot.selection_cleared) scene.selection.reset();
    scene.actors.erase(actor);
    snapshot.tree_revision = ++scene.tree_revision;
    return snapshot;
}

NativeResult remove_native_editor_actor(const std::string& scene_route_arg,
                                        const std::string& actor_name) {
    if (trim_ascii(actor_name).empty()) {
        return native_failure("Actor name cannot be empty", 2);
    }

    auto* scene = ensure_native_editor_scene();
    const auto scene_route = normalize_route(scene_route_arg);
    scene = resolve_native_editor_scene_request(scene, scene_route);

    auto it = std::find_if(scene->actors.begin(), scene->actors.end(), [&](const NativeEditorActor& actor) {
        if (actor.name == actor_name || actor.actor_guid == actor_name) {
            return true;
        }
        return actor.engine_actor && std::to_string(actor.engine_actor->get_handle()) == actor_name;
    });
    if (it == scene->actors.end()) {
        return native_failure("Actor not found: " + actor_name, 2);
    }

    const auto deleted = erase_native_editor_actor(*scene, it);

    return native_success({
        {"status", "success"},
        {"scene", deleted.scene},
        {"actor", deleted.old_name},
        {"actor_guid", deleted.actor_guid},
    });
}

SceneReply apply_native_environment(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const char* message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.environment;
    if (request.method == SceneMethod::SetEnvironment) {
        const auto invalid = SceneEnvironmentService::validate_patch(patch);
        if (!invalid.empty()) return fail("INVALID_ARGUMENT", invalid.c_str());
    }
    auto* scene = request.archive_session ? native_editor_state().scene.get() : ensure_native_editor_scene();
    if (request.archive_session) {
        if (!scene || request.archive_session->empty() || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, patch.scene))
            return fail("PROJECT_CONTEXT_CHANGED", "Environment does not identify the active scene");
    } else scene = resolve_native_editor_scene_request(scene, normalize_route(patch.scene));
    if (!scene->environment) return fail("COMPONENT_NOT_FOUND", "Native scene environment is unavailable");
    if (request.method == SceneMethod::SetEnvironment) {
        SceneEnvironmentService::EnvironmentScalars scalars{scene->sun_enabled, scene->sun_direction, scene->floor_grid_enabled};
        SceneEnvironmentService::apply_sun_and_grid(scalars, patch);
        scene->sun_enabled = scalars.sun_enabled;
        scene->sun_direction = scalars.sun_direction;
        scene->floor_grid_enabled = scalars.grid_enabled;
        const auto physics = SceneEnvironmentService::physics_update(patch);
        scene->environment->set_properties({.gravity = physics.gravity,
                                           .floor_y = physics.floor_y,
                                           .floor_restitution = physics.floor_restitution,
                                           .fixed_dt = physics.fixed_dt});
        apply_native_scene_environment(*scene);
    }
    reply.environment = native_environment_snapshot(*scene);
    return reply;
}

NativeResult native_environment_result(const SceneRequest& request) {
    const auto reply = apply_native_environment(request);
    if (!reply.error_code.empty()) return native_failure(reply.error_message,
        reply.error_code == "PROJECT_CONTEXT_CHANGED" ? 409 : 2);
    return native_success(native_scene_environment_payload(*native_editor_state().scene));
}

// JSON decoding/encoding belongs exclusively to the non-Web contracts.
// Sun, grid and physics adapters all enter the same typed environment operation.
NativeResult set_native_environment(const NativeRequest& request) {
    try {
        const auto state = arg_object(request.args, 1);
        SceneRequest write{SceneMethod::SetEnvironment};
        auto& patch = write.environment;
        patch.scene = arg_string(request.args, 0);
        if (state.contains("archive_session")) write.archive_session = state.at("archive_session").get<std::string>();
        if (const auto value = state.find("sun"); value != state.end() && value->is_object()) {
            EnvironmentSunPatch sun;
            sun.enabled = actor_data_bool(*value, {"enabled"});
            if (const auto direction = value->find("direction"); direction != value->end()) sun.direction = json_float3_value(*direction);
            patch.sun = sun;
        }
        if (const auto value = state.find("grid"); value != state.end() && value->is_object())
            patch.grid_enabled = actor_data_bool(*value, {"enabled"});
        if (const auto value = state.find("physics"); value != state.end() && value->is_object()) {
            EnvironmentPhysicsPatch physics;
            if (const auto gravity = value->find("gravity"); gravity != value->end()) physics.gravity = json_float3_value(*gravity);
            physics.floor_y = actor_data_float(*value, {"floor_y"});
            physics.floor_restitution = actor_data_float(*value, {"floor_restitution"});
            physics.fixed_dt = actor_data_float(*value, {"fixed_dt"});
            patch.physics = physics;
        }
        return native_environment_result(write);
    } catch (const std::exception& error) { return native_failure(error.what(), 2); }
}

SceneReply apply_native_actor_focus(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const char* message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& target = request.focus;
    if (target.actor.empty() || target.camera_id.empty()) return fail("INVALID_ARGUMENT", "Focus requires Actor and camera identity");
    auto* scene = request.archive_session ? native_editor_state().scene.get() : ensure_native_editor_scene();
    if (!scene || (request.archive_session && (request.archive_session->empty() || *request.archive_session != scene->archive_session)) ||
        !native_scene_request_matches(*scene, target.scene))
        return fail("PROJECT_CONTEXT_CHANGED", "Focus does not identify the active scene");
    // Web identifies the Actor by GUID/name and the camera by exact ID, never a numeric handle or display-name alias.
    const auto actor_it = std::find_if(scene->actors.begin(), scene->actors.end(), [&](const auto& actor) {
        return actor.name == target.actor || actor.actor_guid == target.actor;
    });
    if (actor_it == scene->actors.end() || !actor_it->geometry) return fail("ACTOR_NOT_FOUND", "Actor has no focus geometry");
    const auto camera_it = std::find_if(scene->cameras.begin(), scene->cameras.end(), [&](const auto& camera) {
        return camera.camera_id == target.camera_id;
    });
    if (camera_it == scene->cameras.end() || !camera_it->engine_camera) return fail("CAMERA_NOT_FOUND", "Camera identity is unavailable");
    auto& actor = *actor_it;
    auto& camera = *camera_it;
    auto& focus = reply.focus;
    focus.archive_session = scene->archive_session;
    focus.scene = scene->route; focus.actor = actor.name; focus.actor_guid = actor.actor_guid;
    const auto aabb = actor.geometry->get_aabb();
    const auto position = actor.geometry->get_position();
    const auto scale = actor.geometry->get_scale();
    double diagonal_sq = 0;
    // Preserve the existing native focus framing; source bounds are authoritative Actor geometry.
    for (int axis = 0; axis < 3; ++axis) {
        const double center = position[axis] + (static_cast<double>(aabb[axis]) + aabb[axis + 3]) * 0.5 * scale[axis];
        const double extent = (static_cast<double>(aabb[axis + 3]) - aabb[axis]) * scale[axis];
        if (!std::isfinite(center) || std::abs(center) > std::numeric_limits<float>::max())
            return fail("INVALID_BOUNDS", "Actor focus bounds are not finite");
        focus.center[axis] = static_cast<float>(center);
        diagonal_sq += extent * extent;
    }
    const double distance = std::max(std::sqrt(diagonal_sq) * 2.0, 1.0);
    const double camera_z = static_cast<double>(focus.center[2]) - distance;
    if (!std::isfinite(distance) || distance > std::numeric_limits<float>::max() ||
        !std::isfinite(camera_z) || std::abs(camera_z) > std::numeric_limits<float>::max())
        return fail("INVALID_BOUNDS", "Actor focus pose is not finite");
    focus.distance = static_cast<float>(distance);
    const std::array<float, 3> next_position{focus.center[0], focus.center[1], static_cast<float>(camera_z)};
    camera.engine_camera->set(next_position, {0,0,1}, {0,1,0}, camera.engine_camera->get_pose().fov);
    focus.camera = {camera.camera_id, camera.name, camera.engine_camera->get_handle(), camera.engine_camera->get_pose().position,
        camera.engine_camera->get_pose().forward, camera.engine_camera->get_pose().world_up, camera.engine_camera->get_pose().fov};
    return reply;
}

SceneReply apply_native_actor_transform(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const std::string& message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.transform;
    const auto finite = [](const auto& value) {
        return !value || std::all_of(value->begin(), value->end(), [](float x) { return std::isfinite(x); });
    };
    if (trim_ascii(patch.actor).empty() || (!patch.position && !patch.rotation && !patch.scale) ||
        !finite(patch.position) || !finite(patch.rotation) || !finite(patch.scale))
        return fail("INVALID_ARGUMENT", "Transform requires an Actor and at least one finite vector");
    auto* scene = request.archive_session ? native_editor_state().scene.get() : ensure_native_editor_scene();
    if (request.archive_session) {
        if (!scene || request.archive_session->empty() || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, patch.scene))
            return fail("PROJECT_CONTEXT_CHANGED", "Transform does not identify the active scene");
    } else {
        scene = resolve_native_editor_scene_request(scene, normalize_route(patch.scene));
    }
    auto* actor = find_native_actor(*scene, patch.actor);
    if (!actor && patch.actor.rfind("__shell_", 0) != 0) actor = find_native_actor(*scene, "__shell_" + patch.actor);
    if (!actor) return fail("ACTOR_NOT_FOUND", "Actor not found: " + patch.actor);

    // The patch fills the actor's authoritative TRS cache, then the geometry takes one complete
    // write: Geometry only accepts a whole transform, so the fields this patch omits come from the
    // cache. Three separate writes used to lose every field but the last.
    if (patch.position) actor->position = *patch.position;
    if (patch.rotation) actor->rotation = *patch.rotation;
    if (patch.scale) actor->scale = *patch.scale;
    if (actor->geometry) actor->geometry->set_transform(actor->position, actor->rotation, actor->scale);
    actor->actor_version = std::max(actor->actor_version + 1, 1);
    // Read back through the geometry: with one complete command queued, transform_view() merges the
    // pending write immediately, so this reports the value just applied rather than the old snapshot.
    reply.transform = {scene->route, actor->name, actor->actor_guid, actor->actor_version,
        actor->geometry ? actor->geometry->get_position() : actor->position,
        actor->geometry ? actor->geometry->get_rotation() : actor->rotation,
        actor->geometry ? actor->geometry->get_scale() : actor->scale};
    return reply;
}

ActorPropertiesSnapshot actor_properties_snapshot(const NativeEditorScene& scene, const NativeEditorActor& actor) {
    ActorPropertiesSnapshot result;
    result.scene = scene.route; result.actor = actor.name; result.actor_guid = actor.actor_guid;
    result.actor_version = actor.actor_version; result.follow_camera = actor.follow_camera;
    if (actor.optics) result.visible = actor.optics->get_state().visible;
    if (actor.mechanics) {
        const auto mechanics_state = actor.mechanics->get_state();
        result.mechanics = ActorMechanicsSnapshot{mechanics_state.mass, mechanics_state.restitution,
            mechanics_state.damping, mechanics_state.body_type, mechanics_state.linear_lock,
            mechanics_state.angular_lock};
    }
    result.camera_lock = {actor.camera_lock_enabled, actor.camera_lock_position_offset, actor.camera_lock_rotation_offset};
    return result;
}

SceneReply apply_native_actor_properties(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const std::string& message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.properties;
    if (const auto invalid = ActorPropertiesService::validate_patch(patch))
        return fail(invalid.code.c_str(), invalid.message);
    auto* scene = request.archive_session ? native_editor_state().scene.get() : ensure_native_editor_scene();
    if (request.archive_session) {
        if (!scene || request.archive_session->empty() || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, patch.scene))
            return fail("PROJECT_CONTEXT_CHANGED", "Properties do not identify the active scene");
    } else scene = resolve_native_editor_scene_request(scene, normalize_route(patch.scene));
    auto* actor = find_native_actor(*scene, patch.actor);
    if (!actor) return fail("ACTOR_NOT_FOUND", "Actor not found: " + patch.actor);
    std::optional<std::string> new_name;
    if (patch.name) {
        std::vector<std::string> other_names;
        other_names.reserve(scene->actors.size());
        for (const auto& other : scene->actors) {
            if (&other != actor) other_names.push_back(other.name);
        }
        if (const auto invalid = ActorPropertiesService::validate_rename(*patch.name, scene->read_only, other_names))
            return fail(invalid.code.c_str(), invalid.message);
        new_name = ActorPropertiesService::normalized_name(*patch.name);
    }
    // Validate every component and derived value before importing assets or
    // mutating any property. A mixed invalid patch must never partly apply.
    if (patch.visible && !actor->optics) return fail("COMPONENT_NOT_FOUND", "Actor has no optics component");
    if (patch.mechanics && !actor->mechanics) return fail("COMPONENT_NOT_FOUND", "Actor has no mechanics component");
    if (patch.animation_sources && !actor_animation_player(*actor)) return fail("COMPONENT_NOT_FOUND", "Actor has no geometry");
    auto* camera = patch.camera_lock && patch.camera_lock->enabled ? find_native_camera(*scene, {}) : nullptr;
    if (patch.camera_lock && patch.camera_lock->enabled && (!camera || !camera->engine_camera || !actor->engine_actor))
        return fail("COMPONENT_NOT_FOUND", "Camera-lock requires an active camera and a loaded actor");
    auto position_offset = actor->camera_lock_position_offset;
    auto rotation_offset = actor->camera_lock_rotation_offset;
    if (patch.camera_lock) {
        position_offset = patch.camera_lock->position_offset.value_or(position_offset);
        rotation_offset = patch.camera_lock->rotation_offset.value_or(rotation_offset);
        if (patch.camera_lock->enabled && !patch.camera_lock->position_offset) {
            const auto camera_position = camera->engine_camera->get_pose().position;
            const auto position = actor->geometry ? actor->geometry->get_position() : actor->position;
            for (size_t index = 0; index < 3; ++index) position_offset[index] = camera_position[index] - position[index];
            if (!std::all_of(position_offset.begin(), position_offset.end(), [](float value) { return std::isfinite(value); }))
                return fail("INVALID_ARGUMENT", "Derived camera offset exceeds the finite range");
        }
    }
    std::optional<std::vector<std::string>> sources;
    if (patch.animation_sources) {
        try {
            sources = prepare_animation_sources(*scene, *patch.animation_sources, patch.transient_assets);
            request_actor_animation_sources(*scene, *actor, *sources);
        } catch (const std::exception& error) { return fail("INVALID_ARGUMENT", error.what()); }
    }
    if (patch.visible) actor->optics->set_properties({.visible = *patch.visible});
    if (patch.follow_camera) {
        actor->follow_camera = *patch.follow_camera;
        if (actor->engine_actor) actor->engine_actor->set_follow_camera(*patch.follow_camera);
    }
    if (patch.mechanics) {
        const auto& values = *patch.mechanics;
        if (values.mass) actor->mechanics->set_properties({.mass = *values.mass});
        if (values.restitution) actor->mechanics->set_properties({.restitution = *values.restitution});
        if (values.damping) actor->mechanics->set_properties({.damping = *values.damping});
        if (values.body_type) actor->mechanics->set_properties({.body_type = *values.body_type});
        if (values.linear_lock) actor->mechanics->set_properties({.linear_lock = std::array<bool, 3>{(*values.linear_lock)[0], (*values.linear_lock)[1], (*values.linear_lock)[2]}});
        if (values.angular_lock) actor->mechanics->set_properties({.angular_lock = std::array<bool, 3>{(*values.angular_lock)[0], (*values.angular_lock)[1], (*values.angular_lock)[2]}});
    }
    if (patch.camera_lock) {
        actor->camera_lock_enabled = patch.camera_lock->enabled;
        actor->camera_lock_position_offset = position_offset;
        actor->camera_lock_rotation_offset = rotation_offset;
        auto& controller = Corona::Systems::CameraFollowController::instance();
        if (patch.camera_lock->enabled) controller.set_target(actor->engine_actor->get_handle(),
            camera->engine_camera->get_handle(), position_offset[0], position_offset[1], position_offset[2]);
        else controller.clear_target();
    }
    if (new_name) actor->name = *new_name;
    actor->actor_version = std::max(actor->actor_version + 1, 1);
    ++scene->tree_revision;
    reply.properties = actor_properties_snapshot(*scene, *actor);
    reply.properties.requested_animation_sources = std::move(sources);
    return reply;
}

// Non-Web boundary adapters retain their own public argument shapes. All
// component writes, version changes and camera side effects live above.
enum class ActorPropertyAdapter { State, Mechanics, CameraLock };
NativeResult set_native_actor_properties(const NativeRequest& request, ActorPropertyAdapter kind) {
    try {
        const auto values = arg_object(request.args, 2);
        SceneRequest write{SceneMethod::UpdateActor};
        auto& patch = write.properties;
        patch.scene = arg_string(request.args, 0); patch.actor = arg_string(request.args, 1);
        // The script channel owns no assets: whatever it imports stays in memory and is
        // never written back to the scene archive.
        patch.transient_assets = true;
        if (values.contains("archive_session")) write.archive_session = values.at("archive_session").get<std::string>();
        if (kind == ActorPropertyAdapter::State) {
            patch.visible = actor_data_bool(values, {"visible"});
            patch.follow_camera = actor_data_bool(values, {"follow_camera"});
            if (values.contains("animation_sources")) patch.animation_sources = AnimationConfig::parse_sources(values.at("animation_sources"));
        } else if (kind == ActorPropertyAdapter::Mechanics) {
            ActorMechanicsPatch mechanics;
            mechanics.mass = actor_data_float(values, {"mass"});
            mechanics.restitution = actor_data_float(values, {"restitution"});
            mechanics.damping = actor_data_float(values, {"damping"});
            const auto body = json_string_value(values, {"body_type"});
            if (!body.empty()) mechanics.body_type = normalize_body_type(body);
            else if (const auto enabled = actor_data_bool(values, {"physics_enabled"})) mechanics.body_type = *enabled ? "dynamic" : "static";
            const auto locks = [&](const char* key, auto& target) {
                const auto value = values.find(key);
                if (value != values.end() && value->is_array() && value->size() >= 3)
                    target = std::array<bool, 3>{json_bool_at(*value, 0), json_bool_at(*value, 1), json_bool_at(*value, 2)};
            };
            locks("linear_lock", mechanics.linear_lock); locks("angular_lock", mechanics.angular_lock);
            patch.mechanics = mechanics;
        } else {
            const auto enabled = actor_data_bool(values, {"enabled", "lock_to_camera"});
            if (!enabled) return native_failure("Camera-lock requires boolean enabled", 2);
            ActorCameraLockPatch lock; lock.enabled = *enabled;
            const auto vector = [&](const char* key, auto& target) {
                if (!values.contains(key)) return;
                const auto& value = values.at(key);
                if (!value.is_array() || value.size() != 3) throw std::invalid_argument("Camera offset requires three finite numbers");
                std::array<float, 3> copy;
                for (size_t index = 0; index < 3; ++index) {
                    if (!value[index].is_number() || !std::isfinite(value[index].get<double>()) ||
                        std::abs(value[index].get<double>()) > std::numeric_limits<float>::max())
                        throw std::invalid_argument("Camera offset requires three finite numbers");
                    copy[index] = value[index].get<float>();
                }
                target = copy;
            };
            vector("position_offset", lock.position_offset); vector("rotation_offset", lock.rotation_offset);
            patch.camera_lock = lock;
        }
        const auto reply = apply_native_actor_properties(write);
        if (!reply.error_code.empty()) return native_failure(reply.error_message, 2);
        auto* scene = native_editor_state().scene.get();
        return native_success({{"status", "success"}, {"scene", scene->route},
            {"actor", actor_to_json(*scene, *find_native_actor(*scene, reply.properties.actor_guid))}});
    } catch (const std::exception& error) { return native_failure(error.what(), 2); }
}

// JSON is confined to the non-Web Python/network adapter. The write itself is
// the same typed operation as V8, not another transform implementation.
NativeResult set_native_editor_actor_transform(const std::string& scene_route_arg,
                                               const std::string& actor_name,
                                               const nlohmann::json& transform_data) {
    SceneRequest request{SceneMethod::SetActorTransform};
    request.transform = {scene_route_arg, actor_name,
        transform_float3_value(transform_data, "position", "pos"),
        transform_float3_value(transform_data, "rotation", "rot"),
        transform_float3_value(transform_data, "scale", "scl")};
    if (transform_data.contains("archive_session")) request.archive_session = transform_data.at("archive_session").get<std::string>();
    const auto reply = apply_native_actor_transform(request);
    if (!reply.error_code.empty()) return native_failure(reply.error_message, 2);
    auto* scene = native_editor_state().scene.get();
    const auto* actor = find_native_actor(*scene, reply.transform.actor_guid);
    return native_success({
        {"status", "success"},
        {"scene", scene->route},
        {"actor", actor_to_json(*scene, *actor)},
    });
}

enum class PendingNetworkActorApplyResult {
    Applied,
    Retry,
    Discard,
};

struct PendingNetworkActorApplyOutcome {
    PendingNetworkActorApplyResult result{PendingNetworkActorApplyResult::Discard};
    std::string error;
    nlohmann::json actor = nlohmann::json::object();
};

PendingNetworkActorApplyOutcome pending_actor_outcome(
    PendingNetworkActorApplyResult result,
    std::string error = {},
    nlohmann::json actor = nlohmann::json::object()) {
    return {result, std::move(error), std::move(actor)};
}

PendingNetworkActorApplyOutcome apply_pending_network_actor_state(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& actor_data);

PendingNetworkActorApplyOutcome apply_pending_network_actor_create(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& model_path,
    nlohmann::json actor_data) {
    if (trim_ascii(actor_guid).empty() || !actor_data.is_object()) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor create requires an object and stable GUID");
    }
    actor_data["actor_guid"] = actor_guid;
    actor_data["skip_if_exists"] = true;
    actor_data["update_if_exists"] = true;
    const auto source_path = !model_path.empty()
        ? model_path
        : json_string_value(actor_data, {"asset_path", "route", "path", "model"});
    if (trim_ascii(source_path).empty()) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor create has no logical resource path");
    }
    try {
        auto* scene = resolve_native_editor_scene_request(
            ensure_native_editor_scene(), normalize_route(scene_name));
        const auto requested_name = trim_ascii(actor_data.value("name", std::string{}));
        auto* existing = find_native_actor(*scene, actor_guid);
        if (!requested_name.empty()) {
            auto* named = find_native_actor(*scene, requested_name);
            if (named && named->actor_guid != actor_guid) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                             "Remote actor name already belongs to another GUID");
            }
        }
        if (!existing) {
            const auto actor_type = actor_data.value("actor_type", std::string{"model"});
            const auto created = create_native_editor_actor(
                scene_name, source_path, actor_type, actor_data);
            if (!created.success) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, created.error);
            }
        }
        auto* actor = find_native_actor(*scene, actor_guid);
        if (!actor) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                         "Created remote actor is not materialized yet");
        }
        if (auto sys = get_network_system(); actor->engine_actor) {
            Corona::Network::NetworkCommand command{};
            command.kind = Corona::Network::NetworkCommandKind::RegisterActorIdentity;
            command.guid = actor_guid;
            command.handle = actor->engine_actor->get_handle();
            command.flag = false;
            Corona::Network::push_network_command(std::move(command));
        }
        actor_data["route"] = actor->route;
        actor_data["path"] = actor->route;
        actor_data["model"] = actor->route;
        actor_data["asset_path"] = actor->resolved_asset_path;
        return apply_pending_network_actor_state(
            actor_guid, scene_name, actor_data);
    } catch (const std::exception& error) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, error.what());
    }
}

PendingNetworkActorApplyOutcome apply_pending_network_actor_transform(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& transform_data) {
    if (trim_ascii(actor_guid).empty() || !transform_data.is_object()) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor transform is malformed");
    }
    if (!transform_float3_value(transform_data, "position", "pos") ||
        !transform_float3_value(transform_data, "rotation", "rot") ||
        !transform_float3_value(transform_data, "scale", "scl")) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor transform requires three finite vectors");
    }
    try {
        auto* scene = resolve_native_editor_scene_request(
            ensure_native_editor_scene(), normalize_route(scene_name));
        if (!find_native_actor(*scene, actor_guid)) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                         "Remote transform target is not materialized yet");
        }
        const auto applied = set_native_editor_actor_transform(scene_name, actor_guid, transform_data);
        if (!applied.success) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, applied.error);
        }
        auto* actor = find_native_actor(*scene, actor_guid);
        if (actor) ++scene->tree_revision;
        return pending_actor_outcome(PendingNetworkActorApplyResult::Applied, {},
                                     actor ? actor_to_json(*scene, *actor) : nlohmann::json::object());
    } catch (const std::exception& error) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, error.what());
    }
}

PendingNetworkActorApplyOutcome apply_pending_network_actor_state(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& actor_data) {
    if (trim_ascii(actor_guid).empty() || !actor_data.is_object() || actor_data.empty()) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor state requires an object and stable GUID");
    }
    const auto payload_guid = actor_data.value("actor_guid", actor_guid);
    if (payload_guid != actor_guid) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor state GUID does not match its envelope");
    }
    try {
        auto* scene = resolve_native_editor_scene_request(
            ensure_native_editor_scene(), normalize_route(scene_name));
        auto* actor = find_native_actor(*scene, actor_guid);
        if (!actor) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                         "Remote state target is not materialized yet");
        }

        const auto requested_name = trim_ascii(actor_data.value("name", actor->name));
        std::optional<std::vector<Corona::Resource::IkChain>> incoming_ik_chains;
        if (actor_data.contains("ik_chains")) {
            try {
                incoming_ik_chains = IkChainConfig::parse(actor_data.at("ik_chains"));
            } catch (const std::invalid_argument& error) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard, error.what());
            }
        }
        if (requested_name.empty()) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor state contains an empty name");
        }
        const auto duplicate = std::any_of(
            scene->actors.begin(), scene->actors.end(), [&](const NativeEditorActor& other) {
                return &other != actor && other.name == requested_name;
            });
        if (duplicate) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor state contains a duplicate name");
        }
        const auto geometry = actor_data.value("geometry", nlohmann::json::object());
        if (!geometry.is_object()) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor geometry must be an object");
        }
        for (const char* field : {"position", "rotation", "scale"}) {
            if (geometry.contains(field) &&
                !actor_data_float3(actor_data, field)) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                             std::string("Remote actor state has invalid geometry.") + field);
            }
        }

        const auto optics = actor_data.value("optics", nlohmann::json::object());
        if (!optics.is_object()) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor optics must be an object");
        }
        if (optics.contains("diffuse") && !json_float3_value(optics["diffuse"])) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor diffuse value is invalid");
        }
        for (const char* key : {"metallic", "roughness", "specular", "shininess"}) {
            if (optics.contains(key) &&
                (!optics[key].is_number() || !std::isfinite(optics[key].get<double>()))) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                             std::string("Remote actor optics.") + key + " is invalid");
            }
        }

        const auto mechanics = actor_data.value("mechanics", nlohmann::json::object());
        if (!mechanics.is_object()) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor mechanics must be an object");
        }
        for (const char* key : {"mass", "restitution", "damping"}) {
            if (mechanics.contains(key) && !actor_data_float(mechanics, {key})) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                             std::string("Remote actor mechanics.") + key + " is invalid");
            }
        }

        const auto camera_lock = actor_data.value("camera_lock", nlohmann::json::object());
        if (!camera_lock.is_object()) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor camera lock must be an object");
        }
        std::optional<bool> camera_lock_enabled;
        if (!camera_lock.empty()) {
            camera_lock_enabled = actor_data_bool(camera_lock, {"enabled", "lock_to_camera"});
            if (!camera_lock_enabled) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                             "Remote actor camera lock requires enabled");
            }
            for (const char* key : {"position_offset", "rotation_offset"}) {
                if (camera_lock.contains(key) && !json_float3_value(camera_lock[key])) {
                    return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                                 std::string("Remote camera ") + key + " is invalid");
                }
            }
            if (*camera_lock_enabled) {
                auto* camera = find_native_camera(*scene, {});
                if (!camera || !camera->engine_camera || !actor->engine_actor) {
                    return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                                 "Remote camera lock target is not ready");
                }
            }
        }

        auto merged_actor_data = actor_to_json(*scene, *actor);
        merged_actor_data.merge_patch(actor_data);
        merged_actor_data["actor_guid"] = actor_guid;
        auto desired = native_actor_from_snapshot(merged_actor_data);
        const auto desired_route = normalize_route(desired.route);
        const bool replacing_resource = !desired_route.empty() && desired_route != normalize_route(actor->route);
        if (!replacing_resource && actor_data.contains("animation_sources")) {
            if (!actor->geometry)
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, "Remote animation geometry is not ready");
            try {
                request_actor_animation_sources(*scene, *actor, desired.persisted_animation_sources);
            } catch (const std::invalid_argument& error) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard, error.what());
            }
        }
        if (!replacing_resource && incoming_ik_chains) {
            if (!actor->geometry) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                             "Remote IK geometry is not materialized yet");
            }
            const auto apply_ik = [&](const Corona::Resource::SkeletonData* skeleton) {
                if (skeleton) incoming_ik_chains = IkChainConfig::parse(actor_data.at("ik_chains"), skeleton);
                auto storage = Corona::SharedDataHub::instance().geometry_storage().try_acquire_write(
                    actor->geometry->get_handle());
                if (!storage) throw std::runtime_error("Remote IK geometry storage is unavailable");
                auto saved = IkChainConfig::serialize(*incoming_ik_chains, skeleton);
                IkChainConfig::apply_snapshot(storage->ik_chains, storage->ik_chains_revision,
                                              std::move(*incoming_ik_chains), skeleton);
                actor->persisted_ik_chains = std::move(saved);
            };
            try {
                const auto model_id = actor->geometry->get_model_id();
                if (model_id != 0) {
                    auto resource = read_resource_for_ui<Corona::Resource::Scene>(model_id);
                    if (!resource.valid()) {
                        return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                                     "Remote IK model resource is unavailable");
                    }
                    if (!resource->data.skeleton && !incoming_ik_chains->empty()) {
                        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                                     "Remote IK model has no skeleton");
                    }
                    apply_ik(resource->data.skeleton ? &*resource->data.skeleton : nullptr);
                } else {
                    apply_ik(nullptr); // Resolve saved names after asynchronous import.
                }
            } catch (const std::invalid_argument& error) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Discard, error.what());
            }
        }
        if (!replacing_resource && actor_data.contains("model_node_overrides")) {
            if (!actor->geometry) return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                "Remote model node geometry is not materialized yet");
            const auto applicable = desired.model_node_resource == actor->model_node_current_resource
                ? parse_native_model_node_overrides(desired.persisted_model_node_overrides)
                : std::vector<Corona::API::ModelNodeOverride>{};
            if (!actor->geometry->set_model_node_overrides(applicable)) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                    "Remote model node state is not ready or could not be applied");
            }
            actor->persisted_model_node_overrides = desired.persisted_model_node_overrides;
            actor->model_node_resource = desired.model_node_resource;
            actor->model_node_unmatched = desired.model_node_unmatched;
        }
        if (replacing_resource) {
            const auto replacement_path = resolve_native_actor_asset_path(*scene, desired);
            std::error_code resource_ec;
            if (!std::filesystem::is_regular_file(replacement_path, resource_ec) || resource_ec) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                             "Remote actor replacement resource is unavailable");
            }
            const auto actor_index = static_cast<std::size_t>(actor - scene->actors.data());
            auto& loaded = add_native_actor_to_scene(*scene, std::move(desired), replacement_path);
            loaded.load_status = ActorLoadStatus::Loaded;
            if (loaded.optics) loaded.optics->set_properties({.visible = loaded.persisted_visible});
            if (loaded.mechanics && loaded.actor_type != "ui_image") {
                loaded.mechanics->set_properties({.body_type = loaded.persisted_body_type});
            }
            if (scene->actors[actor_index].engine_actor) {
                scene->engine_scene->remove_actor(scene->actors[actor_index].engine_actor.get());
            }
            scene->actors.erase(scene->actors.begin() + static_cast<std::ptrdiff_t>(actor_index));
            actor = find_native_actor(*scene, actor_guid);
            if (!actor) {
                return pending_actor_outcome(PendingNetworkActorApplyResult::Retry,
                                             "Remote actor replacement did not materialize");
            }
            if (auto sys = get_network_system(); actor->engine_actor) {
                Corona::Network::NetworkCommand command{};
                command.kind = Corona::Network::NetworkCommandKind::RegisterActorIdentity;
                command.guid = actor_guid;
                command.handle = actor->engine_actor->get_handle();
                command.flag = false;
                Corona::Network::push_network_command(std::move(command));
            }
        }

        actor->name = requested_name;
        actor->actor_type = actor_data.value("actor_type", actor->actor_type);
        actor->asset_id = actor_data.value("asset_id", actor->asset_id);
        actor->model_ref = actor_data.value("model_ref", actor->model_ref);
        actor->entity_type = actor_data.value("entity_type", actor->entity_type);
        actor->semantic_role = actor_data.value("semantic_role", actor->semantic_role);
        actor->source_plan_id = actor_data.value("source_plan_id", actor->source_plan_id);
        actor->source_batch_id = actor_data.value("source_batch_id", actor->source_batch_id);
        actor->source_scene_version = std::max(
            actor_data.value("source_scene_version", actor->source_scene_version), 1);
        actor->actor_version = std::max(actor_data.value("actor_version", actor->actor_version), 1);
        actor->persisted_snapshot = actor_data;

        bool transform_touched = false;
        if (auto value = actor_data_float3(actor_data, "position")) {
            actor->position = *value;
            transform_touched = true;
        }
        if (auto value = actor_data_float3(actor_data, "rotation")) {
            actor->rotation = *value;
            transform_touched = true;
        }
        if (auto value = actor_data_float3(actor_data, "scale")) {
            actor->scale = *value;
            transform_touched = true;
        }
        if (transform_touched && actor->geometry) {
            actor->geometry->set_transform(actor->position, actor->rotation, actor->scale);
        }
        if (auto value = actor_data_bool(actor_data, {"visible"})) {
            actor->persisted_visible = *value;
            if (actor->optics) actor->optics->set_properties({.visible = *value});
        }
        if (auto value = actor_data_bool(actor_data, {"follow_camera"})) {
            actor->follow_camera = *value;
            if (actor->engine_actor) actor->engine_actor->set_follow_camera(*value);
        }

        if (optics.contains("diffuse")) {
            const auto diffuse = json_float3_value(optics["diffuse"]);
            if (!diffuse) return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                                       "Remote actor diffuse value is invalid");
            actor->persisted_optics.diffuse = *diffuse;
        }
        const auto apply_optics_float = [&](const char* key, std::optional<float>& target) {
            if (!optics.contains(key)) return true;
            if (!optics[key].is_number() || !std::isfinite(optics[key].get<double>())) return false;
            target = optics[key].get<float>();
            return true;
        };
        if (!apply_optics_float("metallic", actor->persisted_optics.metallic) ||
            !apply_optics_float("roughness", actor->persisted_optics.roughness) ||
            !apply_optics_float("specular", actor->persisted_optics.specular) ||
            !apply_optics_float("shininess", actor->persisted_optics.shininess)) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                         "Remote actor optics contains a non-finite value");
        }
        if (actor_data.contains("material")) {
            NativeMaterialConfig::validate(actor_data["material"]);
            actor->persisted_optics.material = actor_data["material"];
        }
        apply_native_actor_optics_state(*actor);

        if (actor->mechanics) {
            if (auto value = actor_data_float(mechanics, {"mass"})) actor->mechanics->set_properties({.mass = *value});
            if (auto value = actor_data_float(mechanics, {"restitution"})) actor->mechanics->set_properties({.restitution = *value});
            if (auto value = actor_data_float(mechanics, {"damping"})) actor->mechanics->set_properties({.damping = *value});
            const auto bt = json_string_value(mechanics, {"body_type"});
            if (!bt.empty()) {
                actor->persisted_body_type = normalize_body_type(bt);
                actor->mechanics->set_properties({.body_type = actor->persisted_body_type});
            } else if (auto value = actor_data_bool(mechanics, {"physics_enabled"})) {
                // 向后兼容
                actor->persisted_body_type = *value ? "dynamic" : "static";
                actor->mechanics->set_properties({.body_type = actor->persisted_body_type});
            }
            const auto linear = mechanics.find("linear_lock");
            if (linear != mechanics.end() && linear->is_array() && linear->size() >= 3) {
                actor->mechanics->set_properties({.linear_lock = std::array<bool, 3>{json_bool_at(*linear, 0), json_bool_at(*linear, 1), json_bool_at(*linear, 2)}});
            }
            const auto angular = mechanics.find("angular_lock");
            if (angular != mechanics.end() && angular->is_array() && angular->size() >= 3) {
                actor->mechanics->set_properties({.angular_lock = std::array<bool, 3>{json_bool_at(*angular, 0), json_bool_at(*angular, 1), json_bool_at(*angular, 2)}});
            }
        }

        if (!camera_lock.empty()) {
            actor->camera_lock_enabled = *camera_lock_enabled;
            if (camera_lock.contains("position_offset")) {
                const auto value = json_float3_value(camera_lock["position_offset"]);
                if (!value) return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                                         "Remote camera position offset is invalid");
                actor->camera_lock_position_offset = *value;
            }
            if (camera_lock.contains("rotation_offset")) {
                const auto value = json_float3_value(camera_lock["rotation_offset"]);
                if (!value) return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                                         "Remote camera rotation offset is invalid");
                actor->camera_lock_rotation_offset = *value;
            }
            if (*camera_lock_enabled) {
                auto* camera = find_native_camera(*scene, {});
                const auto& offset = actor->camera_lock_position_offset;
                Corona::Systems::CameraFollowController::instance().set_target(
                    actor->engine_actor->get_handle(), camera->engine_camera->get_handle(),
                    offset[0], offset[1], offset[2]);
            } else {
                Corona::Systems::CameraFollowController::instance().clear_target();
            }
        }

        ++scene->tree_revision;
        return pending_actor_outcome(PendingNetworkActorApplyResult::Applied, {},
                                     actor_to_json(*scene, *actor));
    } catch (const nlohmann::json::exception& error) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard, error.what());
    } catch (const std::exception& error) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, error.what());
    }
}

PendingNetworkActorApplyOutcome apply_pending_network_actor_delete(
    const std::string& actor_guid,
    const std::string& scene_name) {
    if (trim_ascii(actor_guid).empty()) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Discard,
                                     "Remote actor delete requires a stable GUID");
    }
    try {
        auto* scene = resolve_native_editor_scene_request(
            ensure_native_editor_scene(), normalize_route(scene_name));
        if (!find_native_actor(*scene, actor_guid)) {
            return pending_actor_outcome(PendingNetworkActorApplyResult::Applied);
        }
        const auto removed = remove_native_editor_actor(scene_name, actor_guid);
        return removed.success
            ? pending_actor_outcome(PendingNetworkActorApplyResult::Applied)
            : pending_actor_outcome(PendingNetworkActorApplyResult::Retry, removed.error);
    } catch (const std::exception& error) {
        return pending_actor_outcome(PendingNetworkActorApplyResult::Retry, error.what());
    }
}

std::filesystem::path editor_root_path() {
    return Utils::editor_directory();
}

std::filesystem::path editor_ini_path() {
    return Utils::executable_directory() / "CoronaEditor.ini";
}

std::filesystem::path runtime_data_dir() {
    return editor_root_path() / "data";
}

std::string settings_value(const std::string& section,
                           const std::string& key,
                           const std::string& fallback);

class ScopedComInitialization {
   public:
    ScopedComInitialization() {
#ifdef _WIN32
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    }

    ~ScopedComInitialization() {
#ifdef _WIN32
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
#endif
    }

    [[nodiscard]] bool available() const {
#ifdef _WIN32
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
#else
        return false;
#endif
    }

   private:
#ifdef _WIN32
    HRESULT result_{E_FAIL};
#endif
};

enum class NativeFileDialogMode {
    OpenFile,
    SaveFile,
    PickFolder,
};

struct NativeFileDialogFilter {
    std::wstring name;
    std::wstring pattern;
};

struct NativeFileDialogOptions {
    NativeFileDialogMode mode{NativeFileDialogMode::OpenFile};
    std::filesystem::path default_path;
    std::wstring title;
    std::wstring default_filename;
    std::wstring default_extension;
    std::vector<NativeFileDialogFilter> filters;
};

std::optional<std::filesystem::path> show_native_file_dialog(
    const NativeFileDialogOptions& dialog_options) {
#ifdef _WIN32
    ScopedComInitialization com;
    if (!com.available()) {
        return std::nullopt;
    }

    IFileDialog* dialog = nullptr;
    const CLSID& dialog_class = dialog_options.mode == NativeFileDialogMode::SaveFile
                                    ? CLSID_FileSaveDialog
                                    : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(
            dialog_class,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options{};
    HRESULT result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (dialog_options.mode == NativeFileDialogMode::PickFolder) {
            options |= FOS_PICKFOLDERS;
        } else if (dialog_options.mode == NativeFileDialogMode::OpenFile) {
            options |= FOS_FILEMUSTEXIST;
        } else {
            options |= FOS_OVERWRITEPROMPT;
        }
        result = dialog->SetOptions(options);
    }
    if (SUCCEEDED(result) && !dialog_options.title.empty()) {
        result = dialog->SetTitle(dialog_options.title.c_str());
    }
    std::vector<COMDLG_FILTERSPEC> native_filters;
    native_filters.reserve(dialog_options.filters.size());
    for (const auto& filter : dialog_options.filters) {
        native_filters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }
    if (SUCCEEDED(result) && !native_filters.empty() &&
        dialog_options.mode != NativeFileDialogMode::PickFolder) {
        result = dialog->SetFileTypes(
            static_cast<UINT>(native_filters.size()), native_filters.data());
    }
    if (SUCCEEDED(result) && !dialog_options.default_filename.empty()) {
        result = dialog->SetFileName(dialog_options.default_filename.c_str());
    }
    if (SUCCEEDED(result) && !dialog_options.default_extension.empty()) {
        result = dialog->SetDefaultExtension(dialog_options.default_extension.c_str());
    }

    std::error_code ec;
    auto initial_dir = dialog_options.default_path;
    if (std::filesystem::is_regular_file(initial_dir, ec)) {
        initial_dir = initial_dir.parent_path();
    }
    ec.clear();
    if (SUCCEEDED(result) && std::filesystem::is_directory(initial_dir, ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial_dir.c_str(),
                nullptr,
                IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    result = SUCCEEDED(result) ? dialog->Show(nullptr) : result;
    if (FAILED(result)) {
        dialog->Release();
        return std::nullopt;
    }

    IShellItem* selected = nullptr;
    result = dialog->GetResult(&selected);
    dialog->Release();
    if (FAILED(result) || selected == nullptr) {
        return std::nullopt;
    }

    PWSTR selected_path = nullptr;
    result = selected->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    selected->Release();
    if (FAILED(result) || selected_path == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path path{selected_path};
    CoTaskMemFree(selected_path);
    return path;
#else
    (void)dialog_options;
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> show_native_path_dialog(
    bool pick_folder,
    const std::filesystem::path& default_path,
    const wchar_t* title) {
    NativeFileDialogOptions options;
    options.mode = pick_folder ? NativeFileDialogMode::PickFolder
                               : NativeFileDialogMode::OpenFile;
    options.default_path = default_path;
    options.title = title ? title : L"";
    if (!pick_folder) {
        options.filters = {
            {L"Corona / Mitsuba 3 / PBRT v4", L"scene.ini;*.xml;*.pbrt"},
            {L"所有文件", L"*.*"},
        };
    }
    return show_native_file_dialog(options);
}

std::optional<std::filesystem::path> open_project_file_native() {
    const auto default_path = path_from_utf8(settings_value(
        "General",
        "default_path",
        path_to_utf8(runtime_data_dir())));
    return show_native_path_dialog(
        false,
        default_path,
        L"打开 Corona 或外部场景");
}



const std::vector<NativeFileDialogFilter>* resource_dialog_filters(
    const std::string& file_type) {
    static const std::vector<NativeFileDialogFilter> model_filters{
        {L"三维模型", L"*.obj;*.ply;*.fbx;*.3ds;*.dae;*.usd;*.usda;*.usdz;*.gltf;*.glb;*.usdc;*.stl"},
        {L"所有文件", L"*.*"},
    };
    static const std::vector<NativeFileDialogFilter> multimedia_filters{
        {L"音视频", L"*.mp4;*.avi;*.mov;*.mp3;*.wav"},
        {L"所有文件", L"*.*"},
    };
    static const std::vector<NativeFileDialogFilter> scene_filters{
        {L"Corona / Mitsuba 3 / PBRT v4", L"scene.ini;*.xml;*.pbrt"},
        {L"所有文件", L"*.*"},
    };
    static const std::vector<NativeFileDialogFilter> actor_filters{
        {L"Actor 文件", L"*.actor"},
        {L"所有文件", L"*.*"},
    };
    static const std::vector<NativeFileDialogFilter> image_filters{
        {L"图片文件", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.webp"},
        {L"所有文件", L"*.*"},
    };

    if (file_type == "model") return &model_filters;
    if (file_type == "multimedia") return &multimedia_filters;
    if (file_type == "scene") return &scene_filters;
    if (file_type == "actor") return &actor_filters;
    if (file_type == "ui_image") return &image_filters;
    return nullptr;
}

void restore_native_camera_lock_target(NativeEditorScene& scene) {
    auto& controller = Corona::Systems::CameraFollowController::instance();
    controller.clear_target();
    auto* camera = find_native_camera(scene, {});
    if (!camera || !camera->engine_camera) {
        return;
    }
    for (auto& actor : scene.actors) {
        if (!actor.camera_lock_enabled || !actor.engine_actor) {
            continue;
        }
        const auto& offset = actor.camera_lock_position_offset;
        controller.set_target(actor.engine_actor->get_handle(), camera->engine_camera->get_handle(),
                              offset[0], offset[1], offset[2]);
        return;
    }
}

std::optional<std::filesystem::path> select_resource_file_native(
    const std::filesystem::path& default_path,
    const std::string& file_type) {
    const auto* filters = resource_dialog_filters(file_type);
    if (!filters) {
        return std::nullopt;
    }
    NativeFileDialogOptions options;
    options.mode = NativeFileDialogMode::OpenFile;
    options.default_path = default_path;
    options.title = L"选择导入资源";
    options.filters = *filters;
    return show_native_file_dialog(options);
}

SceneReply import_native_resource(const SceneRequest& request,
    const ResourceFilePicker& picker = select_resource_file_native,
    const MediaFileImporter& importer = Corona::API::import_media) {
    SceneReply reply;
    if (const auto invalid = ResourceImportService::validate_request(request.archive_session, request.read_scene, request.file_type)) {
        reply.error_code = invalid.code;
        reply.error_message = invalid.message;
        return reply;
    }
    auto* scene = native_editor_state().scene.get();
    if (!scene || *request.archive_session != scene->archive_session ||
        !native_scene_request_matches(*scene, request.read_scene)) {
        reply.error_code = "PROJECT_CONTEXT_CHANGED";
        reply.error_message = "Resource import belongs to an inactive scene";
        return reply;
    }
    auto& imported = reply.resource_import;
    imported.archive_session = scene->archive_session;
    imported.scene = scene->route;
    imported.file_type = request.file_type;
    if (!ResourceImportService::supports_file_type(request.file_type)) {
        imported.status = "error";
        imported.code = "unsupported_file_type";
        imported.message = "Unsupported resource type: " + request.file_type;
        return reply;
    }
    const auto project_root = scene->project_root;
    const auto identity = scene->identity;
    const auto current = [&] {
        const auto* active = native_editor_state().scene.get();
        return active && active->identity == identity && active->archive_session == *request.archive_session
            && native_scene_request_matches(*active, request.read_scene);
    };
    const auto selected = picker(project_root, request.file_type);
    // The modal dialog can pump scene replacement; never dereference its old
    // scene or accept a selection (including cancel) on behalf of a new owner.
    if (!current()) {
        reply.error_code = "PROJECT_CONTEXT_CHANGED";
        reply.error_message = "Archive changed while choosing a resource";
        return reply;
    }
    if (!selected) {
        imported.status = "canceled";
        imported.message = "用户取消了文件选择";
        return reply;
    }
    const auto absolute_path = absolute_normalized_path(*selected);
    imported.path = route_for_project_storage(project_root, path_to_utf8(absolute_path));
    imported.actor_type = request.file_type;
    if (!ResourceImportService::requires_media_import(request.file_type)) {
        imported.status = "success";
        return reply;
    }
    const auto info = importer(path_to_utf8(absolute_path));
    ResourceImportService::MediaImportResult mapped;
    mapped.resource_id = info.resource_id;
    mapped.media_type = info.media_type;
    mapped.duration_seconds = info.duration_seconds;
    mapped.codec = info.codec;
    mapped.width = info.width;
    mapped.height = info.height;
    mapped.fps = info.fps;
    mapped.sample_rate = info.sample_rate;
    mapped.channels = info.channels;
    ResourceImportService::apply_media_import(imported, mapped, path_to_utf8(absolute_path.stem()));
    return reply;
}

std::string change_native_resource_audio(std::uint64_t resource_id, bool playing, bool loop = false) {
    auto* event_bus = Corona::Kernel::KernelContext::instance().event_bus();
    if (!event_bus) return "event_bus unavailable";
    if (!resource_id) return "invalid resource_id";
    if (playing) event_bus->publish<::Corona::Events::PlayAudioEvent>({resource_id, loop});
    else event_bus->publish<::Corona::Events::StopAudioEvent>({resource_id});
    return {};
}

void change_native_actor_audio(NativeEditorActor& actor, bool playing, bool loop = false) {
    if (playing) actor.acoustics->play(loop);
    else actor.acoustics->stop();
}

std::string compact_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d_%H%M%S");
    return out.str();
}

std::filesystem::path absolute_normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec) {
        absolute = path;
        ec.clear();
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    return ec ? absolute : canonical;
}

bool is_path_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(
        absolute_normalized_path(candidate), absolute_normalized_path(root), ec);
    if (ec || relative.empty() || relative == ".") {
        return !ec;
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return false;
        }
    }
    return true;
}

bool is_valid_project_dir(const std::filesystem::path& project_dir) {
    const auto* scene = native_editor_state().scene.get();
    if (scene && !scene->read_only && scene->project_root == project_dir) return true;
    if (!std::filesystem::is_directory(project_dir)) return false;
    const auto result = project_archive_call("project_info", {{"path", path_to_utf8(project_dir)}});
    return result.success && result.data.contains("project_path");
}

std::filesystem::path canonical_project_dir_for_settings(const std::filesystem::path& project_dir) {
    if (project_dir.empty()) {
        return {};
    }

    if (project_dir.is_absolute()) return absolute_normalized_path(project_dir);
    std::vector<std::filesystem::path> candidates;
    {
        candidates.push_back(std::filesystem::current_path() / project_dir);
        candidates.push_back(runtime_data_dir() / project_dir);
        candidates.push_back(editor_root_path() / project_dir);
    }

    for (const auto& candidate : candidates) {
        const auto absolute = absolute_normalized_path(candidate);
        if (is_valid_project_dir(absolute)) {
            return absolute;
        }
    }

    return absolute_normalized_path(project_dir.is_absolute() ? project_dir : runtime_data_dir() / project_dir);
}

std::string safe_project_dir_name(std::string name, const std::string& fallback) {
    name = trim_ascii(std::move(name));
    if (name.empty()) {
        name = fallback.empty() ? "project" : fallback;
    }
    for (char& c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name.empty() ? "project" : name;
}



std::string settings_value(const std::string& section, const std::string& key, const std::string& fallback) {
    auto result = project_archive_call("editor_settings", {{"ini", path_to_utf8(editor_ini_path())},
                                      {"section", section}, {"key", key}, {"fallback", fallback}});
    return result.success ? result.data.value("value", fallback) : fallback;
}

void update_editor_settings_section(const std::string& section, const std::map<std::string, std::string>& updates) {
    auto result = project_archive_call("editor_settings", {{"ini", path_to_utf8(editor_ini_path())},
                                      {"section", section}, {"updates", updates}});
    if (!result.success) throw std::runtime_error(result.error);
}

void add_recent_project_native(const std::filesystem::path& project_dir) {
    auto result = project_archive_call("editor_settings", {{"ini", path_to_utf8(editor_ini_path())},
        {"section", "History"}, {"recent", path_to_utf8(canonical_project_dir_for_settings(project_dir))}});
    if (!result.success) throw std::runtime_error(result.error);
}



NativeEditorActor* find_native_actor_by_guid(NativeEditorScene& scene,
                                            const std::string& actor_guid) {
    if (actor_guid.empty()) return nullptr;
    for (auto& actor : scene.actors) {
        if (actor.actor_guid == actor_guid) return &actor;
    }
    return nullptr;
}

nlohmann::json recent_projects_native() {
    auto history = project_archive_call("editor_settings", {{"ini", path_to_utf8(editor_ini_path())},
                                                           {"section", "History"}, {"recent_projects", true}});
    const auto recent = history.success ? history.data.value("projects", nlohmann::json::array()) : nlohmann::json::array();
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : recent) {
        if (!item.is_string()) {
            continue;
        }
        const auto project_dir = canonical_project_dir_for_settings(path_from_utf8(item.get<std::string>()));
        auto metadata = project_archive_call("project_info", {{"path", path_to_utf8(project_dir)}});
        const bool exists = metadata.success && metadata.data.contains("project_path");
        result.push_back({
            {"name", exists ? metadata.data.value("name", std::string{}) : path_to_utf8(project_dir.filename())},
            {"path", path_to_utf8(project_dir)},
            {"if_exists", exists},
            {"last_edited", exists ? metadata.data.value("last_opened", std::string{"-"}) : "-"},
        });
    }
    return result;
}


std::string detect_hostname_ipv4() {
    char host_name[256] = {};
    if (gethostname(host_name, sizeof(host_name)) != 0) {
        return "127.0.0.1";
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host_name, nullptr, &hints, &result) != 0) {
        return "127.0.0.1";
    }

    std::string fallback = "127.0.0.1";
    for (addrinfo* it = result; it; it = it->ai_next) {
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        char ip[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
            continue;
        }
        std::string candidate(ip);
        if (is_usable_ipv4(candidate)) {
            fallback = candidate;
            break;
        }
    }
    freeaddrinfo(result);
    return fallback;
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* value) {
    if (!value || !*value) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string ipv4_from_sockaddr(const sockaddr* address) {
    if (!address || address->sa_family != AF_INET) {
        return {};
    }
    const auto* addr = reinterpret_cast<const sockaddr_in*>(address);
    char ip[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
        return {};
    }
    return std::string(ip);
}

bool adapter_has_ipv4_gateway(const IP_ADAPTER_ADDRESSES* adapter) {
    for (auto* gateway = adapter ? adapter->FirstGatewayAddress : nullptr; gateway; gateway = gateway->Next) {
        if (gateway->Address.lpSockaddr &&
            gateway->Address.lpSockaddr->sa_family == AF_INET) {
            return true;
        }
    }
    return false;
}
#endif

std::string detect_wlan_ipv4() {
#ifdef _WIN32
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    std::string wlan_with_gateway;
    std::string wlan_fallback;
    std::string gateway_fallback;
    std::string any_fallback;

    ULONG buffer_size = 15000;
    std::vector<unsigned char> buffer(buffer_size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER |
                  GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(
        AF_INET, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        result = GetAdaptersAddresses(
            AF_INET, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &buffer_size);
    }

    if (result == NO_ERROR) {
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }

            std::string adapter_name = wide_to_utf8(adapter->FriendlyName);
            if (adapter->AdapterName) {
                adapter_name += " ";
                adapter_name += adapter->AdapterName;
            }
            const bool is_wlan = looks_like_wlan_adapter(adapter_name);
            const bool has_gateway = adapter_has_ipv4_gateway(adapter);

            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                const std::string ip = ipv4_from_sockaddr(unicast->Address.lpSockaddr);
                if (!is_usable_ipv4(ip)) {
                    continue;
                }
                if (is_wlan && has_gateway && wlan_with_gateway.empty()) {
                    wlan_with_gateway = ip;
                }
                if (is_wlan && wlan_fallback.empty()) {
                    wlan_fallback = ip;
                }
                if (has_gateway && gateway_fallback.empty()) {
                    gateway_fallback = ip;
                }
                if (any_fallback.empty()) {
                    any_fallback = ip;
                }
            }
        }
    }

    std::string selected = !wlan_with_gateway.empty() ? wlan_with_gateway :
                           !wlan_fallback.empty() ? wlan_fallback :
                           !gateway_fallback.empty() ? gateway_fallback :
                           !any_fallback.empty() ? any_fallback :
                           detect_hostname_ipv4();
    if (started) {
        WSACleanup();
    }
    return selected;
#else
    return detect_hostname_ipv4();
#endif
}




bool network_snapshot_session_active() {
    const auto snapshot = Corona::Network::NetworkStateChannel::instance().snapshots().read();
    return snapshot && snapshot->active;
}




bool network_connect_to_peer(const std::string& ip, std::uint16_t port, const std::string& name) {
    Corona::Network::NetworkCommand command{};
    command.kind = Corona::Network::NetworkCommandKind::ConnectToPeer;
    command.port = port;
    command.params = {ip, name};
    const auto reply = Corona::Network::submit_and_wait(std::move(command));
    return reply && reply->ok;
}


/// 协作对象锁走"命令 + 有界等回执"，还原成调用方原本期望的结果类型。




std::vector<NetworkPeerSnapshot> read_network_peers() {
    // 读快照：CEF 线程不再直接调用 NetworkSystem。
    std::vector<NetworkPeerSnapshot> result;
    const auto snapshot = Corona::Network::NetworkStateChannel::instance().snapshots().read();
    if (!snapshot) return result;
    result.reserve(snapshot->peers.size());
    for (const auto& peer : snapshot->peers) {
        result.push_back({peer.ip, peer.name, peer.role,
                          peer.stable_id, peer.port, peer.project_id, peer.last_seen_ms});
    }
    return result;
}

NetworkSessionSnapshot read_network_session() {
    NetworkSessionSnapshot result;
    if (const auto snapshot = Corona::Network::NetworkStateChannel::instance().snapshots().read()) {
        result.active = snapshot->active;
        result.role = snapshot->role;
        result.peer_count = snapshot->peer_count;
        result.host_address = snapshot->host_address;
        result.host_port = snapshot->host_port;
        result.listen_port = snapshot->listen_port;
    }
    result.local_ip = detect_wlan_ipv4();
    result.discovered_peers = read_network_peers();
    return result;
}

void read_transform_from_actor_json(const nlohmann::json& actor, float transform[9]) {
    if (!actor.contains("geometry")) {
        return;
    }
    const auto& geo = actor["geometry"];
    if (geo.contains("position") && geo["position"].is_array() && geo["position"].size() >= 3) {
        transform[0] = geo["position"][0].get<float>();
        transform[1] = geo["position"][1].get<float>();
        transform[2] = geo["position"][2].get<float>();
    }
    if (geo.contains("rotation") && geo["rotation"].is_array() && geo["rotation"].size() >= 3) {
        transform[3] = geo["rotation"][0].get<float>();
        transform[4] = geo["rotation"][1].get<float>();
        transform[5] = geo["rotation"][2].get<float>();
    }
    if (geo.contains("scale") && geo["scale"].is_array() && geo["scale"].size() >= 3) {
        transform[6] = geo["scale"][0].get<float>();
        transform[7] = geo["scale"][1].get<float>();
        transform[8] = geo["scale"][2].get<float>();
    }
}


std::shared_ptr<Corona::Systems::NetworkSystem> require_network_system() {
    return get_network_system();
}

void initialize_actor_create_packed(Corona::Network::ActorCreatePacked& packed) {
    std::memset(&packed, 0, sizeof(packed));
    packed.visible = true;
    packed.bEnableLighting = true;
    packed.metallic = 0.0f;
    packed.roughness = 0.5f;
    packed.specular = 0.5f;
    packed.specularTint = 0.0f;
    packed.sheen = 0.0f;
    packed.sheenTint = 0.5f;
    packed.clearcoat = 0.0f;
    packed.clearcoatGloss = 1.0f;
    packed.ambient[0] = 0.2f;
    packed.ambient[1] = 0.2f;
    packed.ambient[2] = 0.2f;
    packed.diffuse[0] = 0.8f;
    packed.diffuse[1] = 0.8f;
    packed.diffuse[2] = 0.8f;
    packed.specular_color[0] = 1.0f;
    packed.specular_color[1] = 1.0f;
    packed.specular_color[2] = 1.0f;
    packed.shininess = 32.0f;
}

bool is_network_editor_seed_actor(const NativeEditorActor& actor) {
    if (actor.actor_guid.empty() || actor.route.empty() || !actor.engine_actor ||
        !actor.geometry || actor.actor_type == "actor") {
        return false;
    }
    const auto is_ai_framework_name = [](const std::string& value) {
        static const std::unordered_set<std::string> kNames = {
            "__room_box",
            "__room_terrain",
            "__terrain_grass",
            "__terrain_boundary",
            "__interior_floor",
            "__foundation_surface",
        };
        return kNames.contains(value) || value.starts_with("__shell_");
    };
    if (actor.name.starts_with("__") && !is_ai_framework_name(actor.name)) return false;
    return true;
}

void broadcast_network_editor_actor(
    const NativeEditorScene& scene, const NativeEditorActor& actor) {
    const auto actor_data = actor_to_json(scene, actor);
    float transform[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
    read_transform_from_actor_json(actor_data, transform);

    Corona::Network::ActorCreatePacked optics;
    initialize_actor_create_packed(optics);
    optics.visible = actor_data.value("visible", true);
    optics.bEnableLighting = true;
    optics.metallic = actor_data.value("optics", nlohmann::json::object()).value("metallic", 0.0f);
    optics.roughness = actor_data.value("optics", nlohmann::json::object()).value("roughness", 0.5f);
    optics.specular = actor_data.value("optics", nlohmann::json::object()).value("specular", 0.5f);
    optics.shininess = actor_data.value("optics", nlohmann::json::object()).value("shininess", 32.0f);
    {
        Corona::Network::NetworkCommand command{};
        command.kind = Corona::Network::NetworkCommandKind::RegisterActorIdentity;
        command.guid = actor.actor_guid;
        command.handle = actor.engine_actor->get_handle();
        command.flag = true;
        Corona::Network::push_network_command(std::move(command));
    }
    // JSON is part of the existing network protocol, entirely on the native
    // side. The Web call and its reply remain owned, typed V8 values.
    {
        Corona::Network::NetworkCommand command{};
        command.kind = Corona::Network::NetworkCommandKind::BroadcastActorCreate;
        command.guid = actor.actor_guid;
        command.scene = scene.route;
        command.name = actor.route;
        command.json = actor_data.dump();
        for (int i = 0; i < 9; ++i) command.transform[static_cast<std::size_t>(i)] = transform[i];
        const auto* packed_bytes = reinterpret_cast<const std::uint8_t*>(&optics);
        command.blob.assign(packed_bytes, packed_bytes + sizeof(optics));
        Corona::Network::push_network_command(std::move(command));
    }
}

void broadcast_network_editor_actor_state(const NativeEditorScene& scene, const NativeEditorActor& actor) {
    if (network_snapshot_session_active() && is_network_editor_seed_actor(actor)) {
        Corona::Network::NetworkCommand command{};
        command.kind = Corona::Network::NetworkCommandKind::BroadcastActorStateUpdate;
        command.guid = actor.actor_guid;
        command.scene = scene.route;
        command.json = actor_to_json(scene, actor).dump();
        Corona::Network::push_network_command(std::move(command));
    }
}

// Every collaborative mutation assembles the same two command shapes; keeping
// the assembly here stops each scene entry from repeating the field mapping.
void broadcast_editor_actor_transform(const ActorTransformSnapshot& value) {
    float transform[9];
    std::copy(value.position.begin(), value.position.end(), transform);
    std::copy(value.rotation.begin(), value.rotation.end(), transform + 3);
    std::copy(value.scale.begin(), value.scale.end(), transform + 6);
    Corona::Network::NetworkCommand command{};
    command.kind = Corona::Network::NetworkCommandKind::BroadcastActorTransformUpdate;
    command.guid = value.actor_guid;
    command.scene = value.scene;
    for (int i = 0; i < 9; ++i) command.transform[static_cast<std::size_t>(i)] = transform[i];
    Corona::Network::push_network_command(std::move(command));
}

void broadcast_editor_actor_delete(const ActorDeleteSnapshot& value) {
    Corona::Network::NetworkCommand command{};
    command.kind = Corona::Network::NetworkCommandKind::BroadcastActorDelete;
    command.guid = value.actor_guid;
    command.scene = value.scene;
    command.name = value.old_name;
    Corona::Network::push_network_command(std::move(command));
}

std::size_t seed_network_editor_state(
    Corona::Systems::NetworkSystem::SessionRole role) {
    if (role != Corona::Systems::NetworkSystem::SessionRole::Host) return 0;

    auto* scene = ensure_native_editor_scene();
    if (scene == nullptr) return 0;
    const auto scene_route = normalize_route(scene->route);
    const auto scene_leaf = path_to_utf8(path_from_utf8(scene_route).filename());
    if (scene_route.starts_with("__") || scene_leaf.starts_with("__")) return 0;

    std::size_t seeded_actor_count = 0;
    for (const auto& actor : scene->actors) {
        if (!is_network_editor_seed_actor(actor)) continue;

        broadcast_network_editor_actor(*scene, actor);
        ++seeded_actor_count;
    }
    return seeded_actor_count;
}

// Shared by live Web screenshots and the non-Web offscreen camera adapter.
// Offscreen configuration remains at its caller; saving never changes pose or output mode.
bool save_native_camera_screenshot(NativeEditorCamera& camera, const std::string& path) {
    camera.engine_camera->set_size(std::max(camera.width, 1), std::max(camera.height, 1));
    return camera.engine_camera->save_screenshot_sync(path);
}

std::optional<std::filesystem::path> select_native_screenshot_path(const std::filesystem::path& project_root) {
    NativeFileDialogOptions options;
    options.mode = NativeFileDialogMode::SaveFile;
    options.default_path = project_root;
    options.title = L"保存截图";
    options.default_filename = L"screenshot_";
    const auto timestamp = compact_timestamp();
    options.default_filename.append(timestamp.begin(), timestamp.end());
    options.default_filename += L".png";
    options.default_extension = L"png";
    options.filters = {{L"PNG 图片", L"*.png"}};
    return show_native_file_dialog(options);
}

SceneReply capture_native_screenshot(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const char* message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& target = request.screenshot;
    if (target.scene.empty() || target.camera_id.empty() || !request.archive_session || request.archive_session->empty() ||
        (target.path && (target.path->empty() || target.path->find('\0') != std::string::npos)))
        return fail("INVALID_ARGUMENT", "Screenshot requires scene, camera and archive identity and a valid optional path");
    auto* scene = native_editor_state().scene.get();
    const auto same_scene = [&] {
        return scene && scene->archive_session == *request.archive_session && native_scene_request_matches(*scene, target.scene);
    };
    if (!same_scene()) return fail("PROJECT_CONTEXT_CHANGED", "Screenshot does not identify the active archive");
    const auto find_camera = [&]() -> NativeEditorCamera* {
        const auto found = std::find_if(scene->cameras.begin(), scene->cameras.end(), [&](const auto& camera) {
            return camera.camera_id == target.camera_id;
        });
        return found != scene->cameras.end() && found->engine_camera ? &*found : nullptr;
    };
    auto* camera = find_camera();
    if (!camera) return fail("CAMERA_NOT_FOUND", "Screenshot camera identity is unavailable");
    const auto handle = camera->engine_camera->get_handle();
    auto path = target.path;
    reply.screenshot = {"canceled", *request.archive_session, scene->route, target.camera_id, ""};
    if (!path) {
        const auto selected = select_native_screenshot_path(scene->project_root);
        // A modal dialog can pump native events: reacquire all scene/camera state.
        scene = native_editor_state().scene.get();
        if (!same_scene()) return fail("PROJECT_CONTEXT_CHANGED", "Archive changed while choosing a screenshot path");
        camera = find_camera();
        if (!camera || camera->engine_camera->get_handle() != handle)
            return fail("CAMERA_NOT_FOUND", "Screenshot camera changed while choosing a path");
        if (!selected) return reply;
        path = path_to_utf8(absolute_normalized_path(*selected));
    }
    if (!save_native_camera_screenshot(*camera, *path)) return fail("CAPTURE_FAILED", "Screenshot save failed");
    reply.screenshot.status = "success";
    reply.screenshot.path = *path;
    return reply;
}


}  // namespace

NativeEditorState& native_editor_state() {
    // Deliberately never destroyed. Tearing the scene down from the CRT's atexit table
    // runs NativeEditorCamera::~NativeEditorCamera, whose Corona::API::Camera destructor
    // enqueues a camera release through SharedDataHub. Destruction order between the two
    // translation units' statics is unspecified, so the hub can already be gone and the
    // process faults on exit (see the CameraWindowIdentityTests teardown crash). Leaking
    // the state at process exit is the cheap way to keep the ordering question out of it.
    static auto* state = new NativeEditorState{};
    return *state;
}

EntityId resolve_native_actor_handle_by_guid(const std::string& scene_id,
                                                   const std::string& archive_session,
                                                   const std::string& actor_guid,
                                                   std::string& error_code,
                                                   std::string& error_message) {
    auto* scene = native_editor_state().scene.get();
    if (!scene || scene->archive_session != archive_session ||
        !native_scene_request_matches(*scene, scene_id)) {
        error_code = "PROJECT_CONTEXT_CHANGED";
        error_message = "Gizmo belongs to an inactive scene";
        return 0;
    }
    const auto* actor = find_native_actor_by_guid(*scene, actor_guid);
    if (!actor || !actor->engine_actor) {
        error_code = "ACTOR_NOT_FOUND";
        error_message = "Gizmo actor is no longer available";
        return 0;
    }
    return actor->engine_actor->get_handle();
}

CameraPropertiesSnapshot camera_properties_snapshot(const NativeEditorScene& scene, const NativeEditorCamera& camera) {
    CameraPropertiesSnapshot result;
    const auto& engine = *camera.engine_camera;
    const auto projection = engine.get_projection();
    result.archive_session = scene.archive_session; result.scene = scene.route;
    result.camera_id = camera.camera_id; result.name = camera.name; result.handle = engine.get_handle();
    const auto pose = engine.get_pose();
    result.position = pose.position; result.forward = pose.forward;
    result.world_up = pose.world_up; result.fov = pose.fov;
    result.projection_x_sign = projection[0]; result.near_plane = projection[1]; result.far_plane = projection[2];
    result.width = camera.width; result.height = camera.height; result.move_speed = camera.move_speed;
    result.view_open = requested_camera_view_open(camera); result.view_x = camera.view_x; result.view_y = camera.view_y;
    result.view_width = camera.view_width; result.view_height = camera.view_height;
    return result;
}

bool camera_list_snapshot(const NativeEditorScene& scene, CameraListSnapshot& result) {
    result.archive_session = scene.archive_session; result.scene = scene.route;
    if (!scene.cameras.empty())
        result.active_camera_id = scene.cameras[std::min(scene.active_camera_index, scene.cameras.size() - 1)].camera_id;
    result.cameras.reserve(scene.cameras.size());
    for (const auto& camera : scene.cameras) {
        if (!camera.engine_camera) return false;
        CameraReadSnapshot value;
        value.properties = camera_properties_snapshot(scene, camera);
        const auto& engine = *camera.engine_camera;
        value.output_mode = engine.get_output_mode(); value.render_backend = engine.get_render_backend();
        value.vision_render_mode = engine.get_vision_render_mode(); value.vision_denoiser = engine.get_vision_denoiser();
        value.vision_accumulation = engine.get_vision_accumulation(); value.vision_denoise = engine.get_vision_denoise();
        value.vision_stable_planes = engine.get_vision_stable_planes(); value.deletable = camera.deletable;
        result.cameras.push_back(std::move(value));
    }
    return true;
}

Corona::Ui::Cef::SceneTreeActorRow scene_tree_actor_row(const NativeEditorActor& actor) {
    Corona::Ui::Cef::SceneTreeActorRow row;
    row.name = actor.name; row.actor_guid = actor.actor_guid; row.route = actor.route;
    row.actor_version = actor.actor_version;
    row.actor_type = actor.actor_type;
    row.handle = actor.engine_actor ? actor.engine_actor->get_handle() : 0;
    row.visible = actor.optics ? actor.optics->get_state().visible : true;
    row.gpu_build_state = actor.geometry ? actor.geometry->get_gpu_build_state() : "Ready";
    const bool pending = row.gpu_build_state == "PendingImport" || row.gpu_build_state == "PendingBuild";
    row.load_status = row.gpu_build_state == "Failed" ? "gpu_failed"
        : (pending ? "gpu_uploading" : actor_load_status_name(actor.load_status));
    row.audio_resource_id = actor.actor_type == "audio" ? std::to_string(actor.audio_resource_id) : std::string{};
    row.has_load_error = actor.load_status != ActorLoadStatus::Loaded;
    if (row.has_load_error) {
        row.load_error_code = actor.load_error_code; row.load_error_message = actor.load_error_message;
        row.load_error_resource_path = actor.route; row.load_error_path = actor.resolved_asset_path;
    }
    if (actor.geometry) {
        const auto counts = actor.geometry->get_model_node_counts();
        row.model_node_count = counts.total; row.model_node_root_count = counts.roots;
    }
    return row;
}

SceneReply apply_native_model_node(const SceneRequest& request) {
    SceneReply reply;
    auto fail = [&](const char* code, const std::string& message) { reply.error_code = code; reply.error_message = message; return reply; };
    auto* scene = native_editor_state().scene.get();
    if (!scene || !request.archive_session || scene->archive_session != *request.archive_session ||
        !native_scene_request_matches(*scene, request.method == SceneMethod::ReadModelNodes ? request.model_node_page.scene : request.model_node.scene))
        return fail("PROJECT_CONTEXT_CHANGED", "Model node operation belongs to an inactive archive");
    if (request.method == SceneMethod::SetModelNodeState && scene->read_only)
        return fail("READ_ONLY", "Cannot edit model nodes in a read-only scene");
    const auto& identity = request.method == SceneMethod::ReadModelNodes ? request.model_node_page.actor_guid : request.model_node.actor_guid;
    auto* actor = find_native_actor_by_guid(*scene, identity);
    if (!actor || !actor->geometry) return fail("ACTOR_NOT_FOUND", "Model actor is unavailable");
    if (request.method == SceneMethod::ReadModelNodes) {
        const auto& page_request = request.model_node_page;
        const auto nodes = actor->geometry->get_model_nodes();
        const bool actor_visible = actor->optics ? actor->optics->get_state().visible : actor->persisted_visible;
        auto& result = reply.model_nodes = SceneModelNodeService::project_page(nodes, page_request, actor_visible);
        result.archive_session = scene->archive_session; result.scene = scene->route; result.actor_guid = actor->actor_guid; result.actor = actor->name;
        result.actor_version = actor->actor_version;
        result.status = result.nodes.empty() ? actor->geometry->get_gpu_build_state() : "ready";
        return reply;
    }
    const auto& patch = request.model_node;
    if (!patch.expected_actor_version || *patch.expected_actor_version != actor->actor_version)
        return fail("ACTOR_VERSION_CONFLICT", "Model node edit is based on an older Actor version");
    const auto nodes = actor->geometry->get_model_nodes();
    const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) { return node.node_index == patch.node_index; });
    if (found == nodes.end()) return fail("NODE_NOT_FOUND", "Model node is unavailable or not loaded");
    const auto state = SceneModelNodeService::derive_patched_state(*found, patch);
    if (!actor->geometry->set_model_node_state(patch.node_index, state)) return fail("NODE_EDIT_REJECTED", "Model node edit was rejected");
    auto [active, unmatched] = ModelNodeConfig::partition(actor->persisted_model_node_overrides, actor->model_node_resource, actor->model_node_current_resource, SceneModelNodeService::node_keys(nodes));
    for (auto& group : unmatched) actor->model_node_unmatched.push_back(std::move(group));
    nlohmann::json patch_json = nlohmann::json::object();
    if (patch.position) patch_json["position"] = *patch.position;
    if (patch.rotation) patch_json["rotation"] = *patch.rotation;
    if (patch.scale) patch_json["scale"] = *patch.scale;
    if (patch.visible) patch_json["visible"] = *patch.visible;
    actor->persisted_model_node_overrides = ModelNodeConfig::merge(active, patch.node_index, state.node_path, patch_json); actor->model_node_resource = actor->model_node_current_resource; ++actor->actor_version;
    const auto updated = actor->geometry->get_model_nodes(); const auto it = std::find_if(updated.begin(), updated.end(), [&](const auto& node) { return node.node_index == patch.node_index; });
    const auto child_count = static_cast<std::size_t>(std::count_if(updated.begin(), updated.end(), [&](const auto& node) { return node.parent_index == static_cast<std::int32_t>(patch.node_index); }));
    auto& result = reply.model_node; result.archive_session = scene->archive_session; result.scene = scene->route; result.actor_guid = actor->actor_guid; result.actor = actor->name; result.actor_version = actor->actor_version; result.parent_index = it->parent_index; result.status = "ready"; result.nodes.push_back(SceneModelNodeService::make_snapshot(*it, child_count, actor->optics ? actor->optics->get_state().visible : actor->persisted_visible));
    return reply;
}

SceneReply apply_native_camera_properties(const SceneRequest& request, bool editor_input = true) {
    SceneReply reply;
    const auto fail = [&](const char* code, const std::string& message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.camera_properties;
    if (const auto invalid = CameraPropertiesService::validate_view_lifecycle(patch, editor_input))
        return fail(invalid.code.c_str(), invalid.message);
    if (const auto invalid = CameraPropertiesService::validate_identity(
            patch, request.archive_session.has_value() && !request.archive_session->empty()))
        return fail(invalid.code.c_str(), invalid.message);
    auto* scene = native_editor_state().scene.get();
    if (!scene || scene->archive_session != *request.archive_session || !native_scene_request_matches(*scene, patch.scene))
        return fail("PROJECT_CONTEXT_CHANGED", "Camera patch does not identify the active archive");
    const auto found = std::find_if(scene->cameras.begin(), scene->cameras.end(), [&](const auto& camera) {
        return camera.camera_id == patch.camera_id;
    });
    if (found == scene->cameras.end() || !found->engine_camera)
        return fail("CAMERA_NOT_FOUND", "Camera identity is unavailable");
    auto& camera = *found;
    auto& engine = *camera.engine_camera;
    std::optional<std::string> new_name;
    if (patch.name) {
        std::vector<std::string> other_names;
        other_names.reserve(scene->cameras.size());
        for (const auto& other : scene->cameras) {
            if (&other != &camera) other_names.push_back(other.name);
        }
        if (const auto invalid = CameraPropertiesService::validate_rename(*patch.name, scene->read_only, other_names))
            return fail(invalid.code.c_str(), invalid.message);
        new_name = CameraPropertiesService::normalized_name(*patch.name);
    }
    const auto projection = engine.get_projection();
    if (const auto invalid = CameraPropertiesService::validate_numeric(patch, projection))
        return fail(invalid.code.c_str(), invalid.message);
    if (const auto invalid = CameraPropertiesService::validate_editor_input(
            patch, editor_input, Corona::API::is_editor_camera_input_enabled()))
        return fail(invalid.code.c_str(), invalid.message);
    const auto sign = patch.projection_x_sign.value_or(projection[0]);
    const auto near_plane = patch.near_plane.value_or(projection[1]);
    const auto far_plane = patch.far_plane.value_or(projection[2]);
    // Every field is validated before applying a name, pose or window property.
    if (new_name && camera.name != *new_name) {
        camera.name = *new_name;
        ++scene->tree_revision;
    }
    if (patch.position || patch.forward || patch.world_up || patch.fov) {
        const auto pose = engine.get_pose();
        engine.set(patch.position.value_or(pose.position), patch.forward.value_or(pose.forward),
            patch.world_up.value_or(pose.world_up), patch.fov.value_or(pose.fov));
    }
    if (patch.projection_x_sign || patch.near_plane || patch.far_plane) engine.set_projection(sign, near_plane, far_plane);
    if (patch.width || patch.height) {
        camera.width = patch.width.value_or(camera.width); camera.height = patch.height.value_or(camera.height);
        engine.set_size(camera.width, camera.height);
    }
    if (patch.has_view()) {
        camera.view_open = patch.view_open.value_or(camera.view_open);
        camera.view_x = patch.view_x.value_or(camera.view_x); camera.view_y = patch.view_y.value_or(camera.view_y);
        camera.view_width = patch.view_width.value_or(camera.view_width); camera.view_height = patch.view_height.value_or(camera.view_height);
        camera.move_speed = patch.move_speed.value_or(camera.move_speed);
        if (!editor_input && patch.view_open) {
            camera.window_state->requested_open.store(camera.view_open ? 1 : 0);
            engine.set_view({camera.view_open, camera.view_x, camera.view_y, camera.view_width,
                             camera.view_height, camera.move_speed});
        } else if (patch.view_x || patch.view_y || patch.view_width || patch.view_height) {
            engine.set_view({std::nullopt, camera.view_x, camera.view_y, camera.view_width,
                             camera.view_height, camera.move_speed});
        } else if (patch.move_speed) {
            // A speed-only update must not replay saved window geometry over the embedded viewport.
            const auto view = engine.get_view();
            engine.set_view({std::nullopt, view.x, view.y, view.width, view.height,
                             camera.move_speed});
        }
    }
    reply.camera = camera_properties_snapshot(*scene, camera);
    reply.camera.projection_x_sign = sign; reply.camera.near_plane = near_plane; reply.camera.far_plane = far_plane;
    return reply;
}

SceneReply apply_native_camera_render_settings(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const char* message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& patch = request.camera_render_settings;
    if (const auto invalid = CameraRenderSettingsService::validate_patch(
            patch, request.method, request.archive_session.has_value() && !request.archive_session->empty()))
        return fail(invalid.code.c_str(), invalid.message.c_str());
    auto* scene = native_editor_state().scene.get();
    if (!scene || scene->archive_session != *request.archive_session || !native_scene_request_matches(*scene, patch.scene))
        return fail("PROJECT_CONTEXT_CHANGED", "Camera render settings belong to an inactive archive");
    if (scene->cameras.empty())
        return fail("CAMERA_NOT_FOUND", "Camera identity is unavailable");
    const auto camera_index = patch.camera_id.empty()
        ? std::min(scene->active_camera_index, scene->cameras.size() - 1)
        : static_cast<std::size_t>(std::distance(scene->cameras.begin(), std::find_if(
            scene->cameras.begin(), scene->cameras.end(), [&](const auto& camera) {
                return camera.camera_id == patch.camera_id;
            })));
    if (camera_index >= scene->cameras.size() || !scene->cameras[camera_index].engine_camera)
        return fail("CAMERA_NOT_FOUND", "Camera identity is unavailable");
    const auto found = scene->cameras.begin() + static_cast<std::ptrdiff_t>(camera_index);
    auto& engine = *found->engine_camera;
    if (patch.family) {
        const auto family = *patch.family == "pt" ? API::VisionSettingsFamily::Pt :
            *patch.family == "restir" ? API::VisionSettingsFamily::Restir :
            *patch.family == "reblur" ? API::VisionSettingsFamily::Reblur : API::VisionSettingsFamily::Relax;
        auto requested = API::get_requested_vision_settings(family, engine.get_handle());
        auto effective = API::get_effective_vision_settings(family, engine.get_handle());
        if (request.method == SceneMethod::ReadCameraRenderSettings) {
            effective.update(requested);
            auto& result = reply.camera_render_settings;
            result.archive_session = scene->archive_session; result.scene = scene->route;
            result.camera_id = found->camera_id; result.family = *patch.family; result.settings = std::move(effective);
            result.pending = false;
            return reply;
        }
        if (!patch.settings.is_object() || patch.settings.empty()) return fail("INVALID_ARGUMENT", "Vision settings must be a non-empty object");
        API::set_vision_settings(family, patch.settings, engine.get_handle());
        auto& result = reply.camera_render_settings;
        result.archive_session = scene->archive_session; result.scene = scene->route;
        result.camera_id = found->camera_id; result.family = *patch.family; result.settings = patch.settings;
        result.pending = true;
        return reply;
    }
    if (patch.output_mode) engine.set_output_mode(*patch.output_mode);
    if (patch.vision_render_mode) engine.set_vision_render_mode(*patch.vision_render_mode);
    if (patch.vision_denoiser) engine.set_vision_denoiser(*patch.vision_denoiser);
    if (patch.vision_accumulation) engine.set_vision_accumulation(*patch.vision_accumulation);
    if (patch.vision_denoise) engine.set_vision_denoise(*patch.vision_denoise);
    if (patch.vision_stable_planes) engine.set_vision_stable_planes(*patch.vision_stable_planes);
    if (patch.render_backend) engine.set_render_backend(*patch.render_backend);
    auto& result = reply.camera_render_settings;
    result.archive_session = scene->archive_session; result.scene = scene->route; result.camera_id = found->camera_id;
    result.render_backend = patch.render_backend.value_or(engine.get_render_backend());
    result.output_mode = patch.output_mode.value_or(engine.get_output_mode());
    result.vision_render_mode = patch.vision_render_mode.value_or(engine.get_vision_render_mode());
    result.vision_denoiser = patch.vision_denoiser.value_or(engine.get_requested_vision_denoiser());
    result.vision_accumulation = patch.vision_accumulation.value_or(engine.get_requested_vision_accumulation());
    result.vision_denoise = patch.vision_denoise.value_or(engine.get_requested_vision_denoise());
    result.vision_stable_planes = patch.vision_stable_planes.value_or(engine.get_requested_vision_stable_planes());
    result.pending = true;
    return reply;
}

// JSON is restricted to the existing non-Web caller boundary, never the V8 path.
NativeResult apply_non_web_camera_properties(const NativeRequest& request, bool pose) {
    auto* scene = scene_for_request_route(request);
    const auto camera_name = arg_string(request.args, 1);
    auto* camera = find_native_camera(*scene, camera_name);
    if (!camera || !camera->engine_camera) return camera_not_found_result(camera_name);
    SceneRequest write{SceneMethod::UpdateCamera};
    write.archive_session = scene->archive_session;
    auto& patch = write.camera_properties;
    patch.scene = scene->route; patch.camera_id = camera->camera_id;
    const auto data = arg_object(request.args, 2);
    if (pose) {
        const auto vector = [&](const char* key, auto& value) {
            if (data.contains(key)) {
                value = json_float3_value(data[key]);
                if (!value) throw std::invalid_argument("Invalid camera vector");
            }
        };
        vector("position", patch.position); vector("forward", patch.forward); vector("world_up", patch.world_up);
        if (!patch.world_up) vector("up", patch.world_up);
        if (data.contains("fov")) patch.fov = data["fov"].get<float>();
        if (data.contains("projection_x_sign")) patch.projection_x_sign = data["projection_x_sign"].get<float>();
        if (data.contains("near_plane")) patch.near_plane = data["near_plane"].get<float>();
        if (data.contains("far_plane")) patch.far_plane = data["far_plane"].get<float>();
    } else {
        if (data.contains("view_open")) patch.view_open = data["view_open"].get<bool>();
        if (data.contains("move_speed")) patch.move_speed = data["move_speed"].get<float>();
        const auto integer = [&](const char* key, auto& value) { if (data.contains(key)) value = data[key].get<int>(); };
        integer("width", patch.width); integer("height", patch.height); integer("view_x", patch.view_x); integer("view_y", patch.view_y);
        integer("view_width", patch.view_width); integer("view_height", patch.view_height);
    }
    if (!patch.empty()) {
        // Caller permissions are enforced by the registry. Runtime scripts own pose input.
        const auto reply = apply_native_camera_properties(write, false);
        if (!reply.error_code.empty()) return native_failure(reply.error_message, 2);
    }
    return native_success({{"status", "success"}, {"scene", scene->route}, {"camera", camera_to_json(*camera)}});
}

namespace {

nlohmann::json typed_ik_chain_json(const IkChainSnapshot& chain) {
    return {{"id", chain.id}, {"bone_name", chain.bone_name}, {"mode", chain.mode},
            {"chain_length", chain.chain_length}, {"max_iterations", chain.max_iterations},
            {"weight", chain.weight}, {"tolerance", chain.tolerance}, {"damping", chain.damping},
            {"enabled", chain.enabled}, {"contact_weight_rise", chain.contact_weight_rise},
            {"contact_weight_decay", chain.contact_weight_decay}, {"contact_normal_offset", chain.contact_normal_offset},
            {"probe_max_drop", chain.probe_max_drop}, {"foot_height", chain.foot_height},
            {"plant_threshold", chain.plant_threshold}, {"target", chain.target}};
}

std::vector<IkChainSnapshot> typed_ik_chains(const std::vector<Corona::Resource::IkChain>& chains,
                                             const Corona::Resource::SkeletonData* skeleton,
                                             bool include_runtime) {
    std::vector<IkChainSnapshot> result;
    const auto encoded = IkChainConfig::serialize(chains, skeleton, include_runtime);
    result.reserve(encoded.size());
    for (const auto& value : encoded) {
        IkChainSnapshot chain;
        chain.id = value.value("id", std::string{}); chain.bone_name = value.value("bone_name", std::string{});
        chain.mode = value.value("mode", std::string{"foot_plant"});
        chain.chain_length = value.value("chain_length", 3); chain.max_iterations = value.value("max_iterations", 10);
        chain.weight = value.value("weight", 1.0f); chain.tolerance = value.value("tolerance", 1e-3f);
        chain.damping = value.value("damping", .88f); chain.enabled = value.value("enabled", true);
        chain.contact_weight_rise = value.value("contact_weight_rise", 8.0f);
        chain.contact_weight_decay = value.value("contact_weight_decay", .8f);
        chain.contact_normal_offset = value.value("contact_normal_offset", 0.0f);
        chain.probe_max_drop = value.value("probe_max_drop", .5f); chain.foot_height = value.value("foot_height", 0.0f);
        chain.plant_threshold = value.value("plant_threshold", .08f);
        if (value.contains("target") && value["target"].is_array() && value["target"].size() == 3)
            for (int i = 0; i < 3; ++i) chain.target[i] = value["target"][i].get<float>();
        if (include_runtime && value.contains("runtime")) {
            const auto& runtime = value["runtime"];
            IkRuntimeSnapshot state;
            state.has_target = runtime.value("has_target", false); state.weight = runtime.value("weight", 0.0f);
            state.grounded = runtime.value("grounded", false); state.releasing = runtime.value("releasing", false);
            if (runtime.contains("target") && runtime["target"].is_array() && runtime["target"].size() == 3)
                for (int i = 0; i < 3; ++i) state.target[i] = runtime["target"][i].get<float>();
            if (runtime.contains("normal") && runtime["normal"].is_array() && runtime["normal"].size() == 3)
                for (int i = 0; i < 3; ++i) state.normal[i] = runtime["normal"][i].get<float>();
            chain.runtime = state;
        }
        result.push_back(std::move(chain));
    }
    return result;
}

} // namespace

static NativeResult start_native_script_file(nlohmann::json payload, bool by_path) {
    const auto* scene = native_editor_state().scene.get();
    if (!scene || !matches_archive_session(*scene, payload)) {
        return native_failure("PROJECT_CONTEXT_CHANGED", 409);
    }
    if (by_path && !payload.contains("project_path")) payload["project_path"] = path_to_utf8(scene->project_root);
    if (by_path && !payload.contains("target_type")) {
        payload["target_type"] = ProjectScriptService::default_target_type(payload.value("actor_name", std::string{}));
    }
    const auto expected = path_from_utf8(payload.value("project_path", std::string{}));
    if (const auto invalid = ProjectScriptService::validate_script_project(
            scene->read_only, !expected.empty(),
            !expected.empty() && absolute_normalized_path(expected) == absolute_normalized_path(scene->project_root))) {
        return native_failure(invalid.message, 409);
    }
    const auto path = path_from_utf8(payload.value("path", std::string{}));
    if (const auto invalid = ProjectScriptService::validate_script_path(path)) {
        return native_failure(invalid.message, 400);
    }
    const auto root = absolute_normalized_path(scene->project_root);
    // The Python file host resolves the path, checks containment and verifies
    // the saved bytes against the revision before compiling them.
    payload["project_path"] = path_to_utf8(root);
    return invoke_python_script_service({"FileScripts", by_path ? "run_path" : "run_file", nlohmann::json::array({payload})});
}

SceneReply execute_scene_operation(const SceneRequest& request) {
    if (request.method == SceneMethod::ReadVisionAvailability) {
        SceneReply reply;
        reply.vision_available = Corona::API::is_vision_available();
        return reply;
    }
    if (request.method == SceneMethod::CreateActor) {
        SceneReply reply;
        const auto& patch = request.actor_create;
        if (patch.scene.empty() || patch.path.empty() || !request.archive_session || request.archive_session->empty() ||
            (patch.actor_type != "model" && patch.actor_type != "ui_image" && patch.actor_type != "actor" && patch.actor_type != "audio") ||
            (patch.actor_type == "audio" && (!patch.audio_resource_id || !*patch.audio_resource_id)) ||
            (patch.actor_type != "audio" && patch.audio_resource_id) ||
            (patch.actor_name && trim_ascii(*patch.actor_name).empty())) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Actor creation requires an observed archive, scene, source and supported Actor type";
            return reply;
        }
        auto* scene = native_editor_state().scene.get();
        if (!scene || scene->archive_session != *request.archive_session || !native_scene_request_matches(*scene, patch.scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Actor creation belongs to an inactive archive";
            return reply;
        }
        if (scene->read_only) {
            reply.error_code = "READ_ONLY";
            reply.error_message = "Cannot create an Actor in a read-only scene";
            return reply;
        }
        nlohmann::json data = {{"archive_session", *request.archive_session}};
        if (patch.actor_name) data["actor_name"] = *patch.actor_name;
        if (patch.audio_resource_id) data["audio_resource_id"] = std::to_string(*patch.audio_resource_id);
        // Reuse the same native business operation as Python/ScriptRuntime.
        // Its internal JSON object stays on this owner thread, never serialized
        // or passed to the renderer. Only the owned Actor projection crosses.
        const auto created = create_native_editor_actor(scene->route, patch.path, patch.actor_type, data);
        if (!created.success) {
            reply.error_code = "ACTOR_CREATE_FAILED";
            reply.error_message = created.error;
            return reply;
        }
        if (network_snapshot_session_active()) {
            auto* actor = find_native_actor(*scene, created.data.at("actor").at("actor_guid").get<std::string>());
            if (actor && is_network_editor_seed_actor(*actor)) broadcast_network_editor_actor(*scene, *actor);
        }
        SceneRequest read{SceneMethod::ReadActorProperties};
        read.archive_session = request.archive_session;
        read.read_scene = scene->route;
        read.read_actor = created.data.at("actor").at("actor_guid").get<std::string>();
        return execute_scene_operation(read);
    }
    if (request.method == SceneMethod::CreateCamera) {
        SceneReply reply;
        if (request.camera_create.scene.empty() || !request.archive_session || request.archive_session->empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Camera creation requires the observed archive and scene";
            return reply;
        }
        auto* scene = native_editor_state().scene.get();
        if (!scene || scene->archive_session != *request.archive_session ||
            !native_scene_request_matches(*scene, request.camera_create.scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Camera creation belongs to an inactive archive";
            return reply;
        }
        if (scene->read_only) {
            reply.error_code = "READ_ONLY";
            reply.error_message = "Cannot create a camera in a read-only scene";
            return reply;
        }
        const auto& camera = create_native_camera_view(*scene, request.camera_create.name.value_or(""), false);
        reply.camera = camera_properties_snapshot(*scene, camera);
        return reply;
    }
    if (request.method == SceneMethod::DeleteCamera) {
        SceneReply reply;
        const auto& scene_name = request.camera_delete.scene;
        const auto& camera_id = request.camera_delete.camera_id;
        if (scene_name.empty() || camera_id.empty() || !request.archive_session || request.archive_session->empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Camera mutation requires the observed archive, scene and camera identity";
            return reply;
        }
        auto* scene = native_editor_state().scene.get();
        if (!scene || scene->archive_session != *request.archive_session ||
            !native_scene_request_matches(*scene, scene_name)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Camera mutation belongs to an inactive archive";
            return reply;
        }
        if (scene->read_only) {
            reply.error_code = "READ_ONLY";
            reply.error_message = "Cannot modify cameras in a read-only scene";
            return reply;
        }
        const auto camera_it = std::find_if(scene->cameras.begin(), scene->cameras.end(),
            [&](const NativeEditorCamera& camera) { return camera.camera_id == camera_id; });
        if (camera_it == scene->cameras.end() || !camera_it->engine_camera) {
            reply.error_code = "CAMERA_NOT_FOUND";
            reply.error_message = "Camera not found: " + camera_id;
            return reply;
        }

        if (scene->cameras.size() <= 1) {
            reply.error_code = "LAST_CAMERA";
            reply.error_message = "A scene must keep at least one camera";
            return reply;
        }
        if (!camera_it->deletable) {
            reply.error_code = "CAMERA_NOT_DELETABLE";
            reply.error_message = "The main camera cannot be deleted";
            return reply;
        }
        camera_it->camera_window_lifetime.retire();
        camera_it->engine_camera->set_surface(0);
        if (scene->engine_scene) scene->engine_scene->remove_camera(camera_it->engine_camera.get());
        const auto removed_index = static_cast<std::size_t>(std::distance(scene->cameras.begin(), camera_it));
        ++scene->tree_revision;
        scene->cameras.erase(camera_it);
        if (scene->active_camera_index >= scene->cameras.size()) {
            scene->active_camera_index = scene->cameras.empty() ? 0 : scene->cameras.size() - 1;
        } else if (removed_index < scene->active_camera_index) {
            --scene->active_camera_index;
        }
        reply.deleted_camera = {*request.archive_session, scene->route, camera_id};
        return reply;
    }
    if (request.method == SceneMethod::UpdateCamera) return apply_native_camera_properties(request);
    if (request.method == SceneMethod::ReadCameraRenderSettings || request.method == SceneMethod::SetCameraRenderSettings)
        return apply_native_camera_render_settings(request);
    if (request.method == SceneMethod::CaptureScreenshot) return capture_native_screenshot(request);
    if (request.method == SceneMethod::FocusActor) return apply_native_actor_focus(request);
    if (request.method == SceneMethod::GetEnvironment || request.method == SceneMethod::SetEnvironment)
        return apply_native_environment(request);
    if (request.method == SceneMethod::SetActorTransform) {
        auto reply = apply_native_actor_transform(request);
        auto* scene = native_editor_state().scene.get();
        auto* actor = scene && reply.error_code.empty() ? find_native_actor(*scene, reply.transform.actor_guid) : nullptr;
        if (network_snapshot_session_active() && actor && is_network_editor_seed_actor(*actor))
            broadcast_editor_actor_transform(reply.transform);
        return reply;
    }
    if (request.method == SceneMethod::UpdateActor) {
        auto reply = apply_native_actor_properties(request);
        auto* scene = native_editor_state().scene.get();
        if (scene && reply.error_code.empty()) {
            if (auto* actor = find_native_actor(*scene, reply.properties.actor_guid))
                broadcast_network_editor_actor_state(*scene, *actor);
        }
        return reply;
    }

    if (request.method == SceneMethod::DeleteActor) {
        SceneReply reply;
        const auto& patch = request.actor_delete;
        if (patch.scene.empty() || patch.actor.empty() || !request.archive_session || request.archive_session->empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Actor deletion requires the observed archive, scene and actor";
            return reply;
        }
        auto* scene = native_editor_state().scene.get();
        if (!scene || scene->archive_session != *request.archive_session ||
            !native_scene_request_matches(*scene, patch.scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Actor deletion belongs to an inactive archive";
            return reply;
        }
        if (scene->read_only) {
            reply.error_code = "READ_ONLY";
            reply.error_message = "Cannot delete an actor in a read-only scene";
            return reply;
        }
        auto actor = std::find_if(scene->actors.begin(), scene->actors.end(), [&](const NativeEditorActor& value) {
            return value.actor_guid == patch.actor;
        });
        if (actor == scene->actors.end()) {
            reply.error_code = "ACTOR_NOT_FOUND";
            reply.error_message = "Actor not found: " + patch.actor;
            return reply;
        }
        reply.actor_delete = erase_native_editor_actor(*scene, actor);
        if (network_snapshot_session_active()) broadcast_editor_actor_delete(reply.actor_delete);
        return reply;
    }
    if (request.method == SceneMethod::RebindActorResource) {
        SceneReply reply;
        const auto& patch = request.actor_resource_rebind;
        if (patch.scene.empty() || patch.actor_guid.empty() || patch.path.empty() ||
            !request.archive_session || request.archive_session->empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Actor resource rebinding requires the observed archive, scene, actor and path";
            return reply;
        }
        auto* scene = native_editor_state().scene.get();
        if (!scene || scene->archive_session != *request.archive_session ||
            !native_scene_request_matches(*scene, patch.scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Actor resource rebinding belongs to an inactive archive";
            return reply;
        }
        auto& value = reply.actor_resource_rebind;
        value.archive_session = *request.archive_session;
        value.scene = scene->route;
        value.actor_guid = patch.actor_guid;
        const auto diagnostic = [&](std::string code, std::string message, const std::filesystem::path& path) {
            value.status = "resource_error";
            value.diagnostics.push_back({std::move(code), std::move(message), path_to_utf8(path), true});
            return reply;
        };
        auto source_path = path_from_utf8(patch.path);
        if (source_path.is_relative()) source_path = scene->project_root / source_path;
        if (!std::filesystem::is_regular_file(source_path))
            return diagnostic("RESOURCE_NOT_FOUND", "Replacement resource does not exist", source_path);
        auto* actor = find_native_actor_by_guid(*scene, patch.actor_guid);
        if (!actor) {
            reply.error_code = "ACTOR_NOT_FOUND";
            reply.error_message = "Actor GUID not found: " + patch.actor_guid;
            return reply;
        }
        if (!snapshot_actor_resource_supported(*actor, source_path))
            return diagnostic("UNSUPPORTED_RESOURCE_TYPE", "Replacement resource type is unsupported", source_path);
        if (scene->read_only) {
            reply.error_code = "READ_ONLY";
            reply.error_message = "External scenes are read-only";
            return reply;
        }
        auto resolved_path = std::filesystem::weakly_canonical(source_path);
        std::string route;
        std::error_code ec;
        const auto relative = std::filesystem::relative(resolved_path, scene->project_root, ec);
        if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0) {
            SceneAssetStore store(scene->project_root);
            auto imported = store.import_model(resolved_path);
            if (!imported.ok())
                return diagnostic("RESOURCE_IMPORT_FAILED", "Replacement resource could not be imported", resolved_path);
            route = imported.main_route;
            resolved_path = resolve_project_path(scene->project_root, route);
        } else {
            route = path_to_utf8(relative);
        }
        const auto actor_index = static_cast<std::size_t>(actor - scene->actors.data());
        const auto preserved_position = actor->geometry ? actor->geometry->get_position() : actor->position;
        const auto preserved_rotation = actor->geometry ? actor->geometry->get_rotation() : actor->rotation;
        const auto preserved_scale = actor->geometry ? actor->geometry->get_scale() : actor->scale;
        const auto preserved_body_type = actor->mechanics ? actor->mechanics->get_state().body_type : actor->persisted_body_type;
        auto replacement_data = actor->persisted_snapshot;
        replacement_data["actor_guid"] = patch.actor_guid;
        replacement_data["name"] = actor->name;
        replacement_data["route"] = route;
        replacement_data["asset_path"] = path_to_utf8(resolved_path);
        replacement_data["transform"] = {
            {"position", preserved_position},
            {"rotation", preserved_rotation},
            {"scale", preserved_scale},
        };
        try {
            auto replacement = native_actor_from_snapshot(replacement_data);
            auto& loaded = add_native_actor_to_scene(*scene, std::move(replacement), resolved_path);
            loaded.position = preserved_position;
            loaded.rotation = preserved_rotation;
            loaded.scale = preserved_scale;
            if (loaded.geometry) {
                loaded.geometry->set_transform(preserved_position, preserved_rotation, preserved_scale);
            }
            loaded.load_status = ActorLoadStatus::Loaded;
            loaded.load_error_code.clear();
            loaded.load_error_message.clear();
            if (loaded.optics) loaded.optics->set_properties({.visible = loaded.persisted_visible});
            if (loaded.mechanics && loaded.actor_type != "ui_image") loaded.mechanics->set_properties({.body_type = preserved_body_type});
            if (scene->actors[actor_index].engine_actor)
                scene->engine_scene->remove_actor(scene->actors[actor_index].engine_actor.get());
            scene->actors.erase(scene->actors.begin() + static_cast<std::ptrdiff_t>(actor_index));
            for (auto it = scene->load_diagnostics.begin(); it != scene->load_diagnostics.end();) {
                const bool resolved_main_resource =
                    it->value("actor_guid", std::string{}) == patch.actor_guid &&
                    it->value("code", std::string{}) != "ATTACHMENT_RESOURCE_NOT_FOUND";
                if (resolved_main_resource) it = scene->load_diagnostics.erase(it);
                else ++it;
            }
            auto* rebound = find_native_actor_by_guid(*scene, patch.actor_guid);
            if (!rebound) {
                reply.error_code = "NATIVE_FAILURE";
                reply.error_message = "Rebound actor could not be resolved";
                return reply;
            }
            ++scene->tree_revision;
            value.ok = true;
            value.status = "loaded";
            SceneRequest read{SceneMethod::ReadActorProperties};
            read.archive_session = request.archive_session;
            read.read_scene = scene->route;
            read.read_actor = rebound->name;
            const auto actor_reply = execute_scene_operation(read);
            if (!actor_reply.error_code.empty()) {
                reply.error_code = actor_reply.error_code;
                reply.error_message = actor_reply.error_message;
                return reply;
            }
            value.actor = actor_reply.actor_read;
        } catch (const std::exception& error) {
            return diagnostic("MODEL_DECODE_FAILED", error.what(), resolved_path);
        }
        return reply;
    }
    if (request.method == SceneMethod::ReadModelNodes || request.method == SceneMethod::SetModelNodeState) return apply_native_model_node(request);
    SceneReply reply;
    auto& state = native_editor_state();
    if (request.method == SceneMethod::ReadActive) {
        // A read must not lazily create a scene or invoke a storage adapter.
        if (const auto* scene = state.scene.get()) {
            reply.active = {scene->archive_session, scene->route, scene->name,
                state.project_path, scene->read_only, scene->tree_revision, scene->selection};
        }
        return reply;
    }
    if (request.method == SceneMethod::Save) {
        if (!request.archive_session || request.archive_session->empty() || request.read_scene.empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Scene save requires the observed archive and scene";
            return reply;
        }
        auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, request.read_scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Scene save belongs to an inactive scene";
            return reply;
        }
        auto& saved = reply.save;
        saved.archive_session = scene->archive_session;
        saved.scene = scene->route;
        if (scene->read_only) {
            saved.status = "read_only";
            saved.message = "External geometry scenes cannot be saved";
            return reply;
        }
        const auto result = save_editor_scene(*scene, request.save_actors_only);
        if (!result.success || !result.data.value("ok", false)) {
            reply.error_code = result.success ? "SAVE_FAILED" : "SAVE_FAILED";
            reply.error_message = result.error.empty()
                ? result.data.value("message", std::string{"Scene save failed"})
                : result.error;
            return reply;
        }
        saved.status = "saved";
        saved.filepath = result.data.value("filepath", std::string{});
        saved.unresolved_actor_count = result.data.value("unresolved_actor_count", std::size_t{0});
        return reply;
    }
    if (request.method == SceneMethod::SelectModelFile) {
        if (!request.archive_session || request.archive_session->empty() || request.read_scene.empty() || request.file_type.empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Model file selection requires scene scope and file type";
            return reply;
        }
        auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, request.read_scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Model file selection belongs to an inactive scene";
            return reply;
        }
        if (!resource_dialog_filters(request.file_type)) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Unsupported resource type: " + request.file_type;
            return reply;
        }
        const auto selected = select_resource_file_native(scene->project_root, request.file_type);
        if (selected) {
            reply.selected_file = route_for_project_storage(
                scene->project_root, path_to_utf8(absolute_normalized_path(*selected)));
        }
        return reply;
    }
    if (request.method == SceneMethod::ImportResourceFile) return import_native_resource(request);
    if (request.method == SceneMethod::PlayAudioResource || request.method == SceneMethod::StopAudioResource) {
        const auto error = change_native_resource_audio(request.audio.resource_id,
            request.method == SceneMethod::PlayAudioResource, request.audio.loop);
        if (!error.empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = error;
            return reply;
        }
        reply.audio.status = "success";
        reply.audio.resource_id = request.audio.resource_id;
        reply.audio.playing = request.method == SceneMethod::PlayAudioResource;
        return reply;
    }
    if (request.method == SceneMethod::PlayActorAudio || request.method == SceneMethod::StopActorAudio) {
        if (!request.archive_session || request.archive_session->empty() || request.audio.scene.empty() || request.audio.actor.empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Actor audio requires archive, scene and actor scope";
            return reply;
        }
        auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, request.audio.scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Actor audio belongs to an inactive scene";
            return reply;
        }
        auto* actor = find_native_actor_by_guid(*scene, request.audio.actor);
        if (!actor) actor = find_native_actor(*scene, request.audio.actor);
        if (!actor || !actor->acoustics) {
            reply.error_code = "ACTOR_AUDIO_UNAVAILABLE";
            reply.error_message = "Actor not found or has no acoustics component";
            return reply;
        }
        change_native_actor_audio(*actor, request.method == SceneMethod::PlayActorAudio, request.audio.loop);
        reply.audio.status = "success";
        reply.audio.archive_session = scene->archive_session;
        reply.audio.scene = scene->route;
        reply.audio.actor = actor->actor_guid;
        reply.audio.spatial = true;
        reply.audio.playing = request.method == SceneMethod::PlayActorAudio;
        return reply;
    }
    if (request.method == SceneMethod::ReadScene) {
        if (!request.archive_session || request.archive_session->empty() || trim_ascii(request.read_scene).empty() ||
            (request.scene_read.actors == ActorProjection::None && !request.scene_read.cameras)) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Scene query requires archive, scene and a projection";
            return reply;
        }
        const auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, request.read_scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Scene query belongs to an inactive scene";
            return reply;
        }
        reply.active = {scene->archive_session, scene->route, scene->name,
            state.project_path, scene->read_only, scene->tree_revision, scene->selection};
        reply.scene_read = request.scene_read;
        if (request.scene_read.cameras && !camera_list_snapshot(*scene, reply.camera_list)) {
            reply.error_code = "CAMERA_NOT_FOUND";
            reply.error_message = "Camera runtime is unavailable";
            return reply;
        }
        if (request.scene_read.actors == ActorProjection::Index) {
            reply.actor_index = {scene->archive_session, scene->route, scene->tree_revision, {}};
            std::vector<SceneProjectionService::ActorIdentity> identities;
            identities.reserve(scene->actors.size());
            // Identity only. Never acquire geometry or compute bounds for this projection.
            for (const auto& actor : scene->actors) {
                identities.push_back({actor.actor_guid, actor.name, actor_file_extension(actor.route), actor.actor_type});
            }
            reply.actor_index.actors = SceneProjectionService::project_index(identities);
        } else if (request.scene_read.actors == ActorProjection::Tree) {
            reply.tree.actors.reserve(scene->actors.size());
            for (const auto& actor : scene->actors) reply.tree.actors.push_back(scene_tree_actor_row(actor));
        } else if (request.scene_read.actors == ActorProjection::Context) {
            auto& result = reply.actor_context;
            result.archive_session = scene->archive_session;
            result.scene = scene->route;
            result.tree_revision = scene->tree_revision;
            result.actors.reserve(scene->actors.size());
            for (const auto& actor : scene->actors) {
                ActorContextRow row;
                row.actor_guid = actor.actor_guid;
                row.name = actor.name;
                row.type = SceneProjectionService::display_type(
                    {actor.actor_guid, actor.name, actor_file_extension(actor.route), actor.actor_type});
                row.semantic_role = SceneProjectionService::semantic_role(actor.semantic_role, actor.entity_type);
                row.position = actor.geometry ? actor.geometry->get_position() : actor.position;
                row.rotation = actor.geometry ? actor.geometry->get_rotation() : actor.rotation;
                row.scale = actor.geometry ? actor.geometry->get_scale() : actor.scale;
                if (const auto bounds = native_actor_world_aabb(actor)) {
                    row.size = {(*bounds)[3] - (*bounds)[0], (*bounds)[4] - (*bounds)[1], (*bounds)[5] - (*bounds)[2]};
                    row.has_size = true;
                }
                const auto body_type = actor.mechanics ? body_type_name(*actor.mechanics) : actor.persisted_body_type;
                row.physics_enabled = SceneProjectionService::physics_enabled(body_type);
                result.actors.push_back(std::move(row));
            }
        }
        return reply;
    }
    if (request.method == SceneMethod::ReadActorProperties) {
        if (!request.archive_session || request.archive_session->empty() ||
            trim_ascii(request.read_scene).empty() || trim_ascii(request.read_actor).empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "Actor properties require the observed archive, scene and Actor";
            return reply;
        }
        auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, request.read_scene)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Actor properties belong to an inactive scene";
            return reply;
        }
        const auto* actor = find_native_actor(*scene, request.read_actor);
        if (!actor) {
            reply.error_code = "ACTOR_NOT_FOUND";
            reply.error_message = "Actor not found: " + request.read_actor;
            return reply;
        }
        auto& value = reply.actor_read;
        value.properties = actor_properties_snapshot(*scene, *actor);
        value.archive_session = scene->archive_session;
        value.actor_type = actor->actor_type; value.model = actor->route;
        value.body_type = actor->load_status == ActorLoadStatus::Loaded && actor->mechanics
            ? actor->mechanics->get_state().body_type : actor->persisted_body_type;
        value.properties.visible = actor->load_status == ActorLoadStatus::Loaded && actor->optics
            ? actor->optics->get_state().visible : actor->persisted_visible;
        if (value.properties.mechanics) value.properties.mechanics->body_type = value.body_type;
        value.load_status = actor_load_status_name(actor->load_status);
        if (actor->load_status != ActorLoadStatus::Loaded) {
            value.load_error_code = actor->load_error_code; value.load_error_message = actor->load_error_message;
            value.resolved_asset_path = actor->resolved_asset_path;
        }
        value.handle = actor->engine_actor ? actor->engine_actor->get_handle() : 0;
        value.position = actor->geometry ? actor->geometry->get_position() : actor->position;
        value.rotation = actor->geometry ? actor->geometry->get_rotation() : actor->rotation;
        value.scale = actor->geometry ? actor->geometry->get_scale() : actor->scale;
        return reply;
    }
    if (request.method == SceneMethod::ReadActorRig ||
        request.method == SceneMethod::SetActorIkChains) {
        if (!request.archive_session || request.archive_session->empty() ||
            trim_ascii(request.ik.scene.empty() ? request.read_scene : request.ik.scene).empty() ||
            trim_ascii(request.ik.actor.empty() ? request.read_actor : request.ik.actor).empty()) {
            reply.error_code = "INVALID_ARGUMENT";
            reply.error_message = "IK operations require the observed archive, scene and Actor";
            return reply;
        }
        const std::string& scene_name = request.ik.scene.empty() ? request.read_scene : request.ik.scene;
        const std::string& actor_name = request.ik.actor.empty() ? request.read_actor : request.ik.actor;
        auto* scene = state.scene.get();
        if (!scene || *request.archive_session != scene->archive_session ||
            !native_scene_request_matches(*scene, scene_name)) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "IK operation belongs to an inactive scene";
            return reply;
        }
        auto* actor = find_native_actor(*scene, actor_name);
        if (!actor || !actor->geometry) {
            reply.error_code = "ACTOR_NOT_FOUND";
            reply.error_message = "Actor has no geometry: " + actor_name;
            return reply;
        }
        const auto model_id = actor->geometry->get_model_id();
        if (model_id == 0) {
            reply.error_code = "ACTOR_NOT_READY";
            reply.error_message = "IK model import is not ready: " + actor_name;
            return reply;
        }
        auto resource = read_resource_for_ui<Corona::Resource::Scene>(model_id);
        if (!resource.valid()) {
            reply.error_code = "ACTOR_NOT_READY";
            reply.error_message = "IK model resource is unavailable";
            return reply;
        }
        const auto* skeleton = resource->data.skeleton ? &*resource->data.skeleton : nullptr;
        if (request.method == SceneMethod::ReadActorRig) {
            auto& value = reply.actor_skeleton;
            value.archive_session = scene->archive_session; value.scene = scene->route; value.actor = actor->name;
            value.is_skinned = skeleton != nullptr;
            if (skeleton) {
                value.node_count = static_cast<int>(skeleton->nodes.size());
                value.bone_count = skeleton->bone_count;
                value.nodes.reserve(skeleton->nodes.size());
                for (std::size_t index = 0; index < skeleton->nodes.size(); ++index) {
                    const auto& node = skeleton->nodes[index];
                    value.nodes.push_back({node.name, static_cast<int>(index), node.parent, node.children.empty()});
                    if (node.children.empty()) value.leaves.push_back(node.name);
                }
            }

            auto geometry = Corona::SharedDataHub::instance().geometry_storage().try_acquire_read(actor->geometry->get_handle());
            if (!geometry) { reply.error_code = "ACTOR_NOT_READY"; reply.error_message = "IK geometry storage is unavailable"; return reply; }
            auto& ik = reply.actor_ik;
            ik.archive_session = scene->archive_session; ik.scene = scene->route; ik.actor = actor->name;
            ik.ready = true; ik.is_skinned = skeleton != nullptr; ik.revision = geometry->ik_chains_revision;
            ik.chains = typed_ik_chains(geometry->ik_chains, skeleton, true);
            return reply;
        }
        if (!skeleton) { reply.error_code = "ACTOR_NOT_READY"; reply.error_message = "Actor model has no skeleton"; return reply; }
        nlohmann::json encoded = nlohmann::json::array();
        for (const auto& chain : request.ik.chains) encoded.push_back(typed_ik_chain_json(chain));
        std::vector<Corona::Resource::IkChain> chains;
        try { chains = IkChainConfig::parse(encoded, skeleton); }
        catch (const std::exception& error) { reply.error_code = "INVALID_ARGUMENT"; reply.error_message = error.what(); return reply; }
        auto writable = Corona::SharedDataHub::instance().geometry_storage().try_acquire_write(actor->geometry->get_handle());
        if (!writable) { reply.error_code = "ACTOR_NOT_READY"; reply.error_message = "IK geometry storage is unavailable"; return reply; }
        if (request.ik.expected_revision && *request.ik.expected_revision != writable->ik_chains_revision) {
            reply.error_code = "CONFLICT"; reply.error_message = "IK configuration changed; reload before applying"; return reply;
        }
        IkChainConfig::preserve_runtime(chains, writable->ik_chains);
        auto saved_config = IkChainConfig::serialize(chains, skeleton);
        writable->ik_chains = std::move(chains); ++writable->ik_chains_revision;
        actor->persisted_ik_chains = std::move(saved_config);
        actor->actor_version = std::max(actor->actor_version + 1, 1); ++scene->tree_revision;
        reply.actor_ik = {scene->archive_session, scene->route, actor->name, true, true,
                          writable->ik_chains_revision, typed_ik_chains(writable->ik_chains, skeleton, true)};
        return reply;
    }
    if (request.method == SceneMethod::ReadTransforms) {
        auto* scene = state.scene.get();
        if (!scene || !request.archive_session || *request.archive_session != scene->archive_session) {
            reply.error_code = "PROJECT_CONTEXT_CHANGED";
            reply.error_message = "Transforms belong to an inactive scene";
            return reply;
        }
        reply.active.archive_session = scene->archive_session;
        reply.active.scene = scene->route;
        for (const auto& guid : request.actor_guids) {
            const auto* actor = find_native_actor_by_guid(*scene, guid);
            if (!actor) continue;
            ActorTransformSnapshot snapshot;
            snapshot.scene = scene->route;
            snapshot.actor = actor->name;
            snapshot.actor_guid = guid;
            snapshot.actor_version = actor->actor_version;
            snapshot.position = actor->geometry ? actor->geometry->get_position() : actor->position;
            snapshot.rotation = actor->geometry ? actor->geometry->get_rotation() : actor->rotation;
            snapshot.scale = actor->geometry ? actor->geometry->get_scale() : actor->scale;
            reply.transforms.push_back(std::move(snapshot));
        }
        return reply;
    }
    auto* scene = request.archive_session ? state.scene.get() : ensure_native_editor_scene();
    if (!scene || (request.archive_session && *request.archive_session != scene->archive_session) ||
        !native_scene_request_matches(*scene, request.selection.scene)) {
        reply.error_code = "PROJECT_CONTEXT_CHANGED";
        reply.error_message = "Selection does not identify the active scene";
        return reply;
    }
    reply.selection = request.selection;
    // The GUID is native authority; it is never accepted from the Web request.
    reply.selection.actor_guid.reset();
    if (const auto* actor = find_native_actor(*scene, request.selection.actor))
        reply.selection.actor_guid = actor->actor_guid;
    scene->selection = reply.selection;
    return reply;
}

WindowReply prepare_camera_window_request(WindowRequest& request) {
    request.camera_handle = 0;
    request.camera_authority.reset();
    request.camera_state.reset();
    WindowReply reply;
    const auto* scene = native_editor_state().scene.get();
    if (!scene || request.archive_session.empty() || request.scene_id.empty() ||
        request.archive_session != scene->archive_session ||
        !native_scene_request_matches(*scene, request.scene_id)) {
        reply.error_code = "PROJECT_CONTEXT_CHANGED";
        reply.error_message = "Camera window belongs to an inactive scene";
        return reply;
    }
    const auto camera = std::find_if(scene->cameras.begin(), scene->cameras.end(),
        [&](const NativeEditorCamera& camera) { return camera.camera_id == request.camera_id; });
    if (camera == scene->cameras.end() || !camera->engine_camera) {
        reply.error_code = "CAMERA_NOT_FOUND";
        reply.error_message = "Camera identity is no longer available";
        return reply;
    }
    request.scene_id = scene->route;
    request.camera_handle = camera->engine_camera->get_handle();
    request.camera_authority = camera->camera_window_lifetime.token();
    request.camera_state = camera->window_state;
    return reply;
}

ViewportPickReply finish_viewport_pick(const ViewportPickRequest& request, const Corona::ActorPickCompletion& result) {
    ViewportPickReply reply;
    auto camera = request.camera;
    const auto checked = prepare_camera_window_request(camera);
    reply.archive_session = camera.archive_session;
    reply.scene = camera.scene_id;
    reply.camera_id = camera.camera_id;
    reply.x = result.x; reply.y = result.y;
    if (!checked.error_code.empty() || camera.camera_handle != request.camera.camera_handle ||
        camera.camera_authority != request.camera.camera_authority) {
        reply.error_code = checked.error_code.empty() ? "PROJECT_CONTEXT_CHANGED" : checked.error_code;
        reply.error_message = "Pick belongs to a retired camera or archive";
        return reply;
    }
    if (result.status != "success" && result.status != "miss") {
        reply.error_code = result.status == "timeout" ? "PICK_TIMEOUT" :
            result.status == "permission_denied" ? "PERMISSION_DENIED" :
            result.status == "window_closed" ? "WINDOW_CLOSED" :
            result.status == "stale_scene" ? "PROJECT_CONTEXT_CHANGED" :
            result.status == "invalid_argument" ? "INVALID_ARGUMENT" : "PICK_FAILED";
        reply.error_message = "Viewport pick failed: " + result.status;
        return reply;
    }
    reply.status = "miss";
    if (result.status == "miss" || !result.actor_handle) return reply;
    auto& scene = *native_editor_state().scene;
    // Exact native handle lookup only: actor names/guids may look numeric.
    const auto actor = std::find_if(scene.actors.begin(), scene.actors.end(), [&](const auto& item) {
        return item.engine_actor && item.engine_actor->get_handle() == result.actor_handle;
    });
    if (actor == scene.actors.end()) return reply;
    reply.status = "success";
    reply.actor_handle = std::to_string(result.actor_handle);
    reply.actor_guid = actor->actor_guid;
    reply.actor_name = actor->name;
    reply.actor_type = actor->actor_type;
    if (result.node_index != UINT32_MAX) reply.node_index = result.node_index;
    return reply;
}

namespace {

Corona::Systems::UI::CollaborativeEditorApplyOutcome to_collaborative_outcome(
    PendingNetworkActorApplyOutcome outcome) {
    using PublicResult = Corona::Systems::UI::CollaborativeEditorApplyResult;
    const auto result = outcome.result == PendingNetworkActorApplyResult::Applied
        ? PublicResult::Applied
        : outcome.result == PendingNetworkActorApplyResult::Retry
            ? PublicResult::Retry
            : PublicResult::Discard;
    return {result, std::move(outcome.error), std::move(outcome.actor)};
}

/// 协作同步的 apply 从**引擎主线程**（`tick_collaborative_editor_runtime`）发起，但它改的
/// 是编辑器场景状态 —— 那份状态的唯一 owner 是 CEF UI 线程（见 `editor_dispatch.h`）。
/// 所以真正干活的闭包被投递到 owner 线程执行：
///   - owner 线程能在时限内接手 → 同步拿到真实结果；
///   - owner 线程没接手 → **撤销**，返回 nullopt，调用方按"下一帧再试"处理。
///
/// 撤销语义是这里的关键：拖到超时之后才执行的任务必须变成空操作，否则调用方会"报告
/// 没执行 → 下一帧重试"，同一个 actor 就被创建两次。反过来，如果 owner 线程已经开始
/// 执行，就一定要等它跑完拿回结果（见 `call_on_editor_owner_thread`）。
///
/// 代价：引擎主线程最多在这里花 `kCollaborativeApplyDispatchTimeout`；正常情况下投递
/// 一次就是几毫秒。
constexpr std::chrono::milliseconds kCollaborativeApplyDispatchTimeout{200};

std::optional<PendingNetworkActorApplyOutcome> run_collaborative_apply(
    const std::function<void(PendingNetworkActorApplyOutcome&)>& apply) {
    using namespace Corona::Systems::UI::Detail;
    // 注意顺序：`on_editor_owner_thread()` 会记录"见过 owner 线程"，必须先于
    // `editor_owner_thread_seen()` 求值（函数实参的求值顺序是不指定的）。
    const bool on_owner_thread = on_editor_owner_thread();
    const auto route = decide_editor_dispatch_route(
        on_owner_thread,
        editor_thread_waiting_for_python(),
        editor_owner_thread_seen());

    EditorDispatchReport report;
    auto outcome = call_editor_dispatch(
        [&apply] {
            PendingNetworkActorApplyOutcome result =
                pending_actor_outcome(PendingNetworkActorApplyResult::Discard);
            apply(result);
            return result;
        },
        route,
        kCollaborativeApplyDispatchTimeout,
        &report);
    if (outcome) {
        return outcome;
    }

    // 编辑器线程没接手：本帧跳过，下一帧再来。按 1/100 节流，避免卡顿时刷屏。
    static std::atomic<std::uint64_t> busy_count{0};
    const auto count = busy_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || count % 100 == 0) {
        CFW_LOG_WARNING(
            "CollaborativeEditorRuntime: the editor thread did not take the pending actor "
            "apply within {}ms ({} times); the mutation stays queued for the next frame",
            static_cast<long long>(kCollaborativeApplyDispatchTimeout.count()), count);
    }
    return std::nullopt;
}

}  // namespace

std::shared_ptr<Corona::Systems::NetworkSystem> collaborative_editor_network_system() {
    return get_network_system();
}

CollaborativeEditorApplyOutcome apply_collaborative_actor_create(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& model_path,
    nlohmann::json actor_data) {
    auto outcome = run_collaborative_apply([&](PendingNetworkActorApplyOutcome& result) {
        result = apply_pending_network_actor_create(
            actor_guid, scene_name, model_path, std::move(actor_data));
    });
    if (!outcome) {
        return {Corona::Systems::UI::CollaborativeEditorApplyResult::Retry,
                "the editor thread is busy",
                nlohmann::json::object()};
    }
    return to_collaborative_outcome(std::move(*outcome));
}

CollaborativeEditorApplyOutcome apply_collaborative_actor_state(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& actor_data) {
    auto outcome = run_collaborative_apply([&](PendingNetworkActorApplyOutcome& result) {
        result = apply_pending_network_actor_state(actor_guid, scene_name, actor_data);
    });
    if (!outcome) {
        return {Corona::Systems::UI::CollaborativeEditorApplyResult::Retry,
                "the editor thread is busy",
                nlohmann::json::object()};
    }
    return to_collaborative_outcome(std::move(*outcome));
}

CollaborativeEditorApplyOutcome apply_collaborative_actor_transform(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& transform_data) {
    auto outcome = run_collaborative_apply([&](PendingNetworkActorApplyOutcome& result) {
        result = apply_pending_network_actor_transform(actor_guid, scene_name, transform_data);
    });
    if (!outcome) {
        return {Corona::Systems::UI::CollaborativeEditorApplyResult::Retry,
                "the editor thread is busy",
                nlohmann::json::object()};
    }
    return to_collaborative_outcome(std::move(*outcome));
}

CollaborativeEditorApplyOutcome apply_collaborative_actor_delete(
    const std::string& actor_guid,
    const std::string& scene_name) {
    auto outcome = run_collaborative_apply([&](PendingNetworkActorApplyOutcome& result) {
        result = apply_pending_network_actor_delete(actor_guid, scene_name);
    });
    if (!outcome) {
        return {Corona::Systems::UI::CollaborativeEditorApplyResult::Retry,
                "the editor thread is busy",
                nlohmann::json::object()};
    }
    return to_collaborative_outcome(std::move(*outcome));
}

void load_native_editor_scene_for_test(const nlohmann::json& snapshot) {
    nlohmann::json diagnostics = nlohmann::json::array();
    materialize_scene_snapshot_into_state(
        native_editor_state(), snapshot, false, diagnostics);
}

SceneReply import_native_resource_for_test(const SceneRequest& request,
    const ResourceFilePicker& picker, const MediaFileImporter& importer) {
    return import_native_resource(request, picker, importer);
}

void reset_native_editor_scene_for_test() {
    auto& state = native_editor_state();
    state.scene.reset();
    state.project_path.clear();
}

std::string native_editor_scene_snapshot_json(const std::string& scene_name) {
    try {
        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);
        nlohmann::json actors = nlohmann::json::array();
        // Engine reads here are best effort on the CEF UI thread (the default of the
        // helpers below), so a slow resource or a held lock degrades the snapshot
        // instead of freezing the editor.
        for (const auto& actor : scene->actors) {
            actors.push_back(actor_to_json(*scene, actor));
        }
        const auto scene_aabb = native_scene_world_aabb(*scene);
        nlohmann::json cameras = nlohmann::json::array();
        for (const auto& camera : scene->cameras) {
            auto snapshot = camera_to_json(camera);
            snapshot["vision_stable_planes"] = camera.engine_camera
                                                   ? camera.engine_camera->get_requested_vision_stable_planes()
                                                   : true;
            cameras.push_back(std::move(snapshot));
        }
        const auto active_index = scene->cameras.empty()
                                      ? 0
                                      : std::min(scene->active_camera_index, scene->cameras.size() - 1);
        const auto active_camera = scene->cameras.empty()
                                       ? nlohmann::json(nullptr)
                                       : cameras[active_index];
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"scene_name", scene->name},
            {"archive_session", scene->archive_session},
            {"project_path", path_to_utf8(scene->project_root)},
            {"actor_count", scene->actors.size()},
            {"actors", actors},
            {"active_camera_id", active_camera.is_null() ? "" : active_camera.value("camera_id", "")},
            {"active_camera_name", active_camera.is_null() ? "" : active_camera.value("name", "")},
            {"camera", active_camera},
            {"cameras", cameras},

            {"scene_aabb", scene_aabb ? aabb_to_json(*scene_aabb) : nlohmann::json(nullptr)},
            {"bounds_ready", static_cast<bool>(scene_aabb)},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "get_editor_scene_snapshot native handler error"},
        }.dump();
    }
}



std::optional<std::filesystem::path> resolve_file_manager_path(
    const std::filesystem::path& project_root, const std::string& raw_path,
    bool allow_root = false) {
    const auto route = normalize_route(raw_path);
    if (route.empty()) {
        return allow_root ? std::optional{project_root} : std::nullopt;
    }
    const auto candidate = resolve_project_path(project_root, route);
    const auto normalized_candidate = absolute_normalized_path(candidate);
    if (!is_path_within(project_root, normalized_candidate) ||
        (!allow_root && normalized_candidate == absolute_normalized_path(project_root))) {
        return std::nullopt;
    }
    return normalized_candidate;
}

namespace {

NativeResult open_project_file(const std::filesystem::path& root, const std::string& path, const std::string& type) {
    const auto target = resolve_file_manager_path(root, path);
    if (!target || !std::filesystem::is_regular_file(*target)) return native_failure("File does not exist", 404);
    // Opening a scene is the same native reload operation for both adapters.
    if (type == "scene") reload_native_editor_scene("", route_for_project_storage(root, path_to_utf8(*target)));
    return native_success(true);
}
}

FilesReply execute_files_operation(const FilesRequest& request) {
    FilesReply reply;
    const auto* scene = native_editor_state().scene.get();
    if (!scene || scene->archive_session != request.archive_session ||
        absolute_normalized_path(path_from_utf8(request.project_path)) != scene->project_root) {
        reply.error_code = "PROJECT_CONTEXT_CHANGED"; reply.error_message = "Project file request belongs to a retired archive"; return reply;
    }
    if (scene->read_only) { reply.error_code = "READ_ONLY_PROJECT"; reply.error_message = "Project is read-only"; return reply; }
    reply.archive_session = request.archive_session;
    reply.project_path = path_to_utf8(scene->project_root);
    const auto route = std::filesystem::weakly_canonical(scene->project_root / path_from_utf8(request.path));
    if (!is_path_within(scene->project_root, route)) {
        reply.error_code = "INVALID_PROJECT_PATH"; reply.error_message = "Path outside current project"; return reply;
    }
    const auto result = open_project_file(scene->project_root, request.path, request.file_type);
    if (!result.success) {
        reply.error_code = result.error_code == 409 ? "PROJECT_CONTEXT_CHANGED" : result.error_code == 404 ? "FILE_NOT_FOUND" :
            result.error_code == 504 ? "EDITOR_API_TIMEOUT" : "FILE_OPERATION_FAILED";
        reply.error_message = result.error; return reply;
    }
    reply.ok = true;
    return reply;
}


NativeResult create_launcher_project(const std::string& prompt, const std::string& role) {
    const bool multiplayer = !role.empty();
    const std::string display_base = !multiplayer ? "创造世界" : role == "host" ? "联机房主" : "联机加入";
    const std::string dir_base = !multiplayer ? "creative_world" : role == "host" ? "multiplayer_host" : "multiplayer_guest";
    const auto base_dir = runtime_data_dir();
    std::filesystem::create_directories(base_dir);
    int index = 1;
    std::filesystem::path target;
    do {
        target = base_dir / (dir_base + "_" + std::to_string(index++));
    } while (std::filesystem::exists(target));
    const auto display_name = display_base + "_" + std::to_string(index - 1);
    nlohmann::json payload{{"path", path_to_utf8(target)}, {"name", display_name}};
    if (multiplayer) payload["multiplayer"] = {{"role", role}};
    else payload["world"] = {{"type", "creative"}, {"prompt", prompt}};
    auto created = project_archive_call("create_project", payload);
    if (!created.success || !created.data.contains("path")) return native_failure("Project creation failed: " + created.error, 2);
    update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}});
    nlohmann::json result{{"name", display_name}, {"path", path_to_utf8(target)}};
    if (multiplayer) result["role"] = role;
    return native_success(std::move(result));
}

NativeResult open_launcher_project(const std::string& requested_path, const std::string& load_policy) {
    const auto path = normalize_route(requested_path);
    CFW_LOG_INFO("[ProjectLauncher] open_project request path='{}'", path);
    if (path.empty()) {
        return native_success({{"ok", false}, {"path", std::string{}}, {"status", "cancelled"}});
    }
    auto prepared = prepare_archive_load(path, load_policy);
    if (!prepared.service_ok) {
        if (!python_script_service_dispatcher_registered()) {
            return native_success({
                {"ok", false},
                {"status", "service_initializing"},
                {"path", path},
                {"message", "Archive service is initializing"},
            });
        }
        return native_success({
            {"ok", false},
            {"status", "archive_service_error"},
            {"path", path},
            {"message", prepared.error.empty() ? "Python archive parser failed"
                                               : prepared.error},
        });
    }
    if (!prepared.ready) {
        return native_success({
            {"ok", false}, {"status", prepared.status}, {"path", path},
            {"diagnostics", prepared.diagnostics},
        });
    }

    auto& state = native_editor_state();
    // A file run can start between the Web launcher's cleanup and this open.
    // Fence the actual replacement on the editor thread with the old scene still installed.
    if (auto error = stop_before_scene_replacement(state)) {
        return native_success({{"ok", false}, {"status", "script_cleanup_failed"}, {"path", path},
            {"message", *error}});
    }
    try {
        materialize_scene_snapshot_into_state(
            state,
            prepared.snapshot,
            load_policy == "degraded",
            prepared.diagnostics);
    } catch (const std::exception& error) {
        prepared.diagnostics.push_back({
            {"severity", "error"}, {"recoverable", false},
            {"stage", "scene_commit"}, {"code", "SCENE_COMMIT_FAILED"},
            {"message", error.what()}, {"path", path},
        });
        return native_success({
            {"ok", false}, {"status", "invalid_archive"}, {"path", path},
            {"diagnostics", prepared.diagnostics},
        });
    }
    if (!state.scene->read_only) {
        add_recent_project_native(state.scene->project_root);
    }
    std::size_t unresolved_count = 0;
    for (const auto& actor : state.scene->actors) {
        if (actor.load_status != ActorLoadStatus::Loaded) ++unresolved_count;
    }
    CFW_LOG_INFO("[ProjectLauncher] open_project opened path='{}' status='{}'",
                 state.project_path, prepared.status);
    ++state.scene->tree_revision;
    return native_success({
        {"ok", true}, {"status", prepared.status}, {"path", state.project_path},
        {"degraded", prepared.status == "opened_degraded" || unresolved_count > 0},
        {"unresolved_actor_count", unresolved_count},
        {"diagnostics", prepared.diagnostics},
        {"scene", {{"path", state.scene->route}, {"name", state.scene->name}}},
    });
}

ProjectLauncherReply execute_project_launcher_operation(const ProjectLauncherRequest& request) {
    ProjectLauncherReply reply;
    reply.method = request.method;
    if (request.method == ProjectLauncherMethod::Open) {
        const auto result = open_launcher_project(request.path, request.load_policy);
        if (!result.success) {
            reply.error_code = "PROJECT_OPEN_FAILED"; reply.error_message = result.error;
            return reply;
        }
        const auto& value = result.data;
        auto& opened = reply.opened;
        opened.ok = value.at("ok").get<bool>();
        opened.status = value.at("status").get<std::string>();
        opened.path = value.at("path").get<std::string>();
        if (value.contains("message")) opened.message = value.at("message").get<std::string>();
        if (value.contains("degraded")) opened.degraded = value.at("degraded").get<bool>();
        if (value.contains("unresolved_actor_count")) opened.unresolved_actor_count = value.at("unresolved_actor_count").get<std::uint64_t>();
        if (value.contains("scene")) opened.scene = SelectedProjectSnapshot{
            value.at("scene").at("name").get<std::string>(), value.at("scene").at("path").get<std::string>()};
        if (value.contains("diagnostics")) {
            opened.diagnostics.emplace();
            for (const auto& item : value.at("diagnostics")) {
                ProjectOpenDiagnostic diagnostic;
                if (item.contains("severity")) diagnostic.severity = item.at("severity").get<std::string>();
                if (item.contains("stage")) diagnostic.stage = item.at("stage").get<std::string>();
                if (item.contains("code")) diagnostic.code = item.at("code").get<std::string>();
                if (item.contains("message")) diagnostic.message = item.at("message").get<std::string>();
                if (item.contains("path")) diagnostic.path = item.at("path").get<std::string>();
                if (item.contains("actor_name")) diagnostic.actor_name = item.at("actor_name").get<std::string>();
                if (item.contains("actor_guid")) diagnostic.actor_guid = item.at("actor_guid").get<std::string>();
                if (item.contains("shape_type")) diagnostic.shape_type = item.at("shape_type").get<std::string>();
                if (item.contains("resource_path")) diagnostic.resource_path = item.at("resource_path").get<std::string>();
                if (item.contains("field")) diagnostic.field = item.at("field").get<std::string>();
                if (item.contains("camera_id")) diagnostic.camera_id = item.at("camera_id").get<std::string>();
                if (item.contains("recoverable")) diagnostic.recoverable = item.at("recoverable").get<bool>();
                if (item.contains("requires_confirmation")) diagnostic.requires_confirmation = item.at("requires_confirmation").get<bool>();
                if (item.contains("count")) diagnostic.count = item.at("count").get<std::uint64_t>();
                opened.diagnostics->push_back(std::move(diagnostic));
            }
        }
        return reply;
    }
    if (request.method == ProjectLauncherMethod::SelectFile) {
        if (const auto selected = open_project_file_native())
            reply.selected = {path_to_utf8(selected->stem()), path_to_utf8(*selected)};
        return reply;
    }
    if (request.method == ProjectLauncherMethod::CreateWorld || request.method == ProjectLauncherMethod::CreateMultiplayer) {
        const auto result = create_launcher_project(request.prompt,
            request.method == ProjectLauncherMethod::CreateWorld ? "" : request.role);
        if (!result.success) {
            reply.error_code = "PROJECT_CREATION_FAILED"; reply.error_message = result.error;
        } else {
            reply.created = {result.data.value("name", std::string{}), result.data.value("path", std::string{}),
                result.data.value("role", std::string{})};
        }
        return reply;
    }
    if (request.method == ProjectLauncherMethod::GetVersion) {
        reply.version = settings_value("General", "version", "1.2.0");
    } else if (request.method == ProjectLauncherMethod::GetRecent) {
        for (const auto& value : recent_projects_native()) {
            reply.recent.push_back({value.value("name", std::string{}), value.value("path", std::string{}),
                value.value("last_edited", std::string{}), value.value("if_exists", false)});
        }
    } else {
        const auto value = project_resource_load_status();
        auto& load = reply.load;
        load.active = value.value("active", false);
        load.loading = value.value("loading", false);
        load.archive_service_ready = value.value("archive_service_ready", false);
        load.path = value.value("path", std::string{});
        load.scene = value.value("scene", std::string{});
        const auto* scene = native_editor_state().scene.get();
        load.archive_session = scene ? scene->archive_session : "";
        load.total = value.value("total", std::uint64_t{});
        load.ready = value.value("ready", std::uint64_t{});
        load.failed = value.value("failed", std::uint64_t{});
        load.pending = value.value("pending", std::uint64_t{});
        load.progress = value.value("progress", 0.0);
    }
    return reply;
}




namespace {

NativeResult save_project_info(const nlohmann::json& settings) {
    auto* scene = native_editor_state().scene.get();
    if (!scene) return native_failure("PROJECT_CONTEXT_CHANGED", 409);
    if (const auto invalid = ProjectScriptService::validate_document_identity(
            scene->archive_session, settings.value("archive_session", std::string{}), true)) {
        return native_failure(invalid.message, 409);
    }
    if (const auto invalid = ProjectScriptService::require_writable_project(scene->read_only)) {
        return native_failure(invalid.message, 403);
    }
    auto result = project_archive_call("update_metadata", {{"path", path_to_utf8(scene->project_root)}, {"settings", settings}});
    if (!result.success || !result.data.contains("name")) return native_failure("Project settings were not saved: " + result.error, 2);
    scene->name = result.data.at("name");
    scene->core_version = result.data.at("core_version");
    result.data["archive_session"] = scene->archive_session;
    return result;
}
}

ProjectSettingsReply execute_project_settings_operation(const ProjectSettingsRequest& request) {
    NativeResult result;
    ProjectSettingsReply reply;
    const auto* scene = native_editor_state().scene.get();
    if (!scene || absolute_normalized_path(path_from_utf8(request.project_path)) != scene->project_root)
        result = native_failure("PROJECT_CONTEXT_CHANGED", 409);
    else result = save_project_info({{"archive_session", request.archive_session},
        {"name", request.name}, {"core_version", request.core_version}});
    if (!result.success) {
        reply.error_code = ProjectScriptService::project_settings_error_code(result.error_code);
        reply.error_message = result.error;
        return reply;
    }
    const auto& data = result.data;
    ProjectInfoSnapshot info;
    info.name = data.value("name", std::string{});
    info.mode = data.value("mode", std::string{});
    info.prompt = data.value("prompt", std::string{});
    info.core_version = data.value("core_version", std::string{});
    info.create_time = data.value("create_time", std::string{});
    info.last_opened = data.value("last_opened", std::string{});
    info.project_path = data.at("project_path").get<std::string>();
    info.archive_session = data.at("archive_session").get<std::string>();
    info.read_only = data.value("read_only", false);
    info.source_path = data.value("source_path", std::string{});
    reply.info = std::move(info);
    return reply;
}


// The engine main thread polls animation revisions, but editor state belongs to
// the CEF UI thread. Use the same ownership handoff as API/collaborative mutations;
// a cancelled dispatch leaves revisions untouched for the next frame to retry.
void tick_native_animation_updates() {
    using namespace Detail;
    const bool on_owner_thread = on_editor_owner_thread();
    const auto route = decide_editor_dispatch_route(
        on_owner_thread, editor_thread_waiting_for_python(), editor_owner_thread_seen());
    (void)call_editor_dispatch([] {
        auto* scene = native_editor_state().scene.get();
        if (!scene) return false;
        for (auto& actor : scene->actors) {
            auto player = actor_animation_player(actor);
            if (!player || actor.animation_revision == player->revision()) continue;
            const auto state = player->snapshot();
            actor.animation_revision = state.revision;
            if (state.load_status == "ready" && actor.persisted_animation_sources != state.sources) {
                actor.persisted_animation_sources = state.sources;
                actor.actor_version = std::max(actor.actor_version + 1, 1);
            }
            ++scene->tree_revision;
        }
        return true;
    }, route, kCollaborativeApplyDispatchTimeout);
}

// The six vision toggles are the one render-settings family that differs only in which camera
// accessor it calls: identical arguments, identical response keys. One pair of entry points and
// one spec table replace six copies of the same eighteen lines. `echo_in_snapshot` records the
// one behavioural difference between them — only the denoiser echoes its value back inside the
// camera snapshot — so the consolidation keeps the observable shapes exactly as they were.
struct VisionToggleSpec {
    bool (Corona::API::Camera::*get)() const;
    void (Corona::API::Camera::*set)(bool);
    bool echo_in_snapshot;
};



// Named entry points for the ScriptRuntime caller. The table below still forwards to them
// while the JSON manifest is retired; once it is gone these functions are the whole
// contract and the table, the registry and the api-name strings disappear with it.

NativeResult script_scene_get_environment(const NativeRequest& request) {
            SceneRequest read{SceneMethod::GetEnvironment};
            read.environment.scene = arg_string(request.args, 0);
            return native_environment_result(read);
}

NativeResult script_scene_read_active_scene(const NativeRequest& request) {
            const auto state = execute_scene_operation({SceneMethod::ReadActive}).active;
            if (state.archive_session.empty()) return native_success({{"archive_session", ""}});
            auto selection = nlohmann::json::object();
            if (state.selection) {
                const auto& selected = *state.selection;
                selection = {{"scene", selected.scene}, {"actor_type", selected.actor_type}, {"actor", selected.actor},
                    {"source_viewport", selected.source_viewport}, {"source_camera_id", selected.source_camera_id}};
                if (selected.actor_guid) selection["actor_guid"] = *selected.actor_guid;
                if (selected.node_index) selection["node_index"] = *selected.node_index;
            }
            return native_success({{"archive_session", state.archive_session}, {"scene", state.scene},
                {"name", state.name}, {"project_path", state.project_path}, {"read_only", state.read_only},
                {"tree_revision", state.tree_revision}, {"selection", std::move(selection)}});
}

NativeResult script_scene_read_actor(const NativeRequest& request) {
            auto* scene = native_editor_state().scene.get();
            const auto query = arg_object(request.args, 0);
            if (!scene || !matches_archive_session(*scene, query)) return native_failure("PROJECT_CONTEXT_CHANGED", 409);
            const auto id = query.value("actor", std::string{});
            auto* actor = find_native_actor(*scene, id);
            if (!actor) return native_failure("Actor not found: " + id, 404);
            return native_success(actor_to_json(*scene, *actor));
}

NativeResult script_scene_read_transforms(const NativeRequest& request) {
            const auto query = arg_object(request.args, 0);
            auto* scene = native_editor_state().scene.get();
            if (!scene || !matches_archive_session(*scene, query)) return native_failure("PROJECT_CONTEXT_CHANGED", 409);
            auto values = nlohmann::json::array();
            for (const auto& guid : query.at("actors").get<std::vector<std::string>>()) {
                auto* actor = find_native_actor_by_guid(*scene, guid);
                if (!actor) continue;
                values.push_back({{"scene", scene->route}, {"actor", actor->name}, {"actor_guid", guid},
                    {"position", actor->geometry ? actor->geometry->get_position() : actor->position},
                    {"rotation", actor->geometry ? actor->geometry->get_rotation() : actor->rotation},
                    {"scale", actor->geometry ? actor->geometry->get_scale() : actor->scale}});
            }
            return native_success({{"archive_session", scene->archive_session}, {"actors", std::move(values)}});
}

NativeResult script_scene_read_archive_scene(const NativeRequest& request) {
            auto* scene = native_editor_state().scene.get();
            if (!scene || !matches_archive_session(*scene, arg_object(request.args, 0)))
                return native_failure("PROJECT_CONTEXT_CHANGED", 409);
            return native_success({{"archive_session", scene->archive_session}, {"scene", archive_scene_values(*scene)}});
}

NativeResult script_scene_get_scene_snapshot(const NativeRequest& request) {
            try {
                const auto raw = native_editor_scene_snapshot_json(arg_string(request.args, 0));
                const auto data = nlohmann::json::parse(raw);
                if (data.value("status", std::string{}) == "error") {
                    return native_failure(data.value("message", std::string{"Scene snapshot failed"}), 2);
                }
                return native_success(data);
            } catch (const std::exception& e) {
                return native_failure(e.what(), 2);
            }
}

NativeResult script_scene_set_actor_transform(const NativeRequest& request) {
            try {
                const auto transform = arg_object(request.args, 2);
                // The script channel may omit the archive; when it carries one it
                // must name the archive the editor currently has open.
                if (transform.contains("archive_session") &&
                    !matches_archive_session(*scene_for_request_route(request), transform))
                    return native_failure("PROJECT_CONTEXT_CHANGED", 409);
                return set_native_editor_actor_transform(
                    arg_string(request.args, 0), arg_string(request.args, 1), transform);
            } catch (const std::exception& e) {
                return native_failure(e.what(), 2);
            }
}

NativeResult script_scene_set_environment(const NativeRequest& request) {
            return set_native_environment(request);
}

NativeResult script_scene_set_actor_state(const NativeRequest& request) {
            return set_native_actor_properties(request, ActorPropertyAdapter::State);
}

NativeResult script_scene_set_actor_animation(const NativeRequest& request) {
            try {
                auto* scene = scene_for_request_route(request);
                auto* actor = find_native_actor(*scene, arg_string(request.args, 1));
                if (!actor) return native_failure("Actor not found", 2);
                auto player = actor_animation_player(*actor);
                if (!player) return native_failure("Actor has no geometry", 2);
                const auto animation = arg_string(request.args, 2);
                if (animation.empty()) return native_failure("animation must be a non-empty id or name", 2);
                const double seconds = request.args.size() > 3 && !request.args[3].is_null()
                    ? request.args[3].get<double>() : 0.2;
                player->request_switch(animation, seconds);
                return native_success({{"status", "accepted"}, {"scene", scene->route},
                                       {"actor", actor_to_json(*scene, *actor)}});
            } catch (const std::exception& error) { return native_failure(error.what(), 2); }
}

NativeResult script_scene_pause_actor_animation(const NativeRequest& request) {
            return set_native_actor_animation_paused(request, true);
}

NativeResult script_scene_resume_actor_animation(const NativeRequest& request) {
            return set_native_actor_animation_paused(request, false);
}

NativeResult script_scene_set_actor_physics(const NativeRequest& request) {
            return set_native_actor_properties(request, ActorPropertyAdapter::Mechanics);
}

NativeResult script_scene_set_camera_pose(const NativeRequest& request) {
            return apply_non_web_camera_properties(request, true);
}

NativeResult script_scene_create_actor(const NativeRequest& request) {
            const auto scene_route = arg_string(request.args, 0);
            const auto source_path = arg_string(request.args, 1);
            auto actor_type = normalize_route(arg_string(request.args, 2, "model"));
            const auto actor_data = arg_object(request.args, 3);
            const auto* scene = native_editor_state().scene.get();
            // Only validate the archive the caller actually named: script code may
            // import an actor into whatever world is open.
            if (actor_data.contains("archive_session") &&
                (!scene || !matches_archive_session(*scene, actor_data)))
                return native_failure("PROJECT_CONTEXT_CHANGED", 409);
            return create_native_editor_actor(scene_route, source_path, actor_type, actor_data,
                                              /*transient_assets=*/true);
}

NativeResult script_scene_remove_actor(const NativeRequest& request) {
            const auto scene_route = arg_string(request.args, 0);
            const auto actor_name = arg_string(request.args, 1);
            return remove_native_editor_actor(scene_route, actor_name);
}

NetworkReply execute_network_operation(const NetworkRequest& request) {
    NetworkReply reply;
    reply.method = request.method;
    const auto sys = require_network_system();
    if (!sys) {
        reply.error_code = "NATIVE_UNAVAILABLE";
        reply.error_message = "NetworkSystem unavailable";
        return reply;
    }
    switch (request.method) {
    case NetworkMethod::StartSession: {
        const auto role = request.role == "client" ? Corona::Systems::NetworkSystem::SessionRole::Client :
            request.role == "none" ? Corona::Systems::NetworkSystem::SessionRole::None :
            Corona::Systems::NetworkSystem::SessionRole::Host;
        const bool was_active = network_snapshot_session_active();
        {
            Corona::Network::NetworkCommand command{};
            command.kind = Corona::Network::NetworkCommandKind::StartSession;
            command.params = {request.name, std::to_string(request.project_id),
                              std::to_string(request.port),
                              role == Corona::Systems::NetworkSystem::SessionRole::None ? "none"
                              : role == Corona::Systems::NetworkSystem::SessionRole::Client ? "client"
                                                                                           : "host"};
            const auto result = Corona::Network::submit_and_wait(std::move(command));
            reply.ok = result && result->ok;
        }
        reply.session = read_network_session();
        reply.seeded_actor_count = reply.ok && !was_active ? seed_network_editor_state(role) : 0;
        break;
    }
    case NetworkMethod::StopSession: {
        Corona::Network::NetworkCommand command{};
        command.kind = Corona::Network::NetworkCommandKind::StopSession;
        Corona::Network::push_network_command(std::move(command));
        break;
    }
    case NetworkMethod::GetSessionInfo: reply.session = read_network_session(); break;
    case NetworkMethod::GetDiscoveredPeers: reply.peers = read_network_peers(); break;
    case NetworkMethod::ClearDiscoveredPeers: {
        Corona::Network::NetworkCommand command{};
        command.kind = Corona::Network::NetworkCommandKind::ClearDiscoveredPeers;
        Corona::Network::push_network_command(std::move(command));
        break;
    }
    case NetworkMethod::SearchLan:
        {
            Corona::Network::NetworkCommand command{};
            command.kind = Corona::Network::NetworkCommandKind::StartDiscovery;
            command.params = {request.name, std::to_string(request.project_id)};
            const auto result = Corona::Network::submit_and_wait(std::move(command));
            reply.ok = result && result->ok;
            // 发现结果来自快照（下一 tick 才有新数据，这里给的是当前已知数量）
            const auto snapshot = Corona::Network::NetworkStateChannel::instance().snapshots().read();
            reply.discovered_count = snapshot ? snapshot->peers.size() : 0;
        }
        break;
    case NetworkMethod::ConnectToPeer:
        reply.ok = network_connect_to_peer(request.ip, request.port, request.name);
        reply.session = read_network_session();
        break;
    case NetworkMethod::SetProjectRoot:
        if (!request.project_root.empty()) {
            Corona::Network::NetworkCommand command{};
            command.kind = Corona::Network::NetworkCommandKind::SetProjectRoot;
            command.name = request.project_root;
            Corona::Network::push_network_command(std::move(command));
        }
        break;
    }
    return reply;
}

// Transport only.  The envelope, its schema and the service allowlist belong to
// the Python side; keeping this free of branches is what makes the editor
// surface one function wide.
BridgeReply execute_bridge_operation(const std::string& envelope) {
    BridgeReply reply;
    if (envelope.empty()) {
        reply.error_code = "INVALID_ARGUMENT";
        reply.error_message = "Bridge envelope is empty";
        return reply;
    }
    const auto result = invoke_python_script_service(
        {"EditorBridge", "call", nlohmann::json::array({envelope})},
        "python-bridge",
        std::chrono::seconds(300));
    if (!result.success) {
        reply.error_code = result.error_code == 504 ? "EDITOR_API_TIMEOUT"
                           : result.error_code == 429 ? "SERVICE_BUSY"
                           : result.error_code == 503 ? "SERVICE_UNAVAILABLE"
                                                      : "SERVICE_FAILED";
        reply.error_message = result.error;
        return reply;
    }
    reply.json = result.data.is_string() ? result.data.get<std::string>() : result.data.dump();
    return reply;
}

nlohmann::json invoke_file_script_api(const std::string& api, const nlohmann::json& args) {
    const auto envelope = [](const NativeResult& result) {
        return nlohmann::json{{"success", result.success},
                              {"code", result.success ? 0 : result.error_code},
                              {"data", result.data},
                              {"error", result.error}};
    };
    if (api == "Scripts.run_file" || api == "Scripts.run_path") {
        return envelope(start_native_script_file(arg_object(args, 0), api == "Scripts.run_path"));
    }
    if (api == "Scripts.get_status") {
        return envelope(invoke_python_script_service({"FileScripts", "get_script_status", args}));
    }
    if (api == "Scripts.get_run") {
        return envelope(invoke_python_script_service({"FileScripts", "get_file_run", args}));
    }
    if (api == "Scripts.stop") {
        return envelope(invoke_python_script_service({"FileScripts", "stop_file", args}));
    }
    return {{"success", false}, {"code", 404}, {"error", api + " is not a file script route"}};
}

}  // namespace Corona::Systems::UI