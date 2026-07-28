#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <shobjidl.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "browser_manager.h"
#include "cef_client.h"
#include "cef_editor_api.h"
#include "cef_editor_native_api_registry.h"
#include "scene_folder.h"
#include "vision_actor_material_bridge.h"
#include "vision_actor_transform_bridge.h"

#include <corona/events/acoustics_system_events.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/script/corona_engine_api.h>
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

using SceneFolders::create_scene_folder;
using SceneFolders::detect_scene_folder;
using SceneFolders::migrate_legacy_scene;
using SceneFolders::SceneAssetStore;
using SceneFolders::SceneDocumentStore;

constexpr const char* VISION_DOCUMENT_ENCODING = "zlib_base64_json";
constexpr const char* VISION_DOCUMENT_VERSION = "1";

struct EmbeddedVisionDocument {
    nlohmann::json document;
    std::string data;
    std::string asset_root;
};

class PortableSceneValidationError : public std::runtime_error {
   public:
    PortableSceneValidationError(std::string message, nlohmann::json diagnostics)
        : std::runtime_error(std::move(message)), diagnostics_(std::move(diagnostics)) {}

    [[nodiscard]] const nlohmann::json& diagnostics() const noexcept { return diagnostics_; }

   private:
    nlohmann::json diagnostics_;
};

std::shared_ptr<Corona::Systems::NetworkSystem> get_network_system() {
    auto sys_mgr = Corona::Kernel::KernelContext::instance().system_manager();
    if (!sys_mgr) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<Corona::Systems::NetworkSystem>(
        sys_mgr->get_system("Network"));
}

Corona::Systems::NetworkSystem::SessionRole parse_network_session_role(
    const nlohmann::json& value) {
    if (!value.is_string()) {
        return Corona::Systems::NetworkSystem::SessionRole::Host;
    }
    const auto role = value.get<std::string>();
    if (role == "client") {
        return Corona::Systems::NetworkSystem::SessionRole::Client;
    }
    if (role == "none") {
        return Corona::Systems::NetworkSystem::SessionRole::None;
    }
    return Corona::Systems::NetworkSystem::SessionRole::Host;
}

const char* editor_api_value_type_name(EditorApiValueType type) {
    switch (type) {
        case EditorApiValueType::Any:
            return "any";
        case EditorApiValueType::Null:
            return "null";
        case EditorApiValueType::Boolean:
            return "boolean";
        case EditorApiValueType::Integer:
            return "integer";
        case EditorApiValueType::Number:
            return "number";
        case EditorApiValueType::String:
            return "string";
        case EditorApiValueType::Object:
            return "object";
        case EditorApiValueType::Array:
            return "array";
    }
    return "unknown";
}

std::string snake_to_lower_camel(const char* value) {
    if (!value) {
        return {};
    }
    std::string result;
    bool upper_next = false;
    for (const char ch : std::string(value)) {
        if (ch == '_') {
            upper_next = true;
            continue;
        }
        if (upper_next) {
            result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            upper_next = false;
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

const char* editor_api_module_js_wrapper_alias(const char* module) {
    const std::string_view name = module ? std::string_view(module) : std::string_view();
    if (name == "AITool") {
        return "ai";
    }
    if (name == "CoronaEditor") {
        return "app";
    }
    if (name == "EditorApi") {
        return "editor";
    }
    if (name == "FileManager") {
        return "files";
    }
    if (name == "LANChat") {
        return "lanChat";
    }
    if (name == "MainView") {
        return "main";
    }
    if (name == "Network") {
        return "network";
    }
    if (name == "ProjectLauncher") {
        return "project";
    }
    if (name == "ProjectSettings") {
        return "projectSettings";
    }
    if (name == "ResourceSearch") {
        return "resourceSearch";
    }
    if (name == "SceneDatas") {
        return "sceneDatas";
    }
    if (name == "SceneTools") {
        return "sceneTools";
    }
    if (name == "ScratchTool") {
        return "scratch";
    }
    return "";
}

const char* editor_api_module_python_wrapper_alias(const char* module) {
    const std::string_view name = module ? std::string_view(module) : std::string_view();
    if (name == "AITool") {
        return "ai";
    }
    if (name == "CoronaEditor") {
        return "app";
    }
    if (name == "EditorApi") {
        return "editor";
    }
    if (name == "FileManager") {
        return "files";
    }
    if (name == "LANChat") {
        return "lan_chat";
    }
    if (name == "MainView") {
        return "main";
    }
    if (name == "Network") {
        return "network";
    }
    if (name == "ProjectLauncher") {
        return "project";
    }
    if (name == "ProjectSettings") {
        return "project_settings";
    }
    if (name == "ResourceSearch") {
        return "resource_search";
    }
    if (name == "SceneDatas") {
        return "scene_datas";
    }
    if (name == "SceneTools") {
        return "scene_tools";
    }
    if (name == "ScratchTool") {
        return "scratch";
    }
    return "";
}

std::string editor_api_js_wrapper_path(const EditorApiMethodSpec& spec) {
    if (spec.js_wrapper && *spec.js_wrapper) {
        return spec.js_wrapper;
    }
    const std::string module_alias = editor_api_module_js_wrapper_alias(spec.native_module);
    const std::string function_name = snake_to_lower_camel(spec.native_function);
    if (module_alias.empty() || function_name.empty()) {
        return {};
    }
    return module_alias + "." + function_name;
}

std::string editor_api_python_wrapper_path(const EditorApiMethodSpec& spec) {
    if (spec.python_wrapper && *spec.python_wrapper) {
        return spec.python_wrapper;
    }
    const std::string module_alias = editor_api_module_python_wrapper_alias(spec.native_module);
    const std::string function_name = spec.native_function ? spec.native_function : "";
    if (module_alias.empty() || function_name.empty()) {
        return {};
    }
    return module_alias + "." + function_name;
}

nlohmann::json editor_api_method_to_json(const EditorApiMethodSpec& spec) {
    nlohmann::json params = nlohmann::json::array();
    for (std::size_t index = 0; index < spec.param_count; ++index) {
        const auto& param = spec.params[index];
        params.push_back({
            {"name", param.name ? param.name : ""},
            {"type", editor_api_value_type_name(param.type)},
            {"optional", param.optional},
        });
    }

    return {
        {"api", spec.api_name ? spec.api_name : ""},
        {"native_module", spec.native_module ? spec.native_module : ""},
        {"native_function", spec.native_function ? spec.native_function : ""},
        {"params", params},
        {"return", editor_api_value_type_name(spec.return_spec.type)},
        {"js_wrapper", editor_api_js_wrapper_path(spec)},
        {"python_wrapper", editor_api_python_wrapper_path(spec)},
        {"async", spec.async},
        {"allowed_callers", spec.allowed_callers},
    };
}

nlohmann::json editor_api_event_to_json(const EditorApiEventSpec& spec) {
    return {
        {"event", spec.event_name ? spec.event_name : ""},
        {"payload", editor_api_value_type_name(spec.payload_type)},
        {"allowed_callers", spec.allowed_callers},
        {"js_wrapper", spec.js_wrapper ? spec.js_wrapper : ""},
        {"python_wrapper", spec.python_wrapper ? spec.python_wrapper : ""},
    };
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
using IniFile = std::unordered_map<std::string, IniSection>;

void strip_utf8_bom(std::string& line) {
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
}

IniFile read_ini_file(const std::filesystem::path& file_path) {
    IniFile result;
    std::ifstream input(file_path);
    if (!input) {
        return result;
    }

    std::string section;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        strip_utf8_bom(line);
        auto trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = to_lower_ascii(trim_ascii(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }
        const auto equals = trimmed.find('=');
        if (equals == std::string::npos || section.empty()) {
            continue;
        }
        auto key = to_lower_ascii(trim_ascii(trimmed.substr(0, equals)));
        auto value = trim_ascii(trimmed.substr(equals + 1));
        result[section][key] = value;
    }
    return result;
}

std::string ini_value(const IniFile& ini,
                      const std::string& section,
                      const std::string& key,
                      const std::string& fallback = {}) {
    const auto sec_it = ini.find(to_lower_ascii(section));
    if (sec_it == ini.end()) {
        return fallback;
    }
    const auto key_it = sec_it->second.find(to_lower_ascii(key));
    return key_it == sec_it->second.end() ? fallback : key_it->second;
}

std::vector<std::string> split_csv_routes(const std::string& value) {
    std::vector<std::string> routes;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        item = normalize_route(item);
        if (!item.empty()) {
            routes.push_back(item);
        }
    }
    return routes;
}

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

int parse_int(const std::string& value, int fallback) {
    try {
        return std::stoi(trim_ascii(value));
    } catch (...) {
        return fallback;
    }
}

float parse_float(const std::string& value, float fallback) {
    try {
        return std::stof(trim_ascii(value));
    } catch (...) {
        return fallback;
    }
}

struct NativeEditorCamera {
    std::string camera_id;
    std::string name;
    bool deletable{true};
    std::string vision_spp;
    std::string vision_max_depth;
    std::string vision_denoise;
    int width{1920};
    int height{1080};
    bool view_open{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    std::unique_ptr<Corona::API::Camera> engine_camera;
};

struct NativeEditorActorOpticsState {
    std::optional<std::array<float, 3>> diffuse;
    std::optional<float> metallic;
    std::optional<float> roughness;
    std::optional<float> specular;
    std::optional<float> shininess;
    std::optional<std::array<float, 3>> emission;
    std::string texture;
};

enum class ActorLoadStatus {
    Loaded,
    MissingResource,
    DecodeFailed,
    UnsupportedResource,
};

const char* actor_load_status_name(ActorLoadStatus status) {
    switch (status) {
        case ActorLoadStatus::Loaded: return "loaded";
        case ActorLoadStatus::MissingResource: return "missing_resource";
        case ActorLoadStatus::DecodeFailed: return "decode_failed";
        case ActorLoadStatus::UnsupportedResource: return "unsupported_resource";
    }
    return "decode_failed";
}

struct NativeEditorActor {
    std::string name;
    std::string actor_guid;
    std::string route;
    std::string actor_type{"actor"};
    std::string runtime_entity_id;
    std::string asset_id;
    std::string model_ref;
    std::string entity_type;
    std::string semantic_role;
    std::string source_plan_id;
    std::string source_batch_id;
    int source_scene_version{1};
    int actor_version{1};
    bool follow_camera{false};
    std::uint64_t audio_resource_id{0};  // 音频物体绑定的音频资源 id（actor_type=="audio"）
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    ActorLoadStatus load_status{ActorLoadStatus::Loaded};
    std::string load_error_code;
    std::string load_error_message;
    std::string resolved_asset_path;
    bool persisted_visible{true};
    bool persisted_physics_enabled{true};
    std::string persisted_collision_type{"box"};
    nlohmann::json persisted_snapshot = nlohmann::json::object();
    NativeEditorActorOpticsState persisted_optics;
    std::unique_ptr<Corona::API::Geometry> geometry;
    std::unique_ptr<Corona::API::Optics> optics;
    std::unique_ptr<Corona::API::Mechanics> mechanics;
    std::unique_ptr<Corona::API::Acoustics> acoustics;
    std::unique_ptr<Corona::API::Actor> engine_actor;
};

std::string collision_shape_name(const Corona::API::Mechanics& mechanics) {
    return mechanics.get_collision_shape();
}

std::string normalize_collision_type(std::string value) {
    if (value == "none" || value == "box" || value == "mesh") return value;
    CFW_LOG_WARNING("Invalid collision type '{}'; using box", value);
    return "box";
}

struct NativeEditorScene {
    std::filesystem::path project_root;
    std::string route;
    std::string name;
    std::string core_version;
    std::string script_path;
    std::string terrain_type;
    std::string terrain_path;
    std::string vision_storage;
    std::string vision_source_id;
    std::string vision_source_path;
    std::string vision_import_mode;
    std::string vision_document_version;
    std::string vision_document_encoding;
    std::string vision_document_data;
    std::string vision_document_asset_root;
    std::array<float, 3> sun_direction{1.0f, 1.0f, 1.0f};
    bool sun_enabled{true};
    bool floor_grid_enabled{true};
    std::vector<NativeEditorActor> actors;
    std::vector<NativeEditorCamera> cameras;
    nlohmann::json load_diagnostics = nlohmann::json::array();
    size_t active_camera_index{0};
    std::unique_ptr<Corona::API::Environment> environment;
    std::unique_ptr<Corona::API::Scene> engine_scene;
};

struct NativeEditorState {
    std::string project_path;
    std::unique_ptr<NativeEditorScene> scene;
};

NativeEditorState& native_editor_state() {
    static NativeEditorState state;
    return state;
}

bool is_valid_project_dir(const std::filesystem::path& project_dir);
std::filesystem::path canonical_project_dir_for_settings(const std::filesystem::path& project_dir);
std::string safe_project_dir_name(std::string name, const std::string& fallback);
std::string sanitize_vision_source_id(std::string source_id);
std::string fnv1a_hex12(std::string_view text);
std::string encode_vision_document_data(const nlohmann::json& document);
nlohmann::json decode_vision_document_data(const std::string& data);
bool is_vision_resource_path_key(std::string key);
nlohmann::json vision_document_for_render(nlohmann::json document);
bool bind_missing_native_actor_materials(NativeEditorScene& scene,
                                         nlohmann::json& document);
bool hydrate_native_actor_transforms_from_vision_document(
    NativeEditorScene& scene,
    nlohmann::json& document);
void cleanup_vision_document_editor_transform_overrides(nlohmann::json& document);
std::string embedded_vision_scene_key(const NativeEditorScene& scene);
void clear_embedded_vision_actor_bindings(NativeEditorScene& scene);
void register_embedded_vision_actor_bindings(NativeEditorScene& scene,
                                             const nlohmann::json& document,
                                             const std::string& scene_key);
bool refresh_embedded_vision_view(NativeEditorScene& scene,
                                  const nlohmann::json& document);
bool persist_embedded_vision_document(NativeEditorScene& scene, nlohmann::json& document);
bool sync_native_actor_to_embedded_vision_document(NativeEditorScene& scene,
                                                   const NativeEditorActor& actor,
                                                   bool create_if_missing = false,
                                                   bool sync_transform = true);
bool remove_native_actor_from_embedded_vision_document(NativeEditorScene& scene,
                                                       const std::string& actor_guid);
void replace_ini_section_from_map(const std::filesystem::path& file_path,
                                  const std::string& section_name,
                                  const std::map<std::string, std::string>& values);
EmbeddedVisionDocument create_embedded_vision_document(const std::filesystem::path& project_dir,
                                                       const std::filesystem::path& json_path,
                                                       const nlohmann::json& source_document);
void persist_vision_proxy_actors_from_document(const std::filesystem::path& project_dir,
                                               const std::filesystem::path& scene_file,
                                               const nlohmann::json& document,
                                               const std::filesystem::path& source_dir);
std::map<std::string, std::string> vision_camera_section(const nlohmann::json& document);

std::string read_last_project_from_editor_ini() {
    const auto cwd_ini = std::filesystem::current_path() / "CoronaEditor.ini";
    const auto ini = read_ini_file(cwd_ini);
    const auto raw = ini_value(ini, "General", "last_project");
    if (raw.empty()) {
        return {};
    }
    return normalize_route(path_to_utf8(canonical_project_dir_for_settings(path_from_utf8(raw))));
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

std::string choose_single_scene_route(const std::filesystem::path& project_root,
                                      const IniFile& project_ini) {
    auto route = normalize_route(ini_value(project_ini, "Project", "entrance_scene"));
    if (!route.empty()) {
        return route;
    }

    auto scenes = split_csv_routes(ini_value(project_ini, "Project", "scenes"));
    if (!scenes.empty()) {
        return scenes.front();
    }

    const auto scene_dir = project_root / "Scene";
    if (std::filesystem::is_directory(scene_dir)) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(scene_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".scene") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        if (!files.empty()) {
            return normalize_route(path_to_utf8(std::filesystem::relative(files.front(), project_root)));
        }
    }
    return {};
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

std::string format_float3(const std::array<float, 3>& value) {
    std::ostringstream out;
    out << std::setprecision(9)
        << value[0] << ", " << value[1] << ", " << value[2];
    return out.str();
}

std::string unique_actor_key(const NativeEditorScene& scene,
                             const NativeEditorActor& actor,
                             std::unordered_map<std::string, int>& used_keys,
                             size_t index) {
    std::string base = actor.name.empty() ? "actor" + std::to_string(index + 1) : actor.name;
    auto used = used_keys.find(base);
    if (used == used_keys.end()) {
        used_keys.emplace(base, 0);
        return base;
    }
    used->second += 1;
    return base + "_" + std::to_string(used->second);
}

std::vector<std::string> build_actors_section_lines(const NativeEditorScene& scene) {
    std::vector<std::string> lines;
    lines.emplace_back("[actors]");
    std::unordered_map<std::string, int> used_keys;
    for (size_t index = 0; index < scene.actors.size(); ++index) {
        const auto& actor = scene.actors[index];
        const auto key = unique_actor_key(scene, actor, used_keys, index);
        lines.push_back(key + ".actor_type = " + actor.actor_type);
        lines.push_back(key + ".name = " + actor.name);
        lines.push_back(key + ".route = " + actor.route);
        if (!actor.actor_guid.empty()) {
            lines.push_back(key + ".actor_guid = " + actor.actor_guid);
        }
        if (!actor.runtime_entity_id.empty()) {
            lines.push_back(key + ".runtime.entity_id = " + actor.runtime_entity_id);
        }
        if (!actor.asset_id.empty()) {
            lines.push_back(key + ".runtime.asset_id = " + actor.asset_id);
        }
        if (!actor.model_ref.empty()) {
            lines.push_back(key + ".runtime.model_ref = " + actor.model_ref);
        }
        if (!actor.entity_type.empty()) {
            lines.push_back(key + ".runtime.entity_type = " + actor.entity_type);
        }
        if (!actor.semantic_role.empty()) {
            lines.push_back(key + ".runtime.semantic_role = " + actor.semantic_role);
        }
        if (!actor.source_plan_id.empty()) {
            lines.push_back(key + ".runtime.source_plan_id = " + actor.source_plan_id);
        }
        if (!actor.source_batch_id.empty()) {
            lines.push_back(key + ".runtime.source_batch_id = " + actor.source_batch_id);
        }
        lines.push_back(key + ".runtime.source_scene_version = " +
                        std::to_string(std::max(actor.source_scene_version, 1)));
        lines.push_back(key + ".runtime.actor_version = " + std::to_string(std::max(actor.actor_version, 1)));
        lines.push_back(key + ".follow_camera = " + std::string(actor.follow_camera ? "true" : "false"));
        if (actor.load_status != ActorLoadStatus::Loaded) {
            lines.push_back(key + ".mechanics.physics_enabled = " +
                            std::string(actor.persisted_physics_enabled ? "true" : "false"));
            lines.push_back(key + ".mechanics.collision_type = " + actor.persisted_collision_type);
        } else if (actor.mechanics) {
            lines.push_back(key + ".mechanics.physics_enabled = " +
                            std::string(actor.mechanics->get_physics_enabled() ? "true" : "false"));
            lines.push_back(key + ".mechanics.collision_type = " +
                            collision_shape_name(*actor.mechanics));
        }
        lines.push_back(key + ".geometry.position = " + format_float3(actor.geometry ? actor.geometry->get_position() : actor.position));
        lines.push_back(key + ".geometry.rotation = " + format_float3(actor.geometry ? actor.geometry->get_rotation() : actor.rotation));
        lines.push_back(key + ".geometry.scale = " + format_float3(actor.geometry ? actor.geometry->get_scale() : actor.scale));
        if (actor.load_status != ActorLoadStatus::Loaded) {
            lines.push_back(key + ".optics.visible = " +
                            std::string(actor.persisted_visible ? "true" : "false"));
            if (actor.persisted_optics.diffuse) {
                lines.push_back(key + ".optics.diffuse = " + format_float3(*actor.persisted_optics.diffuse));
            }
            if (actor.persisted_optics.metallic) lines.push_back(key + ".optics.metallic = " + std::to_string(*actor.persisted_optics.metallic));
            if (actor.persisted_optics.roughness) lines.push_back(key + ".optics.roughness = " + std::to_string(*actor.persisted_optics.roughness));
            if (actor.persisted_optics.specular) lines.push_back(key + ".optics.specular = " + std::to_string(*actor.persisted_optics.specular));
            if (actor.persisted_optics.shininess) lines.push_back(key + ".optics.shininess = " + std::to_string(*actor.persisted_optics.shininess));
        } else if (actor.optics) {
            lines.push_back(key + ".optics.visible = " +
                            std::string(actor.optics->get_visible() ? "true" : "false"));
            lines.push_back(key + ".optics.diffuse = " + format_float3(actor.optics->get_diffuse()));
            lines.push_back(key + ".optics.metallic = " + std::to_string(actor.optics->get_metallic()));
            lines.push_back(key + ".optics.roughness = " + std::to_string(actor.optics->get_roughness()));
            lines.push_back(key + ".optics.specular = " + std::to_string(actor.optics->get_specular()));
            lines.push_back(key + ".optics.shininess = " + std::to_string(actor.optics->get_shininess()));
        }
        if (actor.persisted_optics.emission) {
            lines.push_back(key + ".optics.emission = " + format_float3(*actor.persisted_optics.emission));
        }
        if (!actor.persisted_optics.texture.empty()) {
            lines.push_back(key + ".material.texture = " + actor.persisted_optics.texture);
        }
        if (actor.actor_type == "audio" && actor.audio_resource_id != 0) {
            lines.push_back(key + ".audio_resource_id = " + std::to_string(actor.audio_resource_id));
        }
        const auto persisted_fields = actor.persisted_snapshot.value(
            "persisted_fields", nlohmann::json::object());
        static const std::set<std::string> normalized_fields{
            "actor_guid", "actor_type", "audio_resource_id", "follow_camera", "name", "route",
            "geometry.position", "geometry.rotation", "geometry.scale",
            "material.texture", "mechanics.collision_enabled", "mechanics.collision_type",
            "mechanics.physics_enabled", "optics.diffuse", "optics.emission", "optics.metallic",
            "optics.roughness", "optics.shininess", "optics.specular", "optics.visible"};
        if (persisted_fields.is_object()) {
            for (const auto& field : persisted_fields.items()) {
                const auto dot = field.key().find('.');
                if (dot == std::string::npos || dot + 1 >= field.key().size()) continue;
                const auto suffix = field.key().substr(dot + 1);
                if (normalized_fields.contains(suffix) || !field.value().is_string()) continue;
                lines.push_back(key + "." + suffix + " = " + field.value().get<std::string>());
            }
        }
    }
    return lines;
}

std::vector<std::string> build_sun_section_lines(const NativeEditorScene& scene) {
    return {
        "[sun]",
        "enabled = " + std::string(scene.sun_enabled ? "true" : "false"),
        "sun_direction = " + format_float3(scene.sun_direction),
    };
}

std::vector<std::string> build_grid_section_lines(const NativeEditorScene& scene) {
    return {
        "[grid]",
        "enabled = " + std::string(scene.floor_grid_enabled ? "true" : "false"),
    };
}

std::string format_bool(bool value) {
    return value ? "true" : "false";
}

std::vector<std::string> build_camera_section_lines(const NativeEditorScene& scene) {
    std::vector<std::string> lines;
    lines.emplace_back("[camera]");
    lines.push_back("count = " + std::to_string(scene.cameras.size()));
    if (scene.cameras.empty()) {
        lines.emplace_back("active_id = ");
        return lines;
    }

    const auto active_index = std::min(scene.active_camera_index, scene.cameras.size() - 1);
    lines.push_back("active_id = " + scene.cameras[active_index].camera_id);
    for (size_t index = 0; index < scene.cameras.size(); ++index) {
        const auto& camera = scene.cameras[index];
        const auto prefix = "camera" + std::to_string(index);
        lines.push_back(prefix + ".id = " + camera.camera_id);
        lines.push_back(prefix + ".name = " + camera.name);
        lines.push_back(prefix + ".deletable = " + format_bool(camera.deletable));
        if (camera.engine_camera) {
            lines.push_back(prefix + ".position = " + format_float3(camera.engine_camera->get_position()));
            lines.push_back(prefix + ".forward = " + format_float3(camera.engine_camera->get_forward()));
            lines.push_back(prefix + ".world_up = " + format_float3(camera.engine_camera->get_world_up()));
            lines.push_back(prefix + ".fov = " + std::to_string(camera.engine_camera->get_fov()));
            lines.push_back(prefix + ".output_mode = " + camera.engine_camera->get_output_mode());
            lines.push_back(prefix + ".ssao_enabled = " + format_bool(camera.engine_camera->get_ssao_enabled()));
            lines.push_back(prefix + ".render_backend = " + camera.engine_camera->get_render_backend());
            lines.push_back(prefix + ".vision_render_mode = " + camera.engine_camera->get_vision_render_mode());
        }
        if (!camera.vision_spp.empty()) {
            lines.push_back(prefix + ".vision_spp = " + camera.vision_spp);
        }
        if (!camera.vision_max_depth.empty()) {
            lines.push_back(prefix + ".vision_max_depth = " + camera.vision_max_depth);
        }
        if (!camera.vision_denoise.empty()) {
            lines.push_back(prefix + ".vision_denoise = " + camera.vision_denoise);
        }
        lines.push_back(prefix + ".width = " + std::to_string(camera.width));
        lines.push_back(prefix + ".height = " + std::to_string(camera.height));
        lines.push_back(prefix + ".move_speed = " + std::to_string(camera.move_speed));
        lines.push_back(prefix + ".view_open = " + format_bool(camera.view_open));
        lines.push_back(prefix + ".view_x = " + std::to_string(camera.view_x));
        lines.push_back(prefix + ".view_y = " + std::to_string(camera.view_y));
        lines.push_back(prefix + ".view_width = " + std::to_string(camera.view_width));
        lines.push_back(prefix + ".view_height = " + std::to_string(camera.view_height));
    }
    return lines;
}

void replace_ini_section(const std::filesystem::path& file_path,
                         const std::string& section_name,
                          const std::vector<std::string>& replacement_lines) {
    if (file_path.filename() == "scene.ini" && detect_scene_folder(file_path.parent_path())) {
        SceneDocumentStore document_store(file_path.parent_path());
        std::vector<SceneFolders::Diagnostic> diagnostics;
        if (!document_store.replace_sections({{section_name, replacement_lines}}, diagnostics)) {
            throw std::runtime_error(diagnostics.empty() ? "Unable to update portable scene"
                                                         : diagnostics.front().message);
        }
        return;
    }
    if (to_lower_ascii(file_path.extension().string()) == ".scene" ||
        file_path.filename() == "scene.ini") {
        throw PortableSceneValidationError(
            "Legacy projects are read-only; use Save as Portable Scene before editing",
            nlohmann::json::array({{{"code", "legacy_read_only"},
                                    {"message", "Legacy projects are read-only"},
                                    {"path", path_to_utf8(file_path)},
                                    {"actor", ""}, {"field", ""}}}));
    }
    std::vector<std::string> lines;
    {
        std::ifstream input(file_path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            strip_utf8_bom(line);
            lines.push_back(line);
        }
    }

    const auto target = to_lower_ascii(section_name);
    auto is_target_section = [&](const std::string& line) {
        const auto trimmed = trim_ascii(line);
        if (trimmed.size() < 3 || trimmed.front() != '[' || trimmed.back() != ']') {
            return false;
        }
        return to_lower_ascii(trim_ascii(trimmed.substr(1, trimmed.size() - 2))) == target;
    };
    auto is_any_section = [](const std::string& line) {
        const auto trimmed = trim_ascii(line);
        return trimmed.size() >= 3 && trimmed.front() == '[' && trimmed.back() == ']';
    };

    auto begin = lines.end();
    for (auto it = lines.begin(); it != lines.end(); ++it) {
        if (is_target_section(*it)) {
            begin = it;
            break;
        }
    }

    if (begin == lines.end()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.insert(lines.end(), replacement_lines.begin(), replacement_lines.end());
    } else {
        auto end = std::next(begin);
        while (end != lines.end() && !is_any_section(*end)) {
            ++end;
        }
        auto insert_pos = lines.erase(begin, end);
        insert_pos = lines.insert(insert_pos, replacement_lines.begin(), replacement_lines.end());
        auto after_insert = std::next(insert_pos, static_cast<std::ptrdiff_t>(replacement_lines.size()));
        if (after_insert != lines.end() && (after_insert == lines.begin() || !std::prev(after_insert)->empty())) {
            after_insert = lines.insert(after_insert, "");
            ++after_insert;
        }

        // A UTF-8 BOM on the first header used to make the original section
        // invisible to the parser, so settings updates appended a duplicate
        // section. Keep the newly written section and remove any stale copies.
        for (auto it = after_insert; it != lines.end();) {
            if (!is_target_section(*it)) {
                ++it;
                continue;
            }
            auto duplicate_end = std::next(it);
            while (duplicate_end != lines.end() && !is_any_section(*duplicate_end)) {
                ++duplicate_end;
            }
            it = lines.erase(it, duplicate_end);
        }
    }

    std::ofstream output(file_path, std::ios::trunc);
    for (const auto& line : lines) {
        output << line << '\n';
    }
}

void remove_ini_section(const std::filesystem::path& file_path,
                        const std::string& section_name) {
    if (file_path.filename() == "scene.ini" && detect_scene_folder(file_path.parent_path())) {
        SceneDocumentStore document_store(file_path.parent_path());
        std::vector<SceneFolders::Diagnostic> diagnostics;
        if (!document_store.replace_sections({{section_name, {}}}, diagnostics)) {
            throw std::runtime_error(diagnostics.empty() ? "Unable to update portable scene"
                                                         : diagnostics.front().message);
        }
        return;
    }
    std::vector<std::string> lines;
    {
        std::ifstream input(file_path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            strip_utf8_bom(line);
            lines.push_back(line);
        }
    }

    const auto target = to_lower_ascii(section_name);
    auto is_target_section = [&](const std::string& line) {
        const auto trimmed = trim_ascii(line);
        if (trimmed.size() < 3 || trimmed.front() != '[' || trimmed.back() != ']') {
            return false;
        }
        return to_lower_ascii(trim_ascii(trimmed.substr(1, trimmed.size() - 2))) == target;
    };
    auto is_any_section = [](const std::string& line) {
        const auto trimmed = trim_ascii(line);
        return trimmed.size() >= 3 && trimmed.front() == '[' && trimmed.back() == ']';
    };

    auto begin = lines.end();
    for (auto it = lines.begin(); it != lines.end(); ++it) {
        if (is_target_section(*it)) {
            begin = it;
            break;
        }
    }
    if (begin == lines.end()) {
        return;
    }
    auto end = std::next(begin);
    while (end != lines.end() && !is_any_section(*end)) {
        ++end;
    }
    lines.erase(begin, end);

    std::ofstream output(file_path, std::ios::trunc);
    for (const auto& line : lines) {
        output << line << '\n';
    }
}

void persist_native_scene_actors(const NativeEditorScene& scene) {
    const auto scene_file = resolve_project_path(scene.project_root, scene.route);
    replace_ini_section(scene_file, "actors", build_actors_section_lines(scene));
}

void persist_native_scene_environment(const NativeEditorScene& scene) {
    const auto scene_file = resolve_project_path(scene.project_root, scene.route);
    replace_ini_section(scene_file, "sun", build_sun_section_lines(scene));
    replace_ini_section(scene_file, "grid", build_grid_section_lines(scene));
}

void persist_native_scene_cameras(const NativeEditorScene& scene) {
    const auto scene_file = resolve_project_path(scene.project_root, scene.route);
    replace_ini_section(scene_file, "camera", build_camera_section_lines(scene));
}

std::filesystem::path resolve_project_sidecar_vision_json(const NativeEditorScene& scene) {
    const auto source_id = sanitize_vision_source_id(scene.vision_source_id);
    if (source_id.empty()) {
        return {};
    }
    return scene.project_root / ".corona" / "vision_sources" / source_id / "vision.json";
}

void persist_native_scene_vision_metadata(const NativeEditorScene& scene) {
    const auto scene_file = resolve_project_path(scene.project_root, scene.route);
    const auto storage = normalize_route(scene.vision_storage);
    const auto source_id = sanitize_vision_source_id(scene.vision_source_id);
    if (storage == "embedded" && !scene.vision_document_data.empty()) {
        replace_ini_section(scene_file,
                            "vision",
                            {"[vision]",
                             "import_mode = external",
                             "storage = embedded"});
        return;
    }
    if (storage == "project_sidecar" && !source_id.empty()) {
        replace_ini_section(scene_file,
                            "vision",
                            {"[vision]",
                             "import_mode = external",
                             "source_id = " + source_id,
                             "storage = project_sidecar"});
        return;
    }

    const std::string vision_section = "vision";
    remove_ini_section(scene_file, vision_section);
}

void persist_native_scene_vision_document(const NativeEditorScene& scene) {
    const auto scene_file = resolve_project_path(scene.project_root, scene.route);
    if (normalize_route(scene.vision_storage) != "embedded" ||
        scene.vision_document_data.empty()) {
        return;
    }
    replace_ini_section(scene_file,
                        "vision_document",
                        {"[vision_document]",
                         "version = " + (scene.vision_document_version.empty()
                                             ? std::string(VISION_DOCUMENT_VERSION)
                                             : scene.vision_document_version),
                         "encoding = " + (scene.vision_document_encoding.empty()
                                              ? std::string(VISION_DOCUMENT_ENCODING)
                                              : scene.vision_document_encoding),
                         "asset_root = " + scene.vision_document_asset_root,
                         "data = " + scene.vision_document_data});
}

void persist_native_scene_common(const NativeEditorScene& scene) {
    if (detect_scene_folder(scene.project_root)) {
        SceneFolders::SceneAssetStore store(scene.project_root);
        auto diagnostics = store.validate_manifest();
        const auto validate_route = [&](const std::string& route,
                                        const std::string& field,
                                        const std::string& actor = {},
                                        bool allow_missing = false) {
            if (route.empty()) return;
            if (!SceneFolders::is_valid_asset_route(route) ||
                (!allow_missing && !store.contains_route(route))) {
                diagnostics.push_back({"untrusted_asset_route",
                                       "Portable scene resource is outside Assets or absent from the manifest",
                                       path_from_utf8(route), actor, field});
            }
        };
        for (const auto& actor : scene.actors) {
            validate_route(actor.route, "actors.route", actor.name,
                           actor.load_status != ActorLoadStatus::Loaded);
            validate_route(actor.persisted_optics.texture, "actors.material.texture", actor.name,
                           true);
        }
        validate_route(scene.script_path, "scripts.path");
        validate_route(scene.terrain_path, "terrain.path");
        if (!scene.vision_document_asset_root.empty() && scene.vision_document_asset_root != "Assets") {
            diagnostics.push_back({"untrusted_asset_route", "Vision asset_root must be Assets",
                                   path_from_utf8(scene.vision_document_asset_root), {},
                                   "vision_document.asset_root"});
        }
        if (!scene.vision_document_data.empty()) {
            try {
                const auto document = decode_vision_document_data(scene.vision_document_data);
                const auto validate_document = [&](const auto& self, const nlohmann::json& value,
                                                   const std::string& field) -> void {
                    if (value.is_object()) {
                        for (const auto& item : value.items()) {
                            if (SceneFolders::is_vision_output_section_key(item.key())) continue;
                            const auto child_field = field.empty() ? item.key() : field + "." + item.key();
                            if (is_vision_resource_path_key(item.key()) && item.value().is_string()) {
                                const auto route = trim_ascii(item.value().get<std::string>());
                                if (to_lower_ascii(route).rfind("data:", 0) == 0) continue;
                                if (route.find("://") != std::string::npos) {
                                    diagnostics.push_back({"remote_dependency",
                                                           "Remote Vision resources are unsupported",
                                                           path_from_utf8(route), {}, child_field});
                                } else {
                                    validate_route(route, child_field);
                                }
                            } else {
                                self(self, item.value(), child_field);
                            }
                        }
                    } else if (value.is_array()) {
                        for (size_t index = 0; index < value.size(); ++index) {
                            self(self, value[index], field + "[" + std::to_string(index) + "]");
                        }
                    }
                };
                validate_document(validate_document, document, "vision_document.data");
            } catch (const std::exception& error) {
                diagnostics.push_back({"invalid_vision_document", error.what(), {}, {},
                                       "vision_document.data"});
            }
        }
        if (!diagnostics.empty()) {
            nlohmann::json details = nlohmann::json::array();
            for (const auto& diagnostic : diagnostics) {
                details.push_back({{"code", diagnostic.code}, {"message", diagnostic.message},
                                   {"path", path_to_utf8(diagnostic.path)}, {"actor", diagnostic.actor},
                                   {"field", diagnostic.field}});
            }
            throw PortableSceneValidationError("Portable scene validation failed", std::move(details));
        }
    } else {
        throw PortableSceneValidationError(
            "Legacy projects are read-only; use Save as Portable Scene before editing",
            nlohmann::json::array({{{"code", "legacy_read_only"},
                                    {"message", "Legacy projects are read-only"},
                                    {"path", path_to_utf8(scene.project_root)},
                                    {"actor", ""}, {"field", ""}}}));
    }
    std::map<std::string, std::vector<std::string>> sections;
    std::map<std::string, std::string> scene_metadata;
    const auto current_ini = read_ini_file(resolve_project_path(scene.project_root, scene.route));
    if (const auto scene_it = current_ini.find("scene"); scene_it != current_ini.end()) {
        scene_metadata.insert(scene_it->second.begin(), scene_it->second.end());
    }
    scene_metadata["name"] = scene.name;
    if (!scene.core_version.empty()) scene_metadata["core_version"] = scene.core_version;
    sections["scene"] = {"[scene]"};
    for (const auto& [key, value] : scene_metadata) {
        sections["scene"].push_back(key + " = " + value);
    }
    sections["actors"] = build_actors_section_lines(scene);
    sections["sun"] = build_sun_section_lines(scene);
    sections["grid"] = build_grid_section_lines(scene);
    sections["camera"] = build_camera_section_lines(scene);
    sections["scripts"] = {"[scripts]", "path = " + scene.script_path};
    sections["terrain"] = {"[terrain]", "path = " + scene.terrain_path,
                           "type = " + scene.terrain_type};
    const auto storage = normalize_route(scene.vision_storage);
    const auto source_id = sanitize_vision_source_id(scene.vision_source_id);
    if (storage == "embedded" && !scene.vision_document_data.empty()) {
        sections["vision"] = {"[vision]", "import_mode = external", "storage = embedded"};
        sections["vision_document"] = {
            "[vision_document]",
            "version = " + (scene.vision_document_version.empty() ? std::string(VISION_DOCUMENT_VERSION)
                                                                    : scene.vision_document_version),
            "encoding = " + (scene.vision_document_encoding.empty() ? std::string(VISION_DOCUMENT_ENCODING)
                                                                      : scene.vision_document_encoding),
            "asset_root = " + scene.vision_document_asset_root,
            "data = " + scene.vision_document_data};
    } else if (storage == "project_sidecar" && !source_id.empty()) {
        sections["vision"] = {"[vision]", "import_mode = external", "source_id = " + source_id,
                              "storage = project_sidecar"};
        sections["vision_document"] = {};
    } else {
        sections["vision"] = {};
        sections["vision_document"] = {};
    }
    sections["vision_bindings"] = {};
    sections["vision_unsupported_shapes"] = {};
    SceneDocumentStore document_store(scene.project_root);
    std::vector<SceneFolders::Diagnostic> write_diagnostics;
    if (!document_store.replace_sections(sections, write_diagnostics)) {
        nlohmann::json details = nlohmann::json::array();
        for (const auto& item : write_diagnostics) {
            details.push_back({{"code", item.code}, {"message", item.message},
                               {"path", path_to_utf8(item.path)}, {"actor", item.actor},
                               {"field", item.field}});
        }
        throw PortableSceneValidationError(
            write_diagnostics.empty() ? "Unable to save portable scene"
                                      : write_diagnostics.front().message,
            std::move(details));
    }
}

bool migrate_project_sidecar_scene_to_embedded(NativeEditorScene& scene,
                                               const std::filesystem::path& scene_file) {
    if (scene.vision_storage != "project_sidecar" ||
        !scene.vision_document_data.empty()) {
        return false;
    }

    const auto sidecar_json = resolve_project_sidecar_vision_json(scene);
    if (sidecar_json.empty() || !std::filesystem::is_regular_file(sidecar_json)) {
        return false;
    }

    try {
        std::ifstream input(sidecar_json);
        if (!input) {
            return false;
        }
        const auto document = nlohmann::json::parse(input);
        const auto embedded = create_embedded_vision_document(scene.project_root, sidecar_json, document);

        scene.vision_storage = "embedded";
        scene.vision_source_id.clear();
        scene.vision_source_path.clear();
        scene.vision_import_mode = "external";
        scene.vision_document_version = VISION_DOCUMENT_VERSION;
        scene.vision_document_encoding = VISION_DOCUMENT_ENCODING;
        scene.vision_document_asset_root = embedded.asset_root;
        scene.vision_document_data = embedded.data;

        replace_ini_section_from_map(scene_file, "camera", vision_camera_section(embedded.document));
        persist_vision_proxy_actors_from_document(scene.project_root, scene_file, embedded.document, scene.project_root);
        persist_native_scene_vision_metadata(scene);
        persist_native_scene_vision_document(scene);
        return true;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("Vision sidecar migration failed: project={}, scene={}, error={}",
                      scene.project_root.string(),
                      scene.route,
                      e.what());
        return false;
    }
}

void apply_native_scene_environment(NativeEditorScene& scene) {
    if (!scene.environment) {
        return;
    }

    scene.environment->set_sun_direction(scene.sun_direction);
    scene.environment->set_floor_grid(scene.floor_grid_enabled);
    scene.environment->set_sun_intensity(scene.sun_enabled ? 10.0f : 0.0f);
    scene.environment->set_sky_intensity(scene.sun_enabled ? 20.0f : 0.0f);
}

void apply_native_scene_vision_source(NativeEditorScene& scene) {
    if (scene.vision_storage == "embedded") {
        if (scene.vision_document_data.empty()) {
            CFW_LOG_ERROR("Vision embedded scene missing: project={}, scene={}",
                          scene.project_root.string(),
                          scene.route);
            return;
        }
        try {
            auto document = decode_vision_document_data(scene.vision_document_data);
            const bool embedded_document_repaired =
                bind_missing_native_actor_materials(scene, document);
            if (embedded_document_repaired) {
                persist_embedded_vision_document(scene, document);
            }
            if (hydrate_native_actor_transforms_from_vision_document(scene, document)) {
                persist_native_scene_actors(scene);
            }
            const auto render_document = vision_document_for_render(document);
            const auto scene_key = embedded_vision_scene_key(scene);
            register_embedded_vision_actor_bindings(scene, render_document, scene_key);
            Corona::API::load_vision_scene_from_json(render_document.dump(),
                                                     path_to_utf8(scene.project_root),
                                                     scene_key,
                                                     true);
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("Vision embedded scene load failed: project={}, scene={}, error={}",
                          scene.project_root.string(),
                          scene.route,
                          e.what());
        }
        return;
    }

    clear_embedded_vision_actor_bindings(scene);
    if (scene.vision_storage == "project_sidecar") {
        const auto sidecar_json = resolve_project_sidecar_vision_json(scene);
        if (sidecar_json.empty() || !std::filesystem::is_regular_file(sidecar_json)) {
            CFW_LOG_ERROR("Vision sidecar scene missing: project={}, source_id={}",
                          scene.project_root.string(),
                          scene.vision_source_id);
            return;
        }
        Corona::API::load_vision_scene(path_to_utf8(sidecar_json));
        return;
    }

    const auto source_path = normalize_route(scene.vision_source_path);
    if (source_path.empty()) {
        Corona::API::load_vision_scene("");
        return;
    }

    const auto resolved = resolve_project_path(scene.project_root, source_path);
    Corona::API::load_vision_scene(path_to_utf8(resolved));
}

void apply_native_scene_vision_camera_defaults(NativeEditorScene& scene) {
    const bool has_sidecar = scene.vision_storage == "project_sidecar" &&
                             !sanitize_vision_source_id(scene.vision_source_id).empty();
    const bool has_embedded = scene.vision_storage == "embedded" &&
                              !scene.vision_document_data.empty();
    if ((!has_embedded && !has_sidecar && normalize_route(scene.vision_source_path).empty()) ||
        scene.cameras.empty()) {
        return;
    }
    auto& camera = scene.cameras[std::min(scene.active_camera_index, scene.cameras.size() - 1)];
    if (camera.engine_camera) {
        camera.engine_camera->set_render_backend("vision");
        camera.engine_camera->set_output_mode("final_color");
    }
}

std::filesystem::path resolve_native_actor_asset_path(const NativeEditorScene& scene,
                                                      NativeEditorActor& item) {
    if (item.actor_type != "actor" || to_lower_ascii(actor_file_extension(item.route)) != "actor") {
        return resolve_project_path(scene.project_root, item.route);
    }

    const auto actor_file = resolve_project_path(scene.project_root, item.route);
    const auto actor_ini = read_ini_file(actor_file);
    const auto model_route = normalize_route(ini_value(actor_ini, "base", "path"));
    if (item.name.empty()) {
        item.name = ini_value(actor_ini, "base", "name", stem_utf8(item.route));
    }
    if (item.actor_guid.empty()) {
        item.actor_guid = ini_value(actor_ini, "base", "actor_guid");
    }
    if (!item.follow_camera) {
        item.follow_camera = parse_bool(ini_value(actor_ini, "base", "follow_camera", "false"));
    }
    if (model_route.empty()) {
        throw std::runtime_error("Actor file missing [base].path: " + item.route);
    }
    return resolve_project_path(scene.project_root, model_route);
}

std::optional<std::string> optional_ini_string(const IniSection& section,
                                               const std::string& key) {
    const auto it = section.find(key);
    if (it == section.end() || trim_ascii(it->second).empty()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<float> optional_ini_float(const IniSection& section,
                                        const std::string& key) {
    const auto value = optional_ini_string(section, key);
    if (!value) {
        return std::nullopt;
    }
    return parse_float(*value, 0.0f);
}

std::optional<std::array<float, 3>> optional_ini_float3(const IniSection& section,
                                                        const std::string& key) {
    const auto value = optional_ini_string(section, key);
    if (!value) {
        return std::nullopt;
    }
    return parse_float3(*value, {0.0f, 0.0f, 0.0f});
}

NativeEditorActorOpticsState load_native_actor_optics_state(const IniSection& actors_section,
                                                            const std::string& actor_key) {
    NativeEditorActorOpticsState state;
    state.diffuse = optional_ini_float3(actors_section, actor_key + ".optics.diffuse");
    state.metallic = optional_ini_float(actors_section, actor_key + ".optics.metallic");
    state.roughness = optional_ini_float(actors_section, actor_key + ".optics.roughness");
    state.specular = optional_ini_float(actors_section, actor_key + ".optics.specular");
    state.shininess = optional_ini_float(actors_section, actor_key + ".optics.shininess");
    state.emission = optional_ini_float3(actors_section, actor_key + ".optics.emission");
    if (auto texture = optional_ini_string(actors_section, actor_key + ".material.texture")) {
        state.texture = *texture;
    }
    return state;
}

void apply_native_actor_optics_state(NativeEditorActor& item) {
    if (!item.optics) {
        return;
    }
    if (item.persisted_optics.diffuse) {
        item.optics->set_diffuse(*item.persisted_optics.diffuse);
    }
    if (item.persisted_optics.metallic) {
        item.optics->set_metallic(*item.persisted_optics.metallic);
    }
    if (item.persisted_optics.roughness) {
        item.optics->set_roughness(*item.persisted_optics.roughness);
    }
    if (item.persisted_optics.specular) {
        item.optics->set_specular(*item.persisted_optics.specular);
    }
    if (item.persisted_optics.shininess) {
        item.optics->set_shininess(*item.persisted_optics.shininess);
    }
}

NativeEditorActor& add_native_actor_to_scene(NativeEditorScene& scene,
                                             NativeEditorActor item,
                                             const std::filesystem::path& asset_path) {
    if (item.actor_type == "ui_image") {
        auto image_geometry = Corona::API::Geometry::from_image(path_to_utf8(asset_path));
        item.geometry = std::make_unique<Corona::API::Geometry>(std::move(image_geometry));
    } else if (item.actor_type == "audio") {
        // 音频物体：无网格，纯 transform 几何（空路径）。
        item.geometry = std::make_unique<Corona::API::Geometry>(std::string{});
    } else {
        item.geometry = std::make_unique<Corona::API::Geometry>(path_to_utf8(asset_path));
    }
    item.geometry->set_position(item.position);
    item.geometry->set_rotation(item.rotation);
    item.geometry->set_scale(item.scale);

    item.optics = std::make_unique<Corona::API::Optics>(*item.geometry);
    item.mechanics = std::make_unique<Corona::API::Mechanics>(*item.geometry);
    item.acoustics = std::make_unique<Corona::API::Acoustics>(*item.geometry);
    apply_native_actor_optics_state(item);
    if (item.actor_type == "ui_image") {
        item.optics->set_lighting_enabled(false);
        item.mechanics->set_physics_enabled(false);
        item.follow_camera = true;
    } else if (item.actor_type == "audio") {
        // 音频物体不渲染、不参与物理；绑定音频资源。
        item.optics->set_visible(false);
        item.mechanics->set_physics_enabled(false);
        if (item.audio_resource_id != 0) {
            item.acoustics->set_audio_resource(item.audio_resource_id);
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
    item.engine_actor->set_actor_guid(item.actor_guid);
    item.engine_actor->set_follow_camera(item.follow_camera);
    scene.engine_scene->add_actor(item.engine_actor.get());
    scene.actors.push_back(std::move(item));
    return scene.actors.back();
}

void load_native_actor(NativeEditorScene& scene,
                       const IniSection& actors_section,
                       const std::string& actor_key,
                       size_t index) {
    NativeEditorActor item;
    item.name = actors_section.contains(actor_key + ".name")
                    ? actors_section.at(actor_key + ".name")
                    : actor_key;
    item.actor_type = actors_section.contains(actor_key + ".actor_type")
                          ? actors_section.at(actor_key + ".actor_type")
                          : "actor";
    item.route = normalize_route(actors_section.contains(actor_key + ".route")
                                     ? actors_section.at(actor_key + ".route")
                                     : "");
    item.actor_guid = actors_section.contains(actor_key + ".actor_guid")
                          ? actors_section.at(actor_key + ".actor_guid")
                          : "";
    const auto actor_value = [&](const std::string& suffix, const std::string& fallback = "") {
        const auto it = actors_section.find(actor_key + suffix);
        return it == actors_section.end() ? fallback : it->second;
    };
    item.runtime_entity_id = actor_value(".runtime.entity_id");
    item.asset_id = actor_value(".runtime.asset_id");
    item.model_ref = actor_value(".runtime.model_ref");
    item.entity_type = actor_value(".runtime.entity_type");
    item.semantic_role = actor_value(".runtime.semantic_role");
    item.source_plan_id = actor_value(".runtime.source_plan_id");
    item.source_batch_id = actor_value(".runtime.source_batch_id");
    item.source_scene_version = std::max(
        parse_int(actor_value(".runtime.source_scene_version", "1"), 1),
        1);
    item.actor_version = std::max(
        parse_int(actor_value(".runtime.actor_version", "1"), 1),
        1);
    item.follow_camera = actors_section.contains(actor_key + ".follow_camera") &&
                         parse_bool(actors_section.at(actor_key + ".follow_camera"));
    item.position = parse_float3(
        actors_section.contains(actor_key + ".geometry.position")
            ? actors_section.at(actor_key + ".geometry.position")
            : "0.0, 0.0, 0.0",
        {0.0f, 0.0f, 0.0f});
    item.rotation = parse_float3(
        actors_section.contains(actor_key + ".geometry.rotation")
            ? actors_section.at(actor_key + ".geometry.rotation")
            : "0.0, 0.0, 0.0",
        {0.0f, 0.0f, 0.0f});
    item.scale = parse_float3(
        actors_section.contains(actor_key + ".geometry.scale")
            ? actors_section.at(actor_key + ".geometry.scale")
            : "1.0, 1.0, 1.0",
        {1.0f, 1.0f, 1.0f});
    item.persisted_optics = load_native_actor_optics_state(actors_section, actor_key);

    if (item.route.empty()) {
        return;
    }

    const auto asset_path = resolve_native_actor_asset_path(scene, item);
    if (item.actor_guid.empty()) {
        item.actor_guid = make_actor_guid(scene.route, item.name, index);
    }
    auto& actor = add_native_actor_to_scene(scene, std::move(item), asset_path);
    if (actor.optics && actors_section.contains(actor_key + ".optics.visible")) {
        actor.optics->set_visible(
            parse_bool(actors_section.at(actor_key + ".optics.visible"), true));
    }
    if (actor.actor_type != "ui_image" && actors_section.contains(actor_key + ".mechanics.physics_enabled")) {
        actor.mechanics->set_physics_enabled(
            parse_bool(actors_section.at(actor_key + ".mechanics.physics_enabled"), true));
    }
    if (actor.actor_type != "ui_image" && actor.mechanics) {
        if (actors_section.contains(actor_key + ".mechanics.collision_type")) {
            actor.mechanics->set_collision_shape(normalize_collision_type(
                actors_section.at(actor_key + ".mechanics.collision_type")));
        } else if (actors_section.contains(actor_key + ".mechanics.collision_enabled")) {
            actor.mechanics->set_collision_shape(
                parse_bool(actors_section.at(actor_key + ".mechanics.collision_enabled"), true)
                    ? "box" : "none");
        } else {
            actor.mechanics->set_collision_shape("box");
        }
    }
}

NativeEditorCamera make_native_camera(NativeEditorScene& scene,
                                      const IniSection* camera_section,
                                      int index) {
    const std::string prefix = "camera" + std::to_string(index);
    const auto section_value = [&](const std::string& key, const std::string& fallback) {
        if (!camera_section) {
            return fallback;
        }
        const auto it = camera_section->find(prefix + "." + key);
        return it == camera_section->end() ? fallback : it->second;
    };

    NativeEditorCamera item;
    item.name = section_value("name", scene.name + (index == 0 ? "_MainCamera" : "_Camera" + std::to_string(index)));
    item.camera_id = section_value("id", scene.route + "#camera" + std::to_string(index));
    item.deletable = parse_bool(section_value("deletable", index == 0 ? "false" : "true"), index != 0);
    auto position = parse_float3(section_value("position", "0.0, 0.0, -5.0"), {0.0f, 0.0f, -5.0f});
    auto forward = parse_float3(section_value("forward", "0.0, 0.0, 1.0"), {0.0f, 0.0f, 1.0f});
    auto world_up = parse_float3(section_value("world_up", "0.0, 1.0, 0.0"), {0.0f, 1.0f, 0.0f});
    const float fov = parse_float(section_value("fov", "45.0"), 45.0f);
    item.width = parse_int(section_value("width", "1920"), 1920);
    item.height = parse_int(section_value("height", "1080"), 1080);
    item.view_open = parse_bool(section_value("view_open", "false"));
    item.view_x = parse_int(section_value("view_x", "120"), 120);
    item.view_y = parse_int(section_value("view_y", "120"), 120);
    item.view_width = parse_int(section_value("view_width", "960"), 960);
    item.view_height = parse_int(section_value("view_height", "540"), 540);
    item.move_speed = parse_float(section_value("move_speed", "1.0"), 1.0f);
    item.vision_spp = section_value("vision_spp", "");
    item.vision_max_depth = section_value("vision_max_depth", "");
    item.vision_denoise = section_value("vision_denoise", "");

    item.engine_camera = std::make_unique<Corona::API::Camera>(position, forward, world_up, fov);
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode(section_value("output_mode", "final_color"));
    item.engine_camera->set_render_backend(section_value("render_backend", "native"));
    item.engine_camera->set_vision_render_mode(section_value("vision_render_mode", "path_tracing"));
    item.engine_camera->set_ssao_enabled(parse_bool(section_value("ssao_enabled", "true"), true));
    item.engine_camera->set_view_state(item.view_open, item.view_x, item.view_y,
                                       item.view_width, item.view_height, item.move_speed);
    if (index > 0) {
        item.engine_camera->set_offscreen_capture_mode(true);
    }
    return item;
}

std::unique_ptr<NativeEditorScene> load_native_scene_from_ini_legacy(
    const std::filesystem::path& project_root,
    const std::string& scene_route) {
    const auto scene_file = resolve_project_path(project_root, scene_route);
    auto scene_ini = read_ini_file(scene_file);

    auto scene = std::make_unique<NativeEditorScene>();
    scene->project_root = project_root;
    scene->route = scene_route;
    scene->name = ini_value(scene_ini, "scene", "name",
                            ini_value(scene_ini, "base", "name", stem_utf8(scene_route)));
    scene->core_version = ini_value(scene_ini, "scene", "core_version");
    scene->script_path = ini_value(scene_ini, "scripts", "path");
    scene->terrain_type = ini_value(scene_ini, "terrain", "type");
    scene->terrain_path = ini_value(scene_ini, "terrain", "path");
    scene->vision_storage = ini_value(scene_ini, "vision", "storage");
    scene->vision_source_id = ini_value(scene_ini, "vision", "source_id");
    scene->vision_source_path = ini_value(scene_ini, "vision", "source_path");
    scene->vision_import_mode = ini_value(scene_ini, "vision", "import_mode");
    scene->vision_document_version = ini_value(scene_ini, "vision_document", "version");
    scene->vision_document_encoding = ini_value(scene_ini, "vision_document", "encoding");
    scene->vision_document_data = ini_value(scene_ini, "vision_document", "data");
    scene->vision_document_asset_root = ini_value(scene_ini, "vision_document", "asset_root");
    if (detect_scene_folder(project_root) &&
        migrate_project_sidecar_scene_to_embedded(*scene, scene_file)) {
        scene_ini = read_ini_file(scene_file);
    }
    scene->sun_direction = parse_float3(
        ini_value(scene_ini, "sun", "sun_direction", "1.0, 1.0, 1.0"),
        {1.0f, 1.0f, 1.0f});
    scene->sun_enabled = parse_bool(ini_value(scene_ini, "sun", "enabled", "true"), true);
    scene->floor_grid_enabled = parse_bool(ini_value(scene_ini, "grid", "enabled", "true"), true);

    scene->engine_scene = std::make_unique<Corona::API::Scene>();
    scene->environment = std::make_unique<Corona::API::Environment>();
    apply_native_scene_environment(*scene);
    scene->engine_scene->set_environment(scene->environment.get());

    const auto actors_it = scene_ini.find("actors");
    if (actors_it != scene_ini.end()) {
        std::vector<std::string> actor_keys;
        for (const auto& [key, value] : actors_it->second) {
            const auto dot = key.find('.');
            if (dot != std::string::npos) {
                actor_keys.push_back(key.substr(0, dot));
            }
        }
        std::sort(actor_keys.begin(), actor_keys.end());
        actor_keys.erase(std::unique(actor_keys.begin(), actor_keys.end()), actor_keys.end());
        size_t index = 0;
        for (const auto& actor_key : actor_keys) {
            try {
                load_native_actor(*scene, actors_it->second, actor_key, index++);
            } catch (const std::exception& e) {
                std::cerr << "Native scene actor load skipped: " << actor_key
                          << " (" << e.what() << ")" << std::endl;
            }
        }
    }

    const auto camera_it = scene_ini.find("camera");
    int camera_count = 0;
    if (camera_it != scene_ini.end()) {
        camera_count = parse_int(camera_it->second.contains("count") ? camera_it->second.at("count") : "0", 0);
        if (camera_count <= 0) {
            for (const auto& [key, value] : camera_it->second) {
                if (key.rfind("camera", 0) == 0) {
                    const auto dot = key.find('.');
                    if (dot > 6) {
                        camera_count = std::max(camera_count, parse_int(key.substr(6, dot - 6), -1) + 1);
                    }
                }
            }
        }
    }
    if (camera_count <= 0) {
        camera_count = 1;
    }
    for (int index = 0; index < camera_count; ++index) {
        scene->cameras.push_back(make_native_camera(*scene, camera_it == scene_ini.end() ? nullptr : &camera_it->second, index));
        scene->engine_scene->add_camera(scene->cameras.back().engine_camera.get());
    }
    scene->active_camera_index = 0;
    if (camera_it != scene_ini.end()) {
        const auto active_id = camera_it->second.contains("active_id") ? camera_it->second.at("active_id") : "";
        for (size_t index = 0; index < scene->cameras.size(); ++index) {
            if (scene->cameras[index].camera_id == active_id || scene->cameras[index].name == active_id) {
                scene->active_camera_index = index;
                break;
            }
        }
    }
    if (!scene->cameras.empty()) {
        scene->engine_scene->set_active_camera(scene->cameras[scene->active_camera_index].engine_camera.get());
    }
    apply_native_scene_vision_camera_defaults(*scene);
    scene->engine_scene->set_simulation_enabled(true);
    scene->engine_scene->set_enabled(true);
    apply_native_scene_vision_source(*scene);
    return scene;
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
        validate_snapshot_float3(transform.value("position", nlohmann::json()),
                                 "actor.transform.position");
        validate_snapshot_float3(transform.value("rotation", nlohmann::json()),
                                 "actor.transform.rotation");
        validate_snapshot_float3(transform.value("scale", nlohmann::json()),
                                 "actor.transform.scale");
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
    }
}

NativeEditorActor native_actor_from_snapshot(const nlohmann::json& actor_data) {
    NativeEditorActor item;
    item.persisted_snapshot = actor_data;
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
    item.resolved_asset_path = actor_data.value("asset_path", std::string{});
    const auto transform = actor_data.value("transform", nlohmann::json::object());
    item.position = snapshot_float3(transform.value("position", nlohmann::json::array()),
                                    {0.0f, 0.0f, 0.0f});
    item.rotation = snapshot_float3(transform.value("rotation", nlohmann::json::array()),
                                    {0.0f, 0.0f, 0.0f});
    item.scale = snapshot_float3(transform.value("scale", nlohmann::json::array()),
                                 {1.0f, 1.0f, 1.0f});
    item.persisted_visible = actor_data.value("visible", true);
    const auto mechanics = actor_data.value("mechanics", nlohmann::json::object());
    item.persisted_physics_enabled = mechanics.value("physics_enabled", true);
    item.persisted_collision_type = normalize_collision_type(
        mechanics.value("collision_type", std::string{"box"}));
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
        ".actor", ".dae", ".fbx", ".glb", ".gltf", ".obj", ".usd", ".usda", ".usdc"};
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
            if (actor.optics) actor.optics->set_visible(actor.persisted_visible);
            if (actor.mechanics && actor.actor_type != "ui_image") {
                actor.mechanics->set_physics_enabled(actor.persisted_physics_enabled);
                actor.mechanics->set_collision_shape(actor.persisted_collision_type);
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
    if (placeholder.optics) placeholder.optics->set_visible(false);
    if (placeholder.mechanics) {
        placeholder.mechanics->set_physics_enabled(false);
        placeholder.mechanics->set_collision_shape("none");
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
    item.vision_denoise = camera_data.value("vision_denoise", std::string{});
    const auto position = snapshot_float3(camera_data.value("position", nlohmann::json::array()),
                                          {0.0f, 0.0f, -5.0f});
    const auto forward = snapshot_float3(camera_data.value("forward", nlohmann::json::array()),
                                         {0.0f, 0.0f, 1.0f});
    const auto world_up = snapshot_float3(camera_data.value("world_up", nlohmann::json::array()),
                                          {0.0f, 1.0f, 0.0f});
    item.engine_camera = std::make_unique<Corona::API::Camera>(
        position, forward, world_up, camera_data.value("fov", 45.0f));
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode(camera_data.value("output_mode", std::string{"final_color"}));
    item.engine_camera->set_render_backend(camera_data.value("render_backend", std::string{"native"}));
    item.engine_camera->set_vision_render_mode(
        camera_data.value("vision_render_mode", std::string{"path_tracing"}));
    item.engine_camera->set_ssao_enabled(camera_data.value("ssao_enabled", true));
    item.engine_camera->set_view_state(item.view_open, item.view_x, item.view_y,
                                       item.view_width, item.view_height, item.move_speed);
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
    scene.project_root = path_from_utf8(snapshot.at("project_root").get<std::string>());
    scene.route = data.value("route", std::string{});
    scene.name = data.value("name", stem_utf8(scene.route));
    scene.core_version = data.value("core_version", std::string{});
    const auto scripts = data.value("scripts", nlohmann::json::object());
    const auto terrain = data.value("terrain", nlohmann::json::object());
    const auto environment = data.value("environment", nlohmann::json::object());
    const auto vision = data.value("vision", nlohmann::json::object());
    scene.script_path = scripts.value("path", std::string{});
    scene.terrain_type = terrain.value("type", std::string{});
    scene.terrain_path = terrain.value("path", std::string{});
    scene.sun_enabled = environment.value("sun_enabled", true);
    scene.sun_direction = snapshot_float3(
        environment.value("sun_direction", nlohmann::json::array()), {1.0f, 1.0f, 1.0f});
    scene.floor_grid_enabled = environment.value("floor_grid_enabled", true);
    scene.vision_storage = vision.value("storage", std::string{});
    scene.vision_source_id = vision.value("source_id", std::string{});
    scene.vision_source_path = vision.value("source_path", std::string{});
    scene.vision_import_mode = vision.value("import_mode", std::string{});
    scene.vision_document_version = vision.value("document_version", std::string{});
    scene.vision_document_encoding = vision.value("document_encoding", std::string{});
    scene.vision_document_data = vision.value("document_data", std::string{});
    scene.vision_document_asset_root = vision.value("document_asset_root", std::string{});
    try {
        scene.engine_scene = std::make_unique<Corona::API::Scene>();
        scene.environment = std::make_unique<Corona::API::Environment>();
        apply_native_scene_environment(scene);
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
        apply_native_scene_vision_camera_defaults(scene);
        scene.engine_scene->set_enabled(true);

        for (const auto& actor_data : data.at("actors")) {
            materialize_actor_snapshot(scene, actor_data, allow_degraded, diagnostics);
        }
        scene.engine_scene->set_simulation_enabled(true);
        apply_native_scene_vision_source(scene);
        scene.load_diagnostics = diagnostics;
        state.project_path = path_to_utf8(scene.project_root);
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
    return invoke_python_script_service(parser_request, "python-archive");
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
    prepared.diagnostics = data.value("diagnostics", nlohmann::json::array());
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
    if (!prepared.diagnostics.empty() && load_policy != "degraded") {
        prepared.status = "decision_required";
    } else {
        prepared.status = prepared.diagnostics.empty() ? "opened" : "opened_degraded";
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

NativeEditorScene* reload_native_editor_scene(const std::string& project_path_arg,
                                              const std::string& scene_route_arg) {
    auto& state = native_editor_state();
    const auto project_path = normalize_route(project_path_arg.empty()
                                                  ? resolve_active_project_path(nlohmann::json::array())
                                                  : project_path_arg);
    if (project_path.empty()) {
        throw std::runtime_error("No active project path for native editor reload");
    }

    (void)scene_route_arg;
    auto prepared = prepare_archive_load(project_path, "prompt");
    if (!prepared.service_ok) {
        throw std::runtime_error(prepared.error.empty() ? "Archive reload failed" : prepared.error);
    }
    if (!prepared.ready) {
        throw std::runtime_error("Archive reload requires user decision");
    }
    return &materialize_scene_snapshot_into_state(
        state, prepared.snapshot, false, prepared.diagnostics);
}

bool native_scene_request_matches(const NativeEditorScene& scene,
                                  const std::string& scene_route_arg) {
    const auto requested = normalize_route(scene_route_arg);
    if (requested.empty()) {
        return true;
    }

    const auto requested_lower = to_lower_ascii(requested);
    const auto route = normalize_route(scene.route);
    const auto route_path = path_from_utf8(route);
    const auto requested_path = path_from_utf8(requested);
    return requested_lower == to_lower_ascii(route)
        || requested_lower == to_lower_ascii(scene.name)
        || requested_lower == to_lower_ascii(path_to_utf8(route_path.filename()))
        || requested_lower == to_lower_ascii(path_to_utf8(route_path.stem()))
        || to_lower_ascii(path_to_utf8(requested_path.filename()))
            == to_lower_ascii(path_to_utf8(route_path.filename()));
}

NativeEditorScene* resolve_native_editor_scene_request(NativeEditorScene* scene,
                                                       const std::string& scene_route_arg) {
    if (scene == nullptr || native_scene_request_matches(*scene, scene_route_arg)) {
        return scene;
    }

    const auto requested = normalize_route(scene_route_arg);
    const auto candidate = resolve_project_path(scene->project_root, requested);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec)
        || ec
        || to_lower_ascii(path_to_utf8(candidate.extension())) != ".scene") {
        // Runtime scene_name is a semantic label, not necessarily a file route.
        // Invalid aliases must remain read-only context and must never reload the
        // live native scene.
        return scene;
    }

    return reload_native_editor_scene(
        "",
        route_for_project_storage(scene->project_root, path_to_utf8(candidate)));
}

NativeEditorScene* scene_for_request_route(const NativeRequest& request, std::size_t scene_arg_index = 0) {
    auto* scene = ensure_native_editor_scene();
    const auto scene_route = normalize_route(arg_string(request.args, scene_arg_index));
    return resolve_native_editor_scene_request(scene, scene_route);
}

nlohmann::json make_on_init_payload(const NativeEditorScene& scene) {
    return {
        {"scenes", nlohmann::json::array({{
            {"path", scene.route},
            {"name", scene.name},
        }})},
        {"active_index", 0},
        {"path", scene.route},
        {"name", scene.name},
        {"single_scene", true},
    };
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

nlohmann::json camera_to_json(const NativeEditorCamera& camera) {
    nlohmann::json item;
    item["id"] = camera.camera_id;
    item["camera_id"] = camera.camera_id;
    item["name"] = camera.name;
    item["handle"] = camera.engine_camera ? camera.engine_camera->get_handle() : 0;
    item["position"] = camera.engine_camera ? camera.engine_camera->get_position() : std::array<float, 3>{0.0f, 0.0f, -5.0f};
    item["forward"] = camera.engine_camera ? camera.engine_camera->get_forward() : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    item["world_up"] = camera.engine_camera ? camera.engine_camera->get_world_up() : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    item["fov"] = camera.engine_camera ? camera.engine_camera->get_fov() : 45.0f;
    item["width"] = camera.width;
    item["height"] = camera.height;
    item["output_mode"] = camera.engine_camera ? camera.engine_camera->get_output_mode() : "final_color";
    item["render_backend"] = camera.engine_camera ? camera.engine_camera->get_render_backend() : "native";
    item["vision_render_mode"] = camera.engine_camera ? camera.engine_camera->get_vision_render_mode() : "path_tracing";
    item["vision_spp"] = camera.vision_spp;
    item["vision_max_depth"] = camera.vision_max_depth;
    item["vision_denoise"] = camera.vision_denoise;
    item["shadow_cascade_debug"] = camera.engine_camera ? camera.engine_camera->get_shadow_cascade_debug() : false;
    item["ssao_enabled"] = camera.engine_camera ? camera.engine_camera->get_ssao_enabled() : true;
    item["move_speed"] = camera.move_speed;
    item["view_open"] = camera.view_open;
    item["view_x"] = camera.view_x;
    item["view_y"] = camera.view_y;
    item["view_width"] = camera.view_width;
    item["view_height"] = camera.view_height;
    item["deletable"] = camera.deletable;
    return item;
}

std::optional<std::array<float, 6>> native_actor_local_aabb(const NativeEditorActor& actor);
std::optional<std::array<float, 6>> native_actor_world_aabb(const NativeEditorActor& actor);
nlohmann::json aabb_to_json(const std::array<float, 6>& aabb);

nlohmann::json actor_to_json(const NativeEditorScene& scene, const NativeEditorActor& actor) {
    nlohmann::json item;
    item["name"] = actor.name;
    item["actor_guid"] = actor.actor_guid;
    item["handle"] = actor.engine_actor ? actor.engine_actor->get_handle() : 0;
    item["path"] = actor.route;
    item["route"] = actor.route;
    item["scene"] = scene.route;
    item["type"] = actor_file_extension(actor.route);
    item["model"] = actor.route;
    item["model_dependencies"] = nlohmann::json::array();
    item["actor_type"] = actor.actor_type;
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
    item["collision"] = actor.load_status == ActorLoadStatus::Loaded && actor.mechanics
                            ? collision_shape_name(*actor.mechanics)
                            : actor.persisted_collision_type;
    item["visible"] = actor.load_status == ActorLoadStatus::Loaded && actor.optics
                          ? actor.optics->get_visible()
                          : actor.persisted_visible;
    item["script"] = "";
    item["follow_camera"] = actor.follow_camera;
    item["render_space"] = actor.follow_camera ? "ui" : "scene";
    item["geometry"] = {
        {"position", actor.geometry ? actor.geometry->get_position() : actor.position},
        {"rotation", actor.geometry ? actor.geometry->get_rotation() : actor.rotation},
        {"scale", actor.geometry ? actor.geometry->get_scale() : actor.scale},
    };
    const auto local_aabb = native_actor_local_aabb(actor);
    const auto world_aabb = native_actor_world_aabb(actor);
    item["local_aabb"] = local_aabb ? aabb_to_json(*local_aabb) : nlohmann::json(nullptr);
    item["world_aabb"] = world_aabb ? aabb_to_json(*world_aabb) : nlohmann::json(nullptr);
    item["aabb"] = item["world_aabb"];
    item["bounds_ready"] = static_cast<bool>(world_aabb);
    const auto render_status = actor.geometry
        ? actor.geometry->get_render_status()
        : Corona::API::GeometryRenderStatus{};
    item["render_status_observed"] = render_status.observed;
    item["render_ready"] = render_status.ready;
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
        const auto [linear_x, linear_y, linear_z] = actor.mechanics->get_linear_lock();
        const auto [angular_x, angular_y, angular_z] = actor.mechanics->get_angular_lock();
        item["mechanics"] = {
            {"mass", actor.mechanics->get_mass()},
            {"restitution", actor.mechanics->get_restitution()},
            {"damping", actor.mechanics->get_damping()},
            {"physics_enabled", actor.load_status == ActorLoadStatus::Loaded
                                    ? actor.mechanics->get_physics_enabled()
                                    : actor.persisted_physics_enabled},
            {"linear_lock", {linear_x, linear_y, linear_z}},
            {"angular_lock", {angular_x, angular_y, angular_z}},
        };
    }
    if (actor.optics) {
        item["optics"] = {
            {"diffuse", actor.optics->get_diffuse()},
            {"metallic", actor.optics->get_metallic()},
            {"roughness", actor.optics->get_roughness()},
            {"specular", actor.optics->get_specular()},
            {"shininess", actor.optics->get_shininess()},
        };
        if (actor.persisted_optics.emission) {
            item["optics"]["emission"] = *actor.persisted_optics.emission;
        }
    }
    if (!actor.persisted_optics.texture.empty()) {
        item["material"] = {{"texture", actor.persisted_optics.texture}};
    }
    item["camera_lock"] = {
        {"lock_to_camera", false},
        {"position_offset", {0.0f, 0.0f, 2.0f}},
        {"rotation_offset", {0.0f, 0.0f, 0.0f}},
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
        {"vision", {
            {"storage", scene.vision_storage},
            {"source_id", scene.vision_source_id},
            {"source_path", scene.vision_source_path},
            {"import_mode", scene.vision_import_mode},
            {"bindings", nlohmann::json::array()},
            {"unsupported_shapes", nlohmann::json::array()},
        }},
        {"script", scene.script_path},
        {"actors", actors},
    };
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

NativeEditorCamera& create_native_camera_view(NativeEditorScene& scene, const std::string& requested_name) {
    NativeEditorCamera item;
    item.name = unique_camera_name(scene, requested_name);
    item.camera_id = scene.route + "#" + item.name;
    item.deletable = true;
    item.width = 1920;
    item.height = 1080;
    item.view_open = true;
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
        position = source->engine_camera->get_position();
        forward = source->engine_camera->get_forward();
        world_up = source->engine_camera->get_world_up();
        fov = source->engine_camera->get_fov();
        item.width = source->width;
        item.height = source->height;
        item.move_speed = source->move_speed;
    }

    item.engine_camera = std::make_unique<Corona::API::Camera>(position, forward, world_up, fov);
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode("final_color");
    item.engine_camera->set_render_backend("native");
    item.engine_camera->set_vision_render_mode("path_tracing");
    item.engine_camera->set_ssao_enabled(true);
    item.engine_camera->set_view_state(item.view_open, item.view_x, item.view_y,
                                       item.view_width, item.view_height, item.move_speed);
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

std::optional<std::array<float, 6>> native_actor_local_aabb(const NativeEditorActor& actor) {
    if (!actor.geometry) {
        return std::nullopt;
    }
    const auto local = actor.geometry->get_aabb();
    if (local.size() < 6) {
        return std::nullopt;
    }
    std::array<float, 6> result{};
    for (size_t index = 0; index < 6; ++index) {
        result[index] = local[index];
    }
    if (!aabb_has_usable_extent(result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::array<float, 6>> native_actor_world_aabb(const NativeEditorActor& actor) {
    const auto local = native_actor_local_aabb(actor);
    if (!local || !actor.geometry) {
        return std::nullopt;
    }
    const auto position = actor.geometry->get_position();
    const auto scale = actor.geometry->get_scale();
    std::array<float, 6> result{};
    for (size_t axis = 0; axis < 3; ++axis) {
        const auto a = position[axis] + (*local)[axis] * scale[axis];
        const auto b = position[axis] + (*local)[axis + 3] * scale[axis];
        result[axis] = std::min(a, b);
        result[axis + 3] = std::max(a, b);
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

std::optional<std::array<float, 6>> native_scene_world_aabb(const NativeEditorScene& scene) {
    std::optional<std::array<float, 6>> aggregate;
    for (const auto& actor : scene.actors) {
        const auto actor_aabb = native_actor_world_aabb(actor);
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

float json_float_value(const nlohmann::json& object, const char* key, float fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        return fallback;
    }
    if (it->is_number()) {
        return it->get<float>();
    }
    if (it->is_string()) {
        try {
            return std::stof(it->get<std::string>());
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

NativeEditorCamera* ensure_native_editor_camera(NativeEditorScene& scene,
                                                const std::string& requested_name,
                                                const nlohmann::json& camera_data) {
    const auto camera_name = trim_ascii(requested_name.empty()
                                            ? json_string_value(camera_data, {"camera_name", "name"})
                                            : requested_name);
    auto* existing = find_native_camera(scene, camera_name);
    if (existing) {
        return existing;
    }

    const auto name = camera_name.empty() ? "vlm_review_camera" : camera_name;
    NativeEditorCamera item;
    item.name = name;
    item.camera_id = json_string_value(camera_data, {"camera_id", "id"});
    if (item.camera_id.empty()) {
        item.camera_id = scene.route + "#" + name;
    }
    item.deletable = parse_bool(json_string_value(camera_data, {"deletable"}), false);
    item.width = std::max(json_int_value(camera_data, "width", 512), 1);
    item.height = std::max(json_int_value(camera_data, "height", 512), 1);
    item.view_open = false;
    item.view_width = item.width;
    item.view_height = item.height;
    item.move_speed = json_float_value(camera_data, "move_speed", 1.0f);

    const auto position = camera_data.contains("position")
                              ? json_float3_value(camera_data["position"]).value_or(std::array<float, 3>{0.0f, 0.0f, -5.0f})
                              : std::array<float, 3>{0.0f, 0.0f, -5.0f};
    const auto forward = camera_data.contains("forward")
                             ? json_float3_value(camera_data["forward"]).value_or(std::array<float, 3>{0.0f, 0.0f, 1.0f})
                             : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    const auto world_up = camera_data.contains("world_up")
                              ? json_float3_value(camera_data["world_up"]).value_or(std::array<float, 3>{0.0f, 1.0f, 0.0f})
                              : std::array<float, 3>{0.0f, 1.0f, 0.0f};
    const auto fov = json_float_value(camera_data, "fov", 45.0f);

    item.engine_camera = std::make_unique<Corona::API::Camera>(position, forward, world_up, fov);
    item.engine_camera->set_size(item.width, item.height);
    item.engine_camera->set_output_mode(json_string_value(camera_data, {"output_mode"}).empty()
                                            ? "base_color"
                                            : json_string_value(camera_data, {"output_mode"}));
    item.engine_camera->set_render_backend(json_string_value(camera_data, {"render_backend"}).empty()
                                               ? "native"
                                               : json_string_value(camera_data, {"render_backend"}));
    item.engine_camera->set_vision_render_mode(json_string_value(camera_data, {"vision_render_mode"}).empty()
                                                   ? "path_tracing"
                                                   : json_string_value(camera_data, {"vision_render_mode"}));
    item.engine_camera->set_ssao_enabled(json_bool_value(camera_data, "ssao_enabled", true));
    item.engine_camera->set_view_state(false, item.view_x, item.view_y,
                                       item.view_width, item.view_height, item.move_speed);
    item.engine_camera->set_offscreen_capture_mode(true);
    item.engine_camera->set_surface(0);

    scene.cameras.push_back(std::move(item));
    auto& camera = scene.cameras.back();
    if (scene.engine_scene && camera.engine_camera) {
        scene.engine_scene->add_camera(camera.engine_camera.get());
    }
    return &camera;
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

float arg_float_value(const nlohmann::json& args, size_t index, float fallback = 0.0f) {
    if (!args.is_array() || index >= args.size()) {
        return fallback;
    }
    const auto& value = args[index];
    try {
        if (value.is_number()) {
            return value.get<float>();
        }
        if (value.is_string()) {
            return std::stof(value.get<std::string>());
        }
    } catch (...) {
    }
    return fallback;
}

std::string json_string_at(const nlohmann::json& value,
                           size_t index,
                           std::string fallback = {}) {
    if (!value.is_array() || index >= value.size()) {
        return fallback;
    }
    if (value[index].is_string()) {
        return value[index].get<std::string>();
    }
    return value[index].dump();
}

void emit_scene_tree_changed(const std::string& scene_route);

NativeResult create_native_editor_actor(const std::string& scene_route_arg,
                                        const std::string& source_path,
                                        std::string actor_type,
                                        const nlohmann::json& actor_data) {
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
        if (auto position = actor_data_float3(actor_data, "position")) {
            target.position = *position;
            if (target.geometry) {
                target.geometry->set_position(*position);
            }
        }
        if (auto rotation = actor_data_float3(actor_data, "rotation")) {
            target.rotation = *rotation;
            if (target.geometry) {
                target.geometry->set_rotation(*rotation);
            }
        }
        if (auto scale = actor_data_float3(actor_data, "scale")) {
            target.scale = *scale;
            if (target.geometry) {
                target.geometry->set_scale(*scale);
            }
        }
        if (auto follow_camera = actor_data_bool(actor_data, {"follow_camera"})) {
            target.follow_camera = *follow_camera;
        }
        if (auto physics_enabled = actor_data_bool(actor_data, {"physics_enabled"})) {
            if (target.mechanics) {
                target.mechanics->set_physics_enabled(*physics_enabled);
            }
        }
        apply_runtime_metadata(target);
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
                persist_native_scene_actors(*scene);
                emit_scene_tree_changed(scene->route);
            }
            return native_success({{"status", "success"}, {"scene", scene->route},
                                   {"actor", actor_to_json(*scene, *existing)}, {"existed", true}});
        }
    }

    NativeEditorActor item;
    item.actor_type = actor_type;
    std::optional<SceneFolders::SceneAssetStore> portable_store;
    if (detect_scene_folder(scene->project_root)) {
        portable_store.emplace(scene->project_root);
        const auto source = resolve_project_path(scene->project_root, source_path);
        SceneFolders::ImportResult imported;
        if (item.actor_type == "ui_image") {
            imported = portable_store->import_file(source, "Images");
        } else if (item.actor_type == "audio") {
            imported = portable_store->import_file(source, "Audio");
        } else if (item.actor_type == "actor" &&
                   to_lower_ascii(source.extension().string()) == ".actor") {
            const auto actor_ini = read_ini_file(source);
            const auto model_route = ini_value(actor_ini, "base", "path");
            auto model_source = path_from_utf8(model_route);
            if (!model_source.is_absolute()) {
                const auto beside_actor = source.parent_path() / model_source;
                model_source = std::filesystem::is_regular_file(beside_actor)
                                   ? beside_actor
                                   : scene->project_root / model_source;
            }
            imported = portable_store->import_actor(source, model_source);
        } else {
            imported = portable_store->import_model(source);
        }
        if (!imported.ok()) {
            if (imported.diagnostics.empty()) {
                return native_failure("Portable asset import failed without diagnostics", 2);
            }
            const auto& diagnostic = imported.diagnostics.front();
            return native_failure("Portable asset import failed: " + diagnostic.message +
                                  " (" + path_to_utf8(diagnostic.path) + ")", 2);
        }
        item.route = imported.main_route;
        if (!portable_store->write_manifest()) {
            return native_failure("Portable asset manifest update failed", 2);
        }
        const auto validation = portable_store->validate_manifest();
        if (!validation.empty()) {
            return native_failure("Portable asset manifest validation failed: " +
                                  validation.front().message, 2);
        }
    } else {
        item.route = route_for_project_storage(scene->project_root, source_path);
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

    if (item.actor_type == "actor" && to_lower_ascii(actor_file_extension(item.route)) == "actor") {
        const auto actor_file = resolve_project_path(scene->project_root, item.route);
        const auto actor_ini = read_ini_file(actor_file);
        const auto configured_name = ini_value(actor_ini, "base", "name");
        if (preferred_name.empty() && !configured_name.empty()) {
            item.name = unique_actor_name(*scene, configured_name);
            item.actor_guid = preferred_guid.empty()
                ? make_actor_guid(scene->route, item.name, scene->actors.size())
                : preferred_guid;
        }
        item.follow_camera = parse_bool(ini_value(actor_ini, "base", "follow_camera", "false"));
        item.position = parse_float3(
            ini_value(actor_ini, "geometry", "position", "0.0, 0.0, 0.0"),
            {0.0f, 0.0f, 0.0f});
        item.rotation = parse_float3(
            ini_value(actor_ini, "geometry", "rotation", "0.0, 0.0, 0.0"),
            {0.0f, 0.0f, 0.0f});
        item.scale = parse_float3(
            ini_value(actor_ini, "geometry", "scale", "1.0, 1.0, 1.0"),
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

    auto asset_path = resolve_native_actor_asset_path(*scene, item);
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
                actor.geometry->set_position(position);
            }
        }
    }
    if (auto physics_enabled = actor_data_bool(actor_data, {"physics_enabled"})) {
        if (actor.mechanics) {
            actor.mechanics->set_physics_enabled(*physics_enabled);
        }
    }
    sync_native_actor_to_embedded_vision_document(*scene, actor, true);
    persist_native_scene_actors(*scene);
    emit_scene_tree_changed(scene->route);
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

    const auto removed_name = it->name;
    const auto removed_guid = it->actor_guid;
    if (scene->engine_scene && it->engine_actor) {
        scene->engine_scene->remove_actor(it->engine_actor.get());
    }
    scene->actors.erase(it);
    remove_native_actor_from_embedded_vision_document(*scene, removed_guid);
    persist_native_scene_actors(*scene);
    emit_scene_tree_changed(scene->route);

    return native_success({
        {"status", "success"},
        {"scene", scene->route},
        {"actor", removed_name},
        {"actor_guid", removed_guid},
    });
}

NativeResult set_native_editor_actor_transform(const std::string& scene_route_arg,
                                               const std::string& actor_name,
                                               const nlohmann::json& transform_data) {
    if (trim_ascii(actor_name).empty()) {
        return native_failure("Actor name cannot be empty", 2);
    }
    auto* scene = ensure_native_editor_scene();
    const auto scene_route = normalize_route(scene_route_arg);
    scene = resolve_native_editor_scene_request(scene, scene_route);

    auto* actor = find_native_actor(*scene, actor_name);
    if (!actor && actor_name.rfind("__shell_", 0) != 0) {
        actor = find_native_actor(*scene, "__shell_" + actor_name);
    }
    if (!actor) {
        return native_failure("Actor not found: " + actor_name, 2);
    }

    bool persist_transform = true;
    if (const auto persist_it = transform_data.find("persist");
        persist_it != transform_data.end() && persist_it->is_boolean()) {
        persist_transform = persist_it->get<bool>();
    }

    const auto position = transform_float3_value(transform_data, "position", "pos");
    const auto rotation = transform_float3_value(transform_data, "rotation", "rot");
    const auto scale = transform_float3_value(transform_data, "scale", "scl");
    if (!position && !rotation && !scale) {
        return native_failure("Transform must include position, rotation, or scale", 2);
    }

    if (position) {
        actor->position = *position;
        if (actor->geometry) {
            actor->geometry->set_position(*position);
        }
    }
    if (rotation) {
        actor->rotation = *rotation;
        if (actor->geometry) {
            actor->geometry->set_rotation(*rotation);
        }
    }
    if (scale) {
        actor->scale = *scale;
        if (actor->geometry) {
            actor->geometry->set_scale(*scale);
        }
    }
    actor->actor_version = std::max(actor->actor_version + 1, 1);
    // Blockly/game-preview transforms can update every frame. Persisting the
    // complete scene on every frame starves stop/restore requests and performs
    // excessive disk I/O. Runtime callers pass persist=false; snapshot restore
    // and regular editor operations keep the default persistent behavior.
    if (persist_transform) {
        sync_native_actor_to_embedded_vision_document(*scene, *actor);
        persist_native_scene_actors(*scene);
    }
    return native_success({
        {"status", "success"},
        {"scene", scene->route},
        {"actor", actor_to_json(*scene, *actor)},
    });
}

NativeResult apply_actor_operation(NativeEditorScene& scene,
                                   NativeEditorActor& actor,
                                   const std::string& operation,
                                   const nlohmann::json& vector) {
    if (operation == "SetMass") {
        if (actor.mechanics) actor.mechanics->set_mass(json_float_at(vector, 0, 1.0f));
    } else if (operation == "SetRestitution") {
        if (actor.mechanics) actor.mechanics->set_restitution(json_float_at(vector, 0, 0.8f));
    } else if (operation == "SetDamping") {
        if (actor.mechanics) actor.mechanics->set_damping(json_float_at(vector, 0, 0.99f));
    } else if (operation == "SetPhysicsEnabled") {
        if (actor.mechanics) actor.mechanics->set_physics_enabled(json_bool_at(vector, 0, true));
    } else if (operation == "SetCollision") {
        const auto value = normalize_collision_type(json_string_at(vector, 0, "box"));
        if (actor.mechanics) actor.mechanics->set_collision_shape(value);
    } else if (operation == "SetVisible") {
        if (actor.optics) actor.optics->set_visible(json_bool_at(vector, 0, true));
    } else if (operation == "SetFollowCamera") {
        actor.follow_camera = json_bool_at(vector, 0, false);
        if (actor.engine_actor) actor.engine_actor->set_follow_camera(actor.follow_camera);
    } else if (operation == "SetLinearLock") {
        if (actor.mechanics) {
            actor.mechanics->set_linear_lock(json_bool_at(vector, 0),
                                             json_bool_at(vector, 1),
                                             json_bool_at(vector, 2));
        }
    } else if (operation == "SetAngularLock") {
        if (actor.mechanics) {
            actor.mechanics->set_angular_lock(json_bool_at(vector, 0),
                                              json_bool_at(vector, 1),
                                              json_bool_at(vector, 2));
        }
    } else if (operation == "SetCameraLock" ||
               operation == "SetCameraLockOffset" ||
               operation == "SetCameraLockRotation") {
        // Camera-lock metadata is not yet stored natively; keep the existing payload shape
        // alive so the details panel remains usable while native persistence catches up.
    } else {
        return native_failure("Unsupported actor operation: " + operation, 2);
    }

    sync_native_actor_to_embedded_vision_document(
        scene,
        actor,
        false,
        operation != "SetVisible");
    return native_success({
        {"scene", scene.route},
        {"actor", actor.name},
        {"operation", operation},
        {"vector", vector},
    });
}

void emit_actor_change(const NativeContext& context,
                       const NativeEditorScene& scene,
                       const NativeEditorActor& actor) {
    (void)context;
    emit_editor_api_event("SceneTools.actorChanged",
                          {{"actor_type", actor.actor_type},
                           {"scene", scene.route},
                           {"actor", actor.name}});
}

void emit_scene_tree_changed(const std::string& scene_route) {
    emit_editor_api_event("SceneTools.sceneTreeChanged", {{"scene", scene_route}});
}

std::string current_time_string() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::filesystem::path editor_root_path() {
    const auto cwd = std::filesystem::current_path();
    const auto installed = cwd / "CabbageEditor";
    if (std::filesystem::is_directory(installed)) {
        return installed;
    }
    const auto source = cwd / "editor";
    if (std::filesystem::is_directory(source)) {
        return source;
    }
    return cwd;
}

std::filesystem::path editor_ini_path() {
    return std::filesystem::current_path() / "CoronaEditor.ini";
}

std::filesystem::path project_template_path() {
    const auto editor_root = editor_root_path();
    const auto corona_core_template = editor_root / "CoronaCore" / "demo" / "project";
    if (std::filesystem::is_directory(corona_core_template)) {
        return corona_core_template;
    }
    return editor_root / "plugins" / "ProjectLauncher" / "demo" / "project";
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

std::optional<std::filesystem::path> show_native_path_dialog(
    bool pick_folder,
    const std::filesystem::path& default_path,
    const wchar_t* title) {
#ifdef _WIN32
    ScopedComInitialization com;
    if (!com.available()) {
        return std::nullopt;
    }

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options{};
    HRESULT result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        options |= pick_folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
        result = dialog->SetOptions(options);
    }
    if (SUCCEEDED(result) && title) {
        result = dialog->SetTitle(title);
    }
    if (SUCCEEDED(result) && !pick_folder) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"Corona 场景、项目或 Vision 文件", L"*.ini;*.scene;*.json"},
            {L"所有文件", L"*.*"},
        };
        result = dialog->SetFileTypes(
            static_cast<UINT>(std::size(filters)),
            filters);
    }

    std::error_code ec;
    auto initial_dir = default_path;
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
    (void)pick_folder;
    (void)default_path;
    (void)title;
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> open_project_file_native() {
    const auto default_path = path_from_utf8(settings_value(
        "General",
        "default_path",
        path_to_utf8(runtime_data_dir())));
    return show_native_path_dialog(
        false,
        default_path,
        L"打开项目或 Vision 场景");
}

std::optional<std::filesystem::path> browse_folder_native(
    const std::filesystem::path& default_path,
    const wchar_t* title) {
    return show_native_path_dialog(true, default_path, title);
}

std::optional<std::filesystem::path> choose_portable_scene_target_native() {
    const auto default_path = path_from_utf8(settings_value(
        "General",
        "default_path",
        path_to_utf8(runtime_data_dir())));
    const auto parent = browse_folder_native(
        default_path,
        L"选择便携场景保存位置");
    if (!parent) {
        return std::nullopt;
    }

    auto target = *parent / "PortableScene";
    for (int suffix = 2; std::filesystem::exists(target); ++suffix) {
        target = *parent / ("PortableScene_" + std::to_string(suffix));
    }
    return target;
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
    std::error_code ec;
    return std::filesystem::is_directory(project_dir, ec) &&
           (std::filesystem::is_regular_file(project_dir / "project.ini", ec) ||
            detect_scene_folder(project_dir).has_value());
}

std::filesystem::path canonical_project_dir_for_settings(const std::filesystem::path& project_dir) {
    if (project_dir.empty()) {
        return {};
    }

    std::vector<std::filesystem::path> candidates;
    if (project_dir.is_absolute()) {
        candidates.push_back(project_dir);
    } else {
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

std::string fnv1a_hex12(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str().substr(0, 12);
}

std::string sanitize_vision_source_id(std::string source_id) {
    source_id = trim_ascii(std::move(source_id));
    if (source_id.empty()) {
        return {};
    }
    return safe_project_dir_name(std::move(source_id), "vision");
}

std::string settings_value(const std::string& section,
                           const std::string& key,
                           const std::string& fallback = {}) {
    return ini_value(read_ini_file(editor_ini_path()), section, key, fallback);
}

void replace_ini_section_from_map(const std::filesystem::path& file_path,
                                  const std::string& section,
                                  const std::map<std::string, std::string>& values) {
    std::vector<std::string> lines;
    lines.push_back("[" + section + "]");
    for (const auto& [key, value] : values) {
        lines.push_back(key + " = " + value);
    }
    replace_ini_section(file_path, section, lines);
}

void update_editor_settings_section(const std::string& section,
                                    const std::map<std::string, std::string>& updates) {
    const auto ini_path = editor_ini_path();
    auto ini = read_ini_file(ini_path);
    std::map<std::string, std::string> values;
    const auto existing = ini.find(to_lower_ascii(section));
    if (existing != ini.end()) {
        for (const auto& [key, value] : existing->second) {
            values[key] = value;
        }
    }
    for (const auto& [key, value] : updates) {
        values[key] = value;
    }
    replace_ini_section_from_map(ini_path, section, values);
}

void add_recent_project_native(const std::filesystem::path& project_dir) {
    const auto path = path_to_utf8(canonical_project_dir_for_settings(project_dir));
    auto recent_raw = settings_value("History", "recent_projects", "[]");
    nlohmann::json recent = nlohmann::json::array();
    try {
        recent = nlohmann::json::parse(recent_raw);
    } catch (...) {
        recent = nlohmann::json::array();
    }
    if (!recent.is_array()) {
        recent = nlohmann::json::array();
    }
    nlohmann::json next = nlohmann::json::array();
    next.push_back(path);
    for (const auto& item : recent) {
        if (!item.is_string() || item.get<std::string>() == path) {
            continue;
        }
        if (next.size() >= 10) {
            break;
        }
        next.push_back(item.get<std::string>());
    }
    update_editor_settings_section("History", {{"recent_projects", next.dump()}});
}

void update_project_ini_native(const std::filesystem::path& project_ini,
                               const std::map<std::string, std::string>& updates,
                               bool update_only_time) {
    auto ini = read_ini_file(project_ini);
    std::map<std::string, std::string> values;
    const auto existing = ini.find("project");
    if (existing != ini.end()) {
        for (const auto& [key, value] : existing->second) {
            values[key] = value;
        }
    }
    if (!update_only_time) {
        for (const auto& [key, value] : updates) {
            values[key] = value;
        }
        values["core_version"] = settings_value("General", "version", "1.2.0");
        values["create_time"] = current_time_string();
    }
    values["last_opened"] = current_time_string();
    replace_ini_section_from_map(project_ini, "Project", values);
}

void normalize_project_runtime_paths_native(const std::filesystem::path& project_dir) {
    const auto scene_dir = project_dir / "Scene";
    const auto old_scene = scene_dir / path_from_utf8("场景1.scene");
    const auto new_scene = scene_dir / "default.scene";
    std::error_code ec;
    if (std::filesystem::exists(old_scene, ec) && !std::filesystem::exists(new_scene, ec)) {
        std::filesystem::rename(old_scene, new_scene, ec);
    }
    const auto project_ini = project_dir / "project.ini";
    if (!std::filesystem::is_regular_file(project_ini, ec)) {
        return;
    }
    auto ini = read_ini_file(project_ini);
    auto project = ini.contains("project") ? ini.at("project") : IniSection{};
    bool changed = false;
    for (const auto& key : {"entrance_scene", "active_scene"}) {
        const auto it = project.find(key);
        if (it != project.end() && it->second == "Scene/场景1.scene") {
            project[key] = "Scene/default.scene";
            changed = true;
        }
    }
    const auto scenes_it = project.find("scenes");
    if (scenes_it != project.end()) {
        std::vector<std::string> scenes;
        for (auto route : split_csv_routes(scenes_it->second)) {
            scenes.push_back(route == "Scene/场景1.scene" ? "Scene/default.scene" : route);
        }
        std::ostringstream joined;
        for (size_t i = 0; i < scenes.size(); ++i) {
            if (i) joined << ",";
            joined << scenes[i];
        }
        project["scenes"] = joined.str();
        changed = true;
    }
    if (changed) {
        std::map<std::string, std::string> values(project.begin(), project.end());
        replace_ini_section_from_map(project_ini, "Project", values);
    }
}

std::filesystem::path create_project_from_template_native(const std::filesystem::path& target_path,
                                                          const std::string& project_name,
                                                          const std::string& mode) {
    const auto template_path = project_template_path();
    if (!std::filesystem::is_directory(template_path)) {
        throw std::runtime_error("Project template not found: " + path_to_utf8(template_path));
    }
    if (std::filesystem::exists(target_path)) {
        throw std::runtime_error("Target project already exists: " + path_to_utf8(target_path));
    }
    std::filesystem::copy(
        template_path,
        target_path,
        std::filesystem::copy_options::recursive);
    normalize_project_runtime_paths_native(target_path);
    const auto project_ini = target_path / "project.ini";
    update_project_ini_native(project_ini, {{"name", project_name}, {"mode", mode}}, false);
    return project_ini;
}

std::filesystem::path unique_project_target(const std::filesystem::path& base_dir,
                                            const std::string& base_name) {
    std::filesystem::path target = base_dir / path_from_utf8(base_name);
    int counter = 1;
    while (std::filesystem::exists(target)) {
        target = base_dir / path_from_utf8(base_name + "_" + std::to_string(counter++));
    }
    return target;
}

std::array<float, 3> json_float3_or(const nlohmann::json& value,
                                    std::array<float, 3> fallback) {
    if (!value.is_array() || value.size() < 3) {
        return fallback;
    }
    return {
        json_float_at(value, 0, fallback[0]),
        json_float_at(value, 1, fallback[1]),
        json_float_at(value, 2, fallback[2]),
    };
}

std::array<float, 3> vision_vec_to_corona(const nlohmann::json& value,
                                          std::array<float, 3> fallback) {
    const auto vec = json_float3_or(value, fallback);
    return {vec[0], vec[1], -vec[2]};
}

std::string format_float(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

const nlohmann::json& json_object_or_empty(const nlohmann::json& object, const char* key) {
    static const nlohmann::json empty = nlohmann::json::object();
    if (!object.is_object()) {
        return empty;
    }
    const auto it = object.find(key);
    return it != object.end() && it->is_object() ? *it : empty;
}

const nlohmann::json& json_member_or(const nlohmann::json& object,
                                     const char* key,
                                     const nlohmann::json& fallback) {
    if (!object.is_object()) {
        return fallback;
    }
    const auto it = object.find(key);
    return it != object.end() ? *it : fallback;
}

const nlohmann::json& vision_param_object(const nlohmann::json& object) {
    return json_object_or_empty(object, "param");
}

std::string vision_shape_type(const nlohmann::json& shape) {
    return to_lower_ascii(json_string_value(shape, {"type", "shape_type"}));
}

std::string vision_shape_name(const nlohmann::json& shape,
                              const std::filesystem::path& source_model,
                              size_t index) {
    const auto name = json_string_value(shape, {"name"});
    if (!name.empty()) {
        return name;
    }
    if (!source_model.empty()) {
        return path_to_utf8(source_model.stem());
    }
    return "vision_shape_" + std::to_string(index);
}

std::string vision_shape_guid(const nlohmann::json& shape, size_t index) {
    const auto guid = json_string_value(shape, {"shape_guid", "guid", "id"});
    if (!guid.empty()) {
        return guid;
    }
    return "vision-shape-" + std::to_string(index);
}

std::filesystem::path resolve_vision_model_path_from_dir(const std::filesystem::path& source_dir,
                                                         const nlohmann::json& shape) {
    const auto& params = vision_param_object(shape);
    std::string model = json_string_value(params, {"fn", "path"});
    if (model.empty()) {
        model = json_string_value(shape, {"fn", "path"});
    }
    model = trim_ascii(model);
    if (model.empty()) {
        return {};
    }
    auto path = path_from_utf8(model);
    if (path.is_absolute()) {
        return path;
    }
    return source_dir / path;
}

std::filesystem::path resolve_vision_model_path(const std::filesystem::path& json_path,
                                                const nlohmann::json& shape) {
    return resolve_vision_model_path_from_dir(json_path.parent_path(), shape);
}

std::filesystem::path copy_vision_model_into_project(const std::filesystem::path& project_dir,
                                                     const std::filesystem::path& source_model) {
    std::error_code rel_ec;
    const auto existing_rel = std::filesystem::relative(source_model, project_dir, rel_ec);
    if (!rel_ec && path_is_inside_project(existing_rel)) {
        const auto existing_route = normalize_route(path_to_utf8(existing_rel));
        if (existing_route.rfind("Resource/vision_imports/", 0) == 0 ||
            existing_route == "Resource/vision_imports") {
            return existing_rel;
        }
    }

    const auto rel_dir = std::filesystem::path("Resource") / "vision_imports";
    const auto dst_dir = project_dir / rel_dir;
    std::filesystem::create_directories(dst_dir);
    const auto hash_text = std::to_string(std::hash<std::string>{}(path_to_utf8(source_model)));
    const auto file_name = source_model.stem().string() + "_" + hash_text.substr(0, 8) + source_model.extension().string();
    const auto dst = dst_dir / file_name;
    std::filesystem::copy_file(source_model, dst, std::filesystem::copy_options::overwrite_existing);
    return rel_dir / file_name;
}

std::filesystem::path write_vision_primitive_proxy(const std::filesystem::path& project_dir,
                                                   const nlohmann::json& shape,
                                                   const std::string& shape_type,
                                                   size_t index) {
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::vector<int>> faces;
    const auto& params = vision_param_object(shape);
    if (shape_type == "quad") {
        const float width = json_float_value(params, "width", 1.0f);
        const float height = json_float_value(params, "height", 1.0f);
        const float hw = width * 0.5f;
        const float hh = height * 0.5f;
        vertices = {{{hw, 0.0f, -hh}, {hw, 0.0f, hh}, {-hw, 0.0f, -hh}, {-hw, 0.0f, hh}}};
        faces = {{1, 2, 4}, {4, 3, 1}};
    } else if (shape_type == "cube") {
        const float x = json_float_value(params, "x", json_float_value(params, "width", 1.0f));
        const float y = json_float_value(params, "y", json_float_value(params, "height", x));
        const float z = json_float_value(params, "z", json_float_value(params, "depth", y));
        const float sx = x * 0.5f, sy = y * 0.5f, sz = z * 0.5f;
        vertices = {{{-sx,-sy,-sz},{sx,-sy,-sz},{sx,sy,-sz},{-sx,sy,-sz},
                     {-sx,-sy,sz},{sx,-sy,sz},{sx,sy,sz},{-sx,sy,sz}}};
        faces = {{1,2,3,4},{5,8,7,6},{1,5,6,2},{2,6,7,3},{3,7,8,4},{4,8,5,1}};
    } else if (shape_type == "sphere") {
        const float r = json_float_value(params, "radius", 1.0f);
        vertices = {{{0,r,0},{r,0,0},{0,0,r},{-r,0,0},{0,0,-r},{0,-r,0}}};
        faces = {{1,2,3},{1,3,4},{1,4,5},{1,5,2},{6,3,2},{6,4,3},{6,5,4},{6,2,5}};
    }
    if (vertices.empty()) {
        return {};
    }
    const auto rel_dir = std::filesystem::path("Resource") / "vision_proxies";
    const auto dst_dir = project_dir / rel_dir;
    std::filesystem::create_directories(dst_dir);
    const auto file_name = safe_project_dir_name(json_string_value(shape, {"name"}), shape_type) +
                           "_" + std::to_string(index) + ".obj";
    const auto dst = dst_dir / file_name;
    std::ofstream output(dst);
    output << "# Corona proxy for Vision " << shape_type << "\n";
    for (const auto& v : vertices) {
        output << "v " << format_float(v[0]) << " " << format_float(v[1]) << " " << format_float(v[2]) << "\n";
    }
    for (const auto& face : faces) {
        output << "f";
        for (int vertex : face) output << " " << vertex;
        output << "\n";
    }
    return rel_dir / file_name;
}

nlohmann::json extract_scene_data(const nlohmann::json& document) {
    if (document.is_object() && document.contains("scene") && document["scene"].is_object()) {
        return document["scene"];
    }
    return document;
}

std::map<std::string, std::string> vision_camera_section(const nlohmann::json& document) {
    std::map<std::string, std::string> camera;
    const std::string camera_id = "scene.ini#camera0";
    camera["count"] = "1";
    camera["active_id"] = camera_id;
    camera["camera0.id"] = camera_id;
    camera["camera0.render_backend"] = "vision";
    camera["camera0.vision_render_mode"] = "path_tracing";
    camera["camera0.output_mode"] = "final_color";
    const auto scene_data = extract_scene_data(document);
    nlohmann::json camera_json = nlohmann::json::object();
    if (scene_data.contains("cameras") && scene_data["cameras"].is_array() && !scene_data["cameras"].empty()) {
        camera_json = scene_data["cameras"][0];
    } else if (scene_data.contains("camera")) {
        camera_json = scene_data["camera"];
    }
    const auto& params = camera_json.contains("param") && camera_json["param"].is_object()
                             ? camera_json["param"]
                             : camera_json;
    const auto& transform = json_object_or_empty(params, "transform");
    const auto& transform_params = transform.contains("param") && transform["param"].is_object()
                                       ? transform["param"]
                                       : transform;
    if (!camera_json.empty()) {
        const auto empty_vector = nlohmann::json::array();
        const auto default_direction = nlohmann::json::array({0.0f, 0.0f, -1.0f});
        const auto default_up = nlohmann::json::array({0.0f, 1.0f, 0.0f});
        const auto position = vision_vec_to_corona(
            transform_params.contains("position") ? transform_params["position"] :
            transform_params.contains("t") ? transform_params["t"] : json_member_or(params, "position", empty_vector),
            {0.0f, 0.0f, 5.0f});
        const auto forward = vision_vec_to_corona(
            transform_params.contains("forward") ? transform_params["forward"] :
            transform_params.contains("direction") ? transform_params["direction"] : json_member_or(params, "direction", default_direction),
            {0.0f, 0.0f, 1.0f});
        const auto up = vision_vec_to_corona(
            transform_params.contains("up") ? transform_params["up"] : json_member_or(params, "up", default_up),
            {0.0f, 1.0f, 0.0f});
        auto camera_name = json_string_value(params, {"name"});
        if (camera_name.empty()) {
            camera_name = json_string_value(camera_json, {"name"});
        }
        camera["camera0.name"] = camera_name.empty() ? "VisionCamera" : camera_name;
        camera["camera0.position"] = format_float3(position);
        camera["camera0.forward"] = format_float3(forward);
        camera["camera0.world_up"] = format_float3(up);
        camera["camera0.fov"] = format_float(json_float_value(params, "fov", json_float_value(params, "fov_y", 45.0f)));
    }
    const auto render = json_object_or_empty(document, "render");
    const auto& integrator = json_object_or_empty(render, "integrator");
    const auto& integrator_params = vision_param_object(integrator);
    if (integrator_params.contains("spp")) camera["camera0.vision_spp"] = integrator_params["spp"].dump();
    if (integrator_params.contains("max_depth")) camera["camera0.vision_max_depth"] = integrator_params["max_depth"].dump();
    const auto output = json_object_or_empty(document, "output");
    if (output.contains("denoise")) camera["camera0.vision_denoise"] = json_bool_value(output, "denoise", false) ? "true" : "false";
    return camera;
}

bool is_vision_resource_path_key(std::string key) {
    key = to_lower_ascii(std::move(key));
    return key == "fn" ||
           key == "path" ||
           key == "file" ||
           key == "filename" ||
           key == "texture" ||
           key == "image";
}

bool is_external_resource_reference(const std::string& value) {
    const auto lower = to_lower_ascii(value);
    return lower.find("://") != std::string::npos ||
           lower.rfind("data:", 0) == 0;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::uint32_t adler32_bytes(const std::string& payload) {
    constexpr std::uint32_t mod_adler = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (unsigned char ch : payload) {
        a = (a + ch) % mod_adler;
        b = (b + a) % mod_adler;
    }
    return (b << 16) | a;
}

std::string base64_encode(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (size_t index = 0; index < input.size(); index += 3) {
        const auto b0 = static_cast<unsigned char>(input[index]);
        const auto b1 = index + 1 < input.size() ? static_cast<unsigned char>(input[index + 1]) : 0;
        const auto b2 = index + 2 < input.size() ? static_cast<unsigned char>(input[index + 2]) : 0;
        output.push_back(alphabet[b0 >> 2]);
        output.push_back(alphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
        output.push_back(index + 1 < input.size() ? alphabet[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
        output.push_back(index + 2 < input.size() ? alphabet[b2 & 0x3f] : '=');
    }
    return output;
}

int base64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

std::string base64_decode(const std::string& input) {
    std::string output;
    int value = 0;
    int bits = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch) || ch == '=') {
            continue;
        }
        const int decoded = base64_value(ch);
        if (decoded < 0) {
            throw std::runtime_error("Invalid base64 data in embedded Vision document");
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return output;
}

std::string zlib_store_blocks(const std::string& payload) {
    std::string output;
    output.reserve(payload.size() + payload.size() / 65535 * 5 + 16);
    output.push_back(static_cast<char>(0x78));
    output.push_back(static_cast<char>(0x01));

    size_t offset = 0;
    do {
        const auto remaining = payload.size() - offset;
        const auto chunk_size = std::min<size_t>(remaining, 65535);
        const bool final_block = offset + chunk_size >= payload.size();
        output.push_back(static_cast<char>(final_block ? 0x01 : 0x00));
        const auto len = static_cast<std::uint16_t>(chunk_size);
        const auto nlen = static_cast<std::uint16_t>(~len);
        output.push_back(static_cast<char>(len & 0xff));
        output.push_back(static_cast<char>((len >> 8) & 0xff));
        output.push_back(static_cast<char>(nlen & 0xff));
        output.push_back(static_cast<char>((nlen >> 8) & 0xff));
        output.append(payload.data() + offset, chunk_size);
        offset += chunk_size;
    } while (offset < payload.size());

    const auto checksum = adler32_bytes(payload);
    output.push_back(static_cast<char>((checksum >> 24) & 0xff));
    output.push_back(static_cast<char>((checksum >> 16) & 0xff));
    output.push_back(static_cast<char>((checksum >> 8) & 0xff));
    output.push_back(static_cast<char>(checksum & 0xff));
    return output;
}

std::string zlib_unstore_blocks(const std::string& payload) {
    if (payload.size() < 6) {
        throw std::runtime_error("Embedded Vision document zlib payload is truncated");
    }
    size_t offset = 2;
    const size_t checksum_offset = payload.size() - 4;
    std::string output;
    while (offset < checksum_offset) {
        const auto header = static_cast<unsigned char>(payload[offset++]);
        const bool final_block = (header & 0x01) != 0;
        const auto block_type = (header >> 1) & 0x03;
        if (block_type != 0) {
            throw std::runtime_error("Embedded Vision document uses unsupported compressed deflate blocks");
        }
        if (offset + 4 > checksum_offset) {
            throw std::runtime_error("Embedded Vision document stored block is truncated");
        }
        const auto len = static_cast<std::uint16_t>(
            static_cast<unsigned char>(payload[offset]) |
            (static_cast<unsigned char>(payload[offset + 1]) << 8));
        const auto nlen = static_cast<std::uint16_t>(
            static_cast<unsigned char>(payload[offset + 2]) |
            (static_cast<unsigned char>(payload[offset + 3]) << 8));
        offset += 4;
        if (static_cast<std::uint16_t>(~len) != nlen) {
            throw std::runtime_error("Embedded Vision document stored block length check failed");
        }
        if (offset + len > checksum_offset) {
            throw std::runtime_error("Embedded Vision document stored block exceeds payload");
        }
        output.append(payload.data() + offset, len);
        offset += len;
        if (final_block) {
            break;
        }
    }
    const auto expected = (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset])) << 24) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 1])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 2])) << 8) |
                          static_cast<std::uint32_t>(static_cast<unsigned char>(payload[checksum_offset + 3]));
    if (adler32_bytes(output) != expected) {
        throw std::runtime_error("Embedded Vision document checksum mismatch");
    }
    return output;
}

std::string encode_vision_document_data(const nlohmann::json& document) {
    return base64_encode(zlib_store_blocks(document.dump()));
}

nlohmann::json decode_vision_document_data(const std::string& data) {
    const auto json_text = zlib_unstore_blocks(base64_decode(data));
    auto document = nlohmann::json::parse(json_text);
    if (!document.is_object()) {
        throw std::runtime_error("Embedded Vision document must be a JSON object");
    }
    return document;
}

nlohmann::json* mutable_vision_scene_data(nlohmann::json& document) {
    if (document.is_object() && document.contains("scene") && document["scene"].is_object()) {
        return &document["scene"];
    }
    return document.is_object() ? &document : nullptr;
}

nlohmann::json* mutable_vision_shapes(nlohmann::json& document) {
    auto* scene_data = mutable_vision_scene_data(document);
    if (!scene_data) {
        return nullptr;
    }
    auto it = scene_data->find("shapes");
    if (it == scene_data->end() || (!it->is_array() && !it->is_object())) {
        return nullptr;
    }
    return &*it;
}

bool vision_shape_visible(const nlohmann::json& shape) {
    if (!shape.is_object()) {
        return true;
    }
    if (shape.contains("visible") && shape["visible"].is_boolean()) {
        return shape["visible"].get<bool>();
    }
    const auto params_it = shape.find("param");
    if (params_it != shape.end() && params_it->is_object() &&
        params_it->contains("visible") && (*params_it)["visible"].is_boolean()) {
        return (*params_it)["visible"].get<bool>();
    }
    return true;
}

void cleanup_editor_trs_overrides_for_non_trs_transform(nlohmann::json& shape) {
    if (!shape.is_object()) {
        return;
    }
    auto params_it = shape.find("param");
    if (params_it == shape.end() || !params_it->is_object()) {
        return;
    }
    auto transform_it = params_it->find("transform");
    if (transform_it == params_it->end() || !transform_it->is_object()) {
        return;
    }
    const auto transform_type = json_string_value(*transform_it, {"type"});
    if (transform_type == "trs") {
        return;
    }
    auto transform_params_it = transform_it->find("param");
    if (transform_params_it == transform_it->end() || !transform_params_it->is_object()) {
        return;
    }
    transform_params_it->erase("t");
    transform_params_it->erase("s");
    transform_params_it->erase("r");
}

void cleanup_vision_document_editor_transform_overrides(nlohmann::json& document) {
    auto* shapes = mutable_vision_shapes(document);
    if (!shapes) {
        return;
    }
    if (shapes->is_array()) {
        for (auto& shape : *shapes) {
            cleanup_editor_trs_overrides_for_non_trs_transform(shape);
        }
        return;
    }
    if (shapes->is_object()) {
        for (auto& item : shapes->items()) {
            cleanup_editor_trs_overrides_for_non_trs_transform(item.value());
        }
    }
}

nlohmann::json vision_document_for_render(nlohmann::json document) {
    cleanup_vision_document_editor_transform_overrides(document);
    return document;
}

std::string vision_shape_match_guid(const nlohmann::json& shape, size_t index) {
    return vision_shape_guid(shape, index);
}

nlohmann::json* find_vision_shape_for_actor(nlohmann::json& document,
                                            const NativeEditorActor& actor) {
    auto* shapes = mutable_vision_shapes(document);
    if (!shapes) {
        return nullptr;
    }
    const auto actor_guid = trim_ascii(actor.actor_guid);
    const auto actor_name = trim_ascii(actor.name);
    auto matches = [&](const nlohmann::json& shape, size_t index) {
        if (!actor_guid.empty() && vision_shape_match_guid(shape, index) == actor_guid) {
            return true;
        }
        return !actor_name.empty() && vision_shape_name(shape, {}, index) == actor_name;
    };
    if (shapes->is_array()) {
        for (size_t index = 0; index < shapes->size(); ++index) {
            if (matches((*shapes)[index], index)) {
                return &(*shapes)[index];
            }
        }
    } else if (shapes->is_object()) {
        size_t index = 0;
        for (auto& item : shapes->items()) {
            if (matches(item.value(), index++)) {
                return &item.value();
            }
        }
    }
    return nullptr;
}

std::string embedded_vision_scene_key(const NativeEditorScene& scene) {
    return path_to_utf8(resolve_project_path(scene.project_root, scene.route)) + ".embedded";
}

void clear_embedded_vision_actor_bindings(NativeEditorScene& scene) {
    for (auto& actor : scene.actors) {
        if (actor.engine_actor) {
            actor.engine_actor->clear_external_vision_binding();
        }
    }
}

NativeEditorActor* find_native_actor_by_guid(NativeEditorScene& scene,
                                             const std::string& actor_guid) {
    if (actor_guid.empty()) {
        return nullptr;
    }
    for (auto& actor : scene.actors) {
        if (actor.actor_guid == actor_guid) {
            return &actor;
        }
    }
    return nullptr;
}

void register_embedded_vision_actor_binding(NativeEditorScene& scene,
                                            const nlohmann::json& shape,
                                            const std::string& scene_key,
                                            size_t index,
                                            const std::string& json_path) {
    const auto shape_guid = vision_shape_guid(shape, index);
    auto* actor = find_native_actor_by_guid(scene, shape_guid);
    if (!actor || !actor->engine_actor) {
        return;
    }
    const bool visible = actor->optics ? actor->optics->get_visible() : true;

    actor->engine_actor->set_external_vision_binding(
        scene_key,
        shape_guid,
        static_cast<int>(index),
        json_path,
        vision_shape_type(shape),
        shape_guid,
        normalize_route(actor->route),
        visible);
}

void register_embedded_vision_actor_bindings(NativeEditorScene& scene,
                                             const nlohmann::json& document,
                                             const std::string& scene_key) {
    clear_embedded_vision_actor_bindings(scene);
    const auto scene_data = extract_scene_data(document);
    const auto shapes_it = scene_data.find("shapes");
    if (shapes_it == scene_data.end()) {
        return;
    }
    if (shapes_it->is_array()) {
        for (size_t index = 0; index < shapes_it->size(); ++index) {
            const auto& shape = (*shapes_it)[index];
            register_embedded_vision_actor_binding(
                scene,
                shape,
                scene_key,
                index,
                "shapes[" + std::to_string(index) + "]");
        }
        return;
    }
    if (shapes_it->is_object()) {
        size_t index = 0;
        for (const auto& item : shapes_it->items()) {
            register_embedded_vision_actor_binding(
                scene,
                item.value(),
                scene_key,
                index++,
                "shapes." + item.key());
        }
    }
}

void ensure_vision_shape_guids(nlohmann::json& document) {
    auto* shapes = mutable_vision_shapes(document);
    if (!shapes) {
        return;
    }
    auto ensure_guid = [](nlohmann::json& shape, size_t index) {
        if (!shape.is_object()) {
            return;
        }
        if (!shape.contains("guid") &&
            !shape.contains("id") &&
            !shape.contains("shape_guid")) {
            shape["guid"] = vision_shape_match_guid(shape, index);
        }
    };
    if (shapes->is_array()) {
        for (size_t index = 0; index < shapes->size(); ++index) {
            ensure_guid((*shapes)[index], index);
        }
    } else if (shapes->is_object()) {
        size_t index = 0;
        for (auto& item : shapes->items()) {
            ensure_guid(item.value(), index++);
        }
    }
}

nlohmann::json& ensure_vision_shape_param(nlohmann::json& shape) {
    if (!shape.is_object()) {
        shape = nlohmann::json::object();
    }
    auto& params = shape["param"];
    if (!params.is_object()) {
        params = nlohmann::json::object();
    }
    return params;
}

nlohmann::json make_vision_shape_from_actor(const NativeEditorActor& actor) {
    nlohmann::json shape = nlohmann::json::object();
    shape["type"] = "model";
    shape["name"] = actor.name;
    if (!actor.actor_guid.empty()) {
        shape["guid"] = actor.actor_guid;
    }
    auto& params = ensure_vision_shape_param(shape);
    params["fn"] = normalize_route(actor.route);
    (void)ensure_native_actor_model_normalization(shape, true);
    return shape;
}

VisionActorMaterialState vision_material_state_from_actor(const NativeEditorActor& actor) {
    VisionActorMaterialState state;
    if (actor.persisted_optics.diffuse) {
        state.diffuse = *actor.persisted_optics.diffuse;
    }
    if (actor.persisted_optics.metallic) {
        state.metallic = *actor.persisted_optics.metallic;
    }
    if (actor.persisted_optics.roughness) {
        state.roughness = *actor.persisted_optics.roughness;
    }
    if (actor.optics) {
        state.diffuse = actor.optics->get_diffuse();
        state.metallic = actor.optics->get_metallic();
        state.roughness = actor.optics->get_roughness();
    }
    return state;
}

std::string vision_actor_material_identity(const NativeEditorActor& actor) {
    return actor.actor_guid.empty() ? actor.name : actor.actor_guid;
}

bool vision_shape_has_named_material(const nlohmann::json& shape) {
    const auto params = shape.find("param");
    if (params == shape.end() || !params->is_object()) {
        return false;
    }
    const auto material = params->find("material");
    return material != params->end() && material->is_string() &&
           !trim_ascii(material->get<std::string>()).empty();
}

bool vision_shape_uses_generated_actor_material(const nlohmann::json& shape) {
    if (!vision_shape_has_named_material(shape)) {
        return false;
    }
    const auto& name = shape.at("param").at("material").get_ref<const std::string&>();
    return name.starts_with("corona_actor_material_");
}

void bind_native_actor_material(nlohmann::json& document,
                                nlohmann::json& shape,
                                const NativeEditorActor& actor) {
    if (auto* scene_data = mutable_vision_scene_data(document)) {
        (void)bind_vision_actor_material(*scene_data,
                                         shape,
                                         vision_actor_material_identity(actor),
                                         vision_material_state_from_actor(actor));
    }
}

bool bind_missing_native_actor_materials(NativeEditorScene& scene,
                                         nlohmann::json& document) {
    ensure_vision_shape_guids(document);
    bool changed = false;
    for (const auto& actor : scene.actors) {
        auto* shape = find_vision_shape_for_actor(document, actor);
        if (!shape) {
            continue;
        }
        changed |= ensure_native_actor_model_normalization(*shape);
        if (!vision_shape_has_named_material(*shape)) {
            bind_native_actor_material(document, *shape, actor);
            changed = true;
        }
    }
    return changed;
}

bool transform_component_changed(const std::array<float, 3>& lhs,
                                 const std::array<float, 3>& rhs) {
    constexpr float epsilon = 1.0e-5f;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (std::abs(lhs[index] - rhs[index]) > epsilon) {
            return true;
        }
    }
    return false;
}

void log_lossy_vision_transform_once(const NativeEditorActor& actor) {
    static std::unordered_set<std::string> logged_actors;
    const auto identity = actor.actor_guid.empty() ? actor.name : actor.actor_guid;
    if (logged_actors.insert(identity).second) {
        CFW_LOG_WARNING(
            "Vision transform for actor '{}' contains shear; native editing uses the closest TRS",
            actor.name);
    }
}

bool hydrate_native_actor_transform_from_vision_shape(NativeEditorActor& actor,
                                                      const nlohmann::json& shape) {
    const auto& params = vision_param_object(shape);
    const auto transform_iterator = params.find("transform");
    if (transform_iterator == params.end()) {
        return false;
    }
    const auto state = decode_vision_actor_transform(*transform_iterator);
    if (!state.valid) {
        CFW_LOG_WARNING("Ignoring invalid Vision transform for actor '{}'", actor.name);
        return false;
    }
    if (state.lossy) {
        log_lossy_vision_transform_once(actor);
    }

    const auto current_position = actor.geometry ? actor.geometry->get_position() : actor.position;
    const auto current_rotation = actor.geometry ? actor.geometry->get_rotation() : actor.rotation;
    const auto current_scale = actor.geometry ? actor.geometry->get_scale() : actor.scale;
    const bool changed = transform_component_changed(current_position, state.position) ||
                         transform_component_changed(current_rotation, state.rotation) ||
                         transform_component_changed(current_scale, state.scale);

    actor.position = state.position;
    actor.rotation = state.rotation;
    actor.scale = state.scale;
    if (actor.geometry && changed) {
        actor.geometry->set_position(state.position);
        actor.geometry->set_rotation(state.rotation);
        actor.geometry->set_scale(state.scale);
    }
    return changed;
}

bool hydrate_native_actor_transforms_from_vision_document(
    NativeEditorScene& scene,
    nlohmann::json& document) {
    bool changed = false;
    for (auto& actor : scene.actors) {
        auto* shape = find_vision_shape_for_actor(document, actor);
        if (shape) {
            changed |= hydrate_native_actor_transform_from_vision_shape(actor, *shape);
        }
    }
    return changed;
}

void write_actor_visibility_to_vision_shape(const NativeEditorActor& actor,
                                            nlohmann::json& shape) {
    const bool visible = actor.optics ? actor.optics->get_visible() : true;
    shape["visible"] = visible;
    auto& params = ensure_vision_shape_param(shape);
    params["visible"] = visible;
    cleanup_editor_trs_overrides_for_non_trs_transform(shape);
    if (actor.engine_actor) {
        const auto actor_handle = actor.engine_actor->get_handle();
        auto binding = Corona::SharedDataHub::instance().external_vision_binding(actor_handle);
        if (binding) {
            binding->visible = visible;
            Corona::SharedDataHub::instance().set_external_vision_binding(actor_handle,
                                                                          std::move(*binding));
        }
    }
}

void write_actor_state_to_vision_shape(const NativeEditorActor& actor,
                                       nlohmann::json& shape,
                                       bool sync_transform) {
    if (!actor.name.empty()) {
        shape["name"] = actor.name;
    }
    if (!actor.actor_guid.empty() &&
        !shape.contains("guid") &&
        !shape.contains("id") &&
        !shape.contains("shape_guid")) {
        shape["guid"] = actor.actor_guid;
    }
    auto& params = ensure_vision_shape_param(shape);
    if (sync_transform) {
        VisionActorTransformState state;
        state.position = actor.geometry ? actor.geometry->get_position() : actor.position;
        state.rotation = actor.geometry ? actor.geometry->get_rotation() : actor.rotation;
        state.scale = actor.geometry ? actor.geometry->get_scale() : actor.scale;
        state.valid = true;
        params["transform"] = encode_vision_actor_transform(state);
    }
    write_actor_visibility_to_vision_shape(actor, shape);
}

bool persist_embedded_vision_document(NativeEditorScene& scene, nlohmann::json& document) {
    if (scene.vision_storage != "embedded") {
        return false;
    }
    scene.vision_document_data = encode_vision_document_data(document);
    scene.vision_document_version = VISION_DOCUMENT_VERSION;
    scene.vision_document_encoding = VISION_DOCUMENT_ENCODING;
    persist_native_scene_vision_document(scene);
    return true;
}

bool refresh_embedded_vision_view(NativeEditorScene& scene,
                                  const nlohmann::json& document) {
    if (scene.vision_storage != "embedded") {
        return false;
    }
    try {
        auto render_document = document;
        (void)bind_missing_native_actor_materials(scene, render_document);
        if (hydrate_native_actor_transforms_from_vision_document(scene, render_document)) {
            persist_native_scene_actors(scene);
        }
        render_document = vision_document_for_render(std::move(render_document));
        const auto scene_key = embedded_vision_scene_key(scene);

        std::size_t shape_count = 0;
        std::size_t duplicate_guid_count = 0;
        std::unordered_set<std::string> unique_shape_guids;
        const auto scene_data = extract_scene_data(render_document);
        const auto shapes_it = scene_data.find("shapes");
        auto inspect_shape = [&](const nlohmann::json& shape, std::size_t index) {
            ++shape_count;
            const auto guid = vision_shape_guid(shape, index);
            if (!unique_shape_guids.insert(guid).second) {
                ++duplicate_guid_count;
            }
        };
        if (shapes_it != scene_data.end() && shapes_it->is_array()) {
            for (std::size_t index = 0; index < shapes_it->size(); ++index) {
                inspect_shape((*shapes_it)[index], index);
            }
        } else if (shapes_it != scene_data.end() && shapes_it->is_object()) {
            std::size_t index = 0;
            for (const auto& item : shapes_it->items()) {
                inspect_shape(item.value(), index++);
            }
        }
        CFW_LOG_INFO(
            "Vision embedded reload snapshot: scene={} shapes={} unique_guids={} duplicate_guids={}",
            scene_key,
            shape_count,
            unique_shape_guids.size(),
            duplicate_guid_count);
        if (duplicate_guid_count != 0) {
            CFW_LOG_WARNING(
                "Vision embedded reload contains duplicate shape GUIDs: scene={} duplicates={}",
                scene_key,
                duplicate_guid_count);
        }

        register_embedded_vision_actor_bindings(scene, render_document, scene_key);
        Corona::API::load_vision_scene_from_json(render_document.dump(),
                                                 path_to_utf8(scene.project_root),
                                                 scene_key,
                                                 true);
        return true;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("Vision embedded view refresh failed: project={}, scene={}, error={}",
                      scene.project_root.string(),
                      scene.route,
                      e.what());
        return false;
    }
}

bool sync_native_actor_to_embedded_vision_document(NativeEditorScene& scene,
                                                   const NativeEditorActor& actor,
                                                   bool create_if_missing,
                                                   bool sync_transform) {
    if (scene.vision_storage != "embedded" || scene.vision_document_data.empty()) {
        return false;
    }
    try {
        auto document = decode_vision_document_data(scene.vision_document_data);
        ensure_vision_shape_guids(document);
        auto* shape = find_vision_shape_for_actor(document, actor);
        bool created_shape = false;
        if (!shape && create_if_missing) {
            auto* scene_data = mutable_vision_scene_data(document);
            if (!scene_data) {
                return false;
            }
            auto& shapes = (*scene_data)["shapes"];
            if (!shapes.is_array()) {
                shapes = nlohmann::json::array();
            }
            shapes.push_back(make_vision_shape_from_actor(actor));
            shape = &shapes.back();
            created_shape = true;
        }
        if (!shape) {
            return false;
        }
        if (created_shape || !vision_shape_has_named_material(*shape) ||
            vision_shape_uses_generated_actor_material(*shape)) {
            bind_native_actor_material(document, *shape, actor);
        }
        (void)ensure_native_actor_model_normalization(*shape, created_shape);
        write_actor_state_to_vision_shape(actor, *shape, sync_transform);
        const bool persisted = persist_embedded_vision_document(scene, document);
        if (persisted && created_shape) {
            refresh_embedded_vision_view(scene, document);
        }
        return persisted;
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("Vision embedded actor sync failed: actor={}, guid={}, error={}",
                      actor.name,
                      actor.actor_guid,
                      e.what());
        return false;
    }
}

bool remove_native_actor_from_embedded_vision_document(NativeEditorScene& scene,
                                                       const std::string& actor_guid) {
    if (scene.vision_storage != "embedded" || scene.vision_document_data.empty() ||
        trim_ascii(actor_guid).empty()) {
        return false;
    }
    try {
        auto document = decode_vision_document_data(scene.vision_document_data);
        ensure_vision_shape_guids(document);
        auto* shapes = mutable_vision_shapes(document);
        if (!shapes) {
            return false;
        }
        bool removed = false;
        if (shapes->is_array()) {
            for (size_t index = 0; index < shapes->size(); ++index) {
                if (vision_shape_match_guid((*shapes)[index], index) == actor_guid) {
                    shapes->erase(shapes->begin() + static_cast<nlohmann::json::difference_type>(index));
                    removed = true;
                    break;
                }
            }
        } else if (shapes->is_object()) {
            size_t index = 0;
            for (auto it = shapes->begin(); it != shapes->end(); ++index) {
                if (vision_shape_match_guid(it.value(), index) == actor_guid) {
                    it = shapes->erase(it);
                    removed = true;
                    break;
                }
                ++it;
            }
        }
        return removed && persist_embedded_vision_document(scene, document);
    } catch (const std::exception& e) {
        CFW_LOG_ERROR("Vision embedded actor remove failed: guid={}, error={}",
                      actor_guid,
                      e.what());
        return false;
    }
}

std::filesystem::path relative_path_if_safe(const std::filesystem::path& path,
                                            const std::filesystem::path& base_dir) {
    std::error_code ec;
    auto relative = std::filesystem::relative(path, base_dir, ec);
    if (ec || relative.empty()) {
        return path.filename();
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return path.filename();
        }
    }
    return relative;
}

void copy_file_creating_directories(const std::filesystem::path& source,
                                    const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    std::error_code ec;
    std::filesystem::copy_file(source,
                               destination,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
        CFW_LOG_WARNING("Vision asset copy failed: {} -> {} ({})",
                        source.string(),
                        destination.string(),
                        ec.message());
    }
}

void copy_mtl_texture_dependencies(const std::filesystem::path& source_mtl,
                                   const std::filesystem::path& sidecar_dir,
                                   const std::filesystem::path& copied_mtl_rel) {
    std::ifstream input(source_mtl);
    if (!input) {
        return;
    }

    static const std::unordered_set<std::string> texture_keys = {
        "map_ka", "map_kd", "map_ks", "map_ke", "map_ns", "map_bump",
        "bump", "disp", "decal", "refl", "norm"};
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream line_stream(line);
        std::string key;
        line_stream >> key;
        key = to_lower_ascii(key);
        if (!texture_keys.contains(key)) {
            continue;
        }

        std::string token;
        std::string texture;
        while (line_stream >> token) {
            if (!token.empty() && token.front() == '-') {
                std::string ignored;
                line_stream >> ignored;
                continue;
            }
            texture = token;
        }
        if (texture.empty() || is_external_resource_reference(texture)) {
            continue;
        }

        const auto source_texture = source_mtl.parent_path() / path_from_utf8(texture);
        if (!std::filesystem::is_regular_file(source_texture)) {
            continue;
        }
        const auto destination =
            sidecar_dir / copied_mtl_rel.parent_path() / path_from_utf8(texture);
        copy_file_creating_directories(source_texture, destination);
    }
}

void copy_obj_dependencies(const std::filesystem::path& source_obj,
                           const std::filesystem::path& sidecar_dir,
                           const std::filesystem::path& copied_obj_rel) {
    std::ifstream input(source_obj);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream line_stream(line);
        std::string key;
        line_stream >> key;
        if (to_lower_ascii(key) != "mtllib") {
            continue;
        }

        std::string mtl_name;
        while (line_stream >> mtl_name) {
            if (mtl_name.empty() || is_external_resource_reference(mtl_name)) {
                continue;
            }
            const auto source_mtl = source_obj.parent_path() / path_from_utf8(mtl_name);
            if (!std::filesystem::is_regular_file(source_mtl)) {
                continue;
            }
            const auto copied_mtl_rel = copied_obj_rel.parent_path() / path_from_utf8(mtl_name);
            copy_file_creating_directories(source_mtl, sidecar_dir / copied_mtl_rel);
            copy_mtl_texture_dependencies(source_mtl, sidecar_dir, copied_mtl_rel);
        }
    }
}

void copy_gltf_dependencies(const std::filesystem::path& source_gltf,
                            const std::filesystem::path& sidecar_dir,
                            const std::filesystem::path& copied_gltf_rel) {
    std::ifstream input(source_gltf);
    if (!input) {
        return;
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(input);
    } catch (...) {
        return;
    }

    auto copy_uri_array = [&](const char* section) {
        if (!document.contains(section) || !document[section].is_array()) {
            return;
        }
        for (const auto& item : document[section]) {
            if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) {
                continue;
            }
            const auto uri = item["uri"].get<std::string>();
            if (uri.empty() || is_external_resource_reference(uri)) {
                continue;
            }
            const auto source_dep = source_gltf.parent_path() / path_from_utf8(uri);
            if (!std::filesystem::is_regular_file(source_dep)) {
                continue;
            }
            const auto destination =
                sidecar_dir / copied_gltf_rel.parent_path() / path_from_utf8(uri);
            copy_file_creating_directories(source_dep, destination);
        }
    };

    copy_uri_array("buffers");
    copy_uri_array("images");
}

std::string copy_vision_archive_asset(const std::filesystem::path& source,
                                      const std::filesystem::path& project_dir,
                                      const std::filesystem::path& archive_root_rel,
                                      const std::filesystem::path& original_scene_dir) {
    if (!std::filesystem::is_regular_file(source)) {
        return {};
    }

    const auto source_rel = relative_path_if_safe(source, original_scene_dir);
    const auto copied_rel = archive_root_rel / source_rel;
    copy_file_creating_directories(source, project_dir / copied_rel);

    const auto ext = to_lower_ascii(source.extension().string());
    if (ext == ".obj") {
        copy_obj_dependencies(source, project_dir, copied_rel);
    } else if (ext == ".gltf") {
        copy_gltf_dependencies(source, project_dir, copied_rel);
    }

    return normalize_route(path_to_utf8(copied_rel));
}

void rewrite_vision_resource_paths_for_project_archive(nlohmann::json& value,
                                                       const std::filesystem::path& source_dir,
                                                       const std::filesystem::path& project_dir,
                                                       const std::filesystem::path& archive_root_rel) {
    if (value.is_object()) {
        for (auto& item : value.items()) {
            auto& child = item.value();
            if (SceneFolders::is_vision_output_section_key(item.key())) continue;
            if (is_vision_resource_path_key(item.key()) && child.is_string()) {
                const auto text = trim_ascii(child.get<std::string>());
                if (!text.empty() && !is_external_resource_reference(text)) {
                    const auto candidate_path = path_from_utf8(text);
                    const auto source_path =
                        candidate_path.is_absolute() ? candidate_path : source_dir / candidate_path;
                    const auto copied = copy_vision_archive_asset(
                        source_path, project_dir, archive_root_rel, source_dir);
                    if (!copied.empty()) {
                        child = copied;
                        continue;
                    }
                }
            }
            rewrite_vision_resource_paths_for_project_archive(child, source_dir, project_dir, archive_root_rel);
        }
        return;
    }

    if (value.is_array()) {
        for (auto& child : value) {
            rewrite_vision_resource_paths_for_project_archive(child, source_dir, project_dir, archive_root_rel);
        }
    }
}

bool is_vision_model_asset(const std::filesystem::path& path) {
    static const std::set<std::string> extensions{
        ".actor", ".dae", ".fbx", ".glb", ".gltf", ".obj", ".usd", ".usda", ".usdc"};
    return extensions.contains(to_lower_ascii(path.extension().string()));
}

void import_vision_resource_paths(nlohmann::json& value,
                                  const std::filesystem::path& source_dir,
                                  SceneAssetStore& store) {
    if (value.is_object()) {
        for (auto& item : value.items()) {
            auto& child = item.value();
            if (SceneFolders::is_vision_output_section_key(item.key())) continue;
            if (is_vision_resource_path_key(item.key()) && child.is_string()) {
                const auto text = trim_ascii(child.get<std::string>());
                if (!text.empty() && !is_external_resource_reference(text)) {
                    const auto candidate = path_from_utf8(text);
                    const auto source = candidate.is_absolute() ? candidate : source_dir / candidate;
                    if (!std::filesystem::is_regular_file(source)) {
                        throw std::runtime_error("Vision resource is missing: " + path_to_utf8(source));
                    }
                    const auto imported = is_vision_model_asset(source)
                                              ? store.import_model(source)
                                              : store.import_file(source, "Vision");
                    if (!imported.ok()) {
                        throw std::runtime_error(
                            imported.diagnostics.empty()
                                ? "Unable to archive Vision resource: " + path_to_utf8(source)
                                : imported.diagnostics.front().message);
                    }
                    child = imported.main_route;
                    continue;
                }
            }
            import_vision_resource_paths(child, source_dir, store);
        }
        return;
    }
    if (value.is_array()) {
        for (auto& child : value) {
            import_vision_resource_paths(child, source_dir, store);
        }
    }
}

EmbeddedVisionDocument create_embedded_vision_document(const std::filesystem::path& project_dir,
                                                       const std::filesystem::path& json_path,
                                                       const nlohmann::json& source_document) {
    const auto raw = read_text_file(json_path);
    const auto import_id = safe_project_dir_name(path_to_utf8(json_path.stem()), "vision") +
                         "_" + fnv1a_hex12(raw.empty() ? path_to_utf8(json_path) : raw);
    auto embedded_document = source_document;
    std::filesystem::path asset_root;
    if (detect_scene_folder(project_dir)) {
        SceneAssetStore store(project_dir);
        import_vision_resource_paths(embedded_document, json_path.parent_path(), store);
        if (!store.write_manifest()) {
            throw std::runtime_error("Unable to write Vision asset manifest");
        }
        asset_root = "Assets";
    } else {
        asset_root = std::filesystem::path("Resource") / "vision_imports" / import_id;
        std::filesystem::create_directories(project_dir / asset_root);
        rewrite_vision_resource_paths_for_project_archive(
            embedded_document, json_path.parent_path(), project_dir, asset_root);
    }
    ensure_vision_shape_guids(embedded_document);

    return EmbeddedVisionDocument{
        embedded_document,
        encode_vision_document_data(embedded_document),
        normalize_route(path_to_utf8(asset_root)),
    };
}

void persist_vision_proxy_actors_from_document(const std::filesystem::path& project_dir,
                                               const std::filesystem::path& scene_file,
                                               const nlohmann::json& document,
                                               const std::filesystem::path& source_dir) {
    const auto scene_data = extract_scene_data(document);
    const bool portable = detect_scene_folder(project_dir).has_value();
    std::optional<SceneAssetStore> portable_store;
    if (portable) {
        portable_store.emplace(project_dir);
    }
    std::map<std::string, std::string> actors;
    size_t imported = 0;
    const auto shapes = scene_data.contains("shapes") ? scene_data["shapes"] : nlohmann::json::array();
    const auto import_shape = [&](size_t index, const nlohmann::json& shape) {
        if (!shape.is_object()) {
            return;
        }
        const auto shape_type = vision_shape_type(shape);
        std::filesystem::path source_model;
        std::filesystem::path route;
        if (shape_type == "model") {
            source_model = resolve_vision_model_path_from_dir(source_dir, shape);
            if (source_model.empty() || !std::filesystem::is_regular_file(source_model)) {
                return;
            }
            if (portable) {
                std::error_code rel_ec;
                const auto relative = std::filesystem::relative(source_model, project_dir, rel_ec);
                const auto existing_route = rel_ec ? std::string{} : normalize_route(path_to_utf8(relative));
                if (!existing_route.empty() && portable_store->contains_route(existing_route)) {
                    route = relative;
                } else {
                    const auto imported = portable_store->import_model(source_model);
                    if (!imported.ok()) {
                        throw std::runtime_error("Unable to archive Vision model proxy");
                    }
                    route = path_from_utf8(imported.main_route);
                }
            } else {
                route = copy_vision_model_into_project(project_dir, source_model);
            }
        } else if (shape_type == "quad" || shape_type == "cube" || shape_type == "sphere") {
            route = write_vision_primitive_proxy(project_dir, shape, shape_type, index);
            if (route.empty()) {
                return;
            }
            if (portable) {
                const auto temporary_proxy = project_dir / route;
                const auto imported = portable_store->import_model(temporary_proxy);
                std::error_code remove_ec;
                std::filesystem::remove(temporary_proxy, remove_ec);
                if (!imported.ok()) {
                    throw std::runtime_error("Unable to archive Vision primitive proxy");
                }
                route = path_from_utf8(imported.main_route);
            }
        } else {
            return;
        }
        const auto key = "actor" + std::to_string(imported++);
        const auto& params = vision_param_object(shape);
        const auto& transform = json_object_or_empty(params, "transform");
        const auto transform_state = decode_vision_actor_transform(transform);
        const auto position = transform_state.valid
                                  ? transform_state.position
                                  : std::array<float, 3>{0.0f, 0.0f, 0.0f};
        const auto rotation = transform_state.valid
                                  ? transform_state.rotation
                                  : std::array<float, 3>{0.0f, 0.0f, 0.0f};
        const auto scale = transform_state.valid
                               ? transform_state.scale
                               : std::array<float, 3>{1.0f, 1.0f, 1.0f};
        actors[key + ".actor_type"] = "model";
        actors[key + ".name"] = vision_shape_name(shape, source_model, index);
        actors[key + ".route"] = normalize_route(path_to_utf8(route));
        actors[key + ".actor_guid"] = vision_shape_guid(shape, index);
        actors[key + ".follow_camera"] = "false";
        actors[key + ".mechanics.physics_enabled"] = "false";
        actors[key + ".geometry.position"] = format_float3(position);
        actors[key + ".geometry.rotation"] = format_float3(rotation);
        actors[key + ".geometry.scale"] = format_float3(scale);
        actors[key + ".optics.visible"] = vision_shape_visible(shape) ? "true" : "false";
        const auto& material = json_object_or_empty(params, "material");
        if (!material.empty()) {
            if (material.contains("base_color")) actors[key + ".optics.diffuse"] = format_float3(json_float3_or(material["base_color"], {0.8f, 0.8f, 0.8f}));
            if (material.contains("metallic")) actors[key + ".optics.metallic"] = material["metallic"].dump();
            if (material.contains("roughness")) actors[key + ".optics.roughness"] = material["roughness"].dump();
            if (material.contains("emission")) actors[key + ".optics.emission"] = format_float3(json_float3_or(material["emission"], {0.0f, 0.0f, 0.0f}));
            if (material.contains("texture") && material["texture"].is_string()) actors[key + ".material.texture"] = material["texture"].get<std::string>();
        }
    };
    if (shapes.is_array()) {
        for (size_t index = 0; index < shapes.size(); ++index) {
            import_shape(index, shapes[index]);
        }
    } else if (shapes.is_object()) {
        size_t index = 0;
        for (const auto& item : shapes.items()) {
            import_shape(index++, item.value());
        }
    }
    replace_ini_section_from_map(scene_file, "actors", actors);
    if (portable && !portable_store->write_manifest()) {
        throw std::runtime_error("Unable to write Vision proxy asset manifest");
    }
}

void apply_vision_json_to_scene_native(const std::filesystem::path& project_dir,
                                       const std::filesystem::path& scene_file,
                                       const std::filesystem::path& json_path) {
    std::ifstream input(json_path);
    if (!input) {
        throw std::runtime_error("Vision scene file not found: " + path_to_utf8(json_path));
    }
    nlohmann::json document = nlohmann::json::parse(input);
    const std::string legacy_vision_section = "vision";
    remove_ini_section(scene_file, legacy_vision_section);
    remove_ini_section(scene_file, "vision_document");
    remove_ini_section(scene_file, "vision_bindings");
    remove_ini_section(scene_file, "vision_unsupported_shapes");
    replace_ini_section_from_map(scene_file, "camera", vision_camera_section(document));
    persist_vision_proxy_actors_from_document(project_dir, scene_file, document, json_path.parent_path());
}

std::filesystem::path create_vision_project_native(const std::filesystem::path& json_path) {
    if (!Corona::API::is_vision_available()) {
        throw std::runtime_error("Vision backend is not available in this build");
    }
    std::ifstream input(json_path);
    if (!input) {
        throw std::runtime_error("Vision scene file not found: " + path_to_utf8(json_path));
    }
    nlohmann::json document = nlohmann::json::parse(input);

    const auto base_dir_text = settings_value("General", "default_path", path_to_utf8(runtime_data_dir()));
    const auto base_dir = path_from_utf8(base_dir_text.empty() ? path_to_utf8(runtime_data_dir()) : base_dir_text);
    std::filesystem::create_directories(base_dir);
    const auto project_name = path_to_utf8(json_path.stem());
    const auto target = unique_project_target(base_dir, project_name);
    const auto final_name = path_to_utf8(target.filename());
    const auto created = create_scene_folder(target, final_name);
    if (!created) {
        throw std::runtime_error("Unable to create portable Vision scene folder");
    }
    try {
        const auto scene_file = target / "scene.ini";
        const auto embedded = create_embedded_vision_document(target, json_path, document);
        remove_ini_section(scene_file, "vision_bindings");
        remove_ini_section(scene_file, "vision_unsupported_shapes");
        replace_ini_section_from_map(scene_file, "camera", vision_camera_section(embedded.document));
        persist_vision_proxy_actors_from_document(target, scene_file, embedded.document, target);
        replace_ini_section_from_map(scene_file,
                                     "vision",
                                     {{"storage", "embedded"},
                                      {"import_mode", "external"}});
        replace_ini_section_from_map(scene_file,
                                     "vision_document",
                                     {{"version", VISION_DOCUMENT_VERSION},
                                      {"encoding", VISION_DOCUMENT_ENCODING},
                                      {"asset_root", embedded.asset_root},
                                      {"data", embedded.data}});
        return target;
    } catch (...) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(target, cleanup_ec);
        throw;
    }
}

void migrate_legacy_embedded_vision_document(const std::filesystem::path& source_path,
                                             const std::filesystem::path& portable_root) {
    auto source = source_path;
    if (std::filesystem::is_directory(source)) source /= "project.ini";
    std::filesystem::path project_root;
    std::filesystem::path scene_file;
    if (source.filename() == "project.ini") {
        project_root = source.parent_path();
        const auto project = read_ini_file(source);
        const auto route = choose_single_scene_route(project_root, project);
        if (route.empty()) return;
        scene_file = resolve_project_path(project_root, route);
    } else if (to_lower_ascii(source.extension().string()) == ".scene") {
        scene_file = source;
        project_root = source.parent_path();
        while (!project_root.empty() && !std::filesystem::is_regular_file(project_root / "project.ini")) {
            const auto parent = project_root.parent_path();
            if (parent == project_root) break;
            project_root = parent;
        }
    } else {
        return;
    }

    const auto legacy_scene = read_ini_file(scene_file);
    const auto encoded = ini_value(legacy_scene, "vision_document", "data");
    if (encoded.empty()) return;
    auto document = decode_vision_document_data(encoded);
    SceneAssetStore store(portable_root);
    import_vision_resource_paths(document, project_root, store);
    if (!store.write_manifest()) {
        throw std::runtime_error("Unable to write migrated Vision asset manifest");
    }
    const auto diagnostics = store.validate_manifest();
    if (!diagnostics.empty()) {
        throw std::runtime_error("Migrated Vision assets are invalid: " + diagnostics.front().message);
    }
    replace_ini_section_from_map(
        portable_root / "scene.ini", "vision_document",
        {{"version", ini_value(legacy_scene, "vision_document", "version", VISION_DOCUMENT_VERSION)},
         {"encoding", VISION_DOCUMENT_ENCODING},
         {"asset_root", "Assets"},
         {"data", encode_vision_document_data(document)}});
}

std::filesystem::path open_project_native(const std::filesystem::path& raw_path) {
    std::filesystem::path project_dir;
    const auto ext = to_lower_ascii(raw_path.extension().string());
    if (ext == ".json") {
        project_dir = create_vision_project_native(raw_path);
    } else if (raw_path.filename() == "scene.ini") {
        project_dir = raw_path.parent_path();
    } else if (ext == ".scene") {
        auto candidate = raw_path.parent_path();
        while (!candidate.empty() && !std::filesystem::is_regular_file(candidate / "project.ini")) {
            const auto parent = candidate.parent_path();
            if (parent == candidate) break;
            candidate = parent;
        }
        if (!std::filesystem::is_regular_file(candidate / "project.ini")) {
            throw std::runtime_error("Legacy scene has no owning project.ini: " + path_to_utf8(raw_path));
        }
        project_dir = candidate;
    } else if (ext == ".ini") {
        if (to_lower_ascii(raw_path.filename().string()) != "project.ini" ||
            !std::filesystem::is_regular_file(raw_path)) {
            throw std::runtime_error("Expected a legacy project.ini: " + path_to_utf8(raw_path));
        }
        project_dir = raw_path.parent_path();
    } else {
        project_dir = raw_path;
    }
    project_dir = canonical_project_dir_for_settings(project_dir);
    if (std::filesystem::is_regular_file(project_dir / "scene.ini") &&
        !detect_scene_folder(project_dir)) {
        const auto ini = read_ini_file(project_dir / "scene.ini");
        const auto type = ini_value(ini, "format", "type");
        const auto version = ini_value(ini, "format", "version");
        if (type == "corona_scene_folder" && version != "1") {
            throw std::runtime_error("Unsupported portable scene version: " + version);
        }
        throw std::runtime_error("Malformed portable scene.ini: " +
                                 path_to_utf8(project_dir / "scene.ini"));
    }
    if (!is_valid_project_dir(project_dir)) {
        throw std::runtime_error("Invalid project path: " + path_to_utf8(project_dir));
    }
    if (detect_scene_folder(project_dir)) {
        const auto validation = SceneFolders::validate_portable_scene(project_dir, true);
        if (!validation.ok()) {
            throw std::runtime_error("Invalid portable scene: " +
                                     validation.diagnostics.front().message + " (" +
                                     path_to_utf8(validation.diagnostics.front().path) + ")");
        }
    }
    update_editor_settings_section("General", {{"last_project", path_to_utf8(project_dir)}});
    add_recent_project_native(project_dir);
    native_editor_state().project_path = path_to_utf8(project_dir);
    native_editor_state().scene.reset();
    return project_dir;
}

nlohmann::json recent_projects_native() {
    auto recent_raw = settings_value("History", "recent_projects", "[]");
    nlohmann::json recent = nlohmann::json::array();
    try {
        recent = nlohmann::json::parse(recent_raw);
    } catch (...) {
        recent = nlohmann::json::array();
    }
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : recent) {
        if (!item.is_string()) {
            continue;
        }
        const auto project_dir = canonical_project_dir_for_settings(path_from_utf8(item.get<std::string>()));
        const auto project_ini = project_dir / "project.ini";
        const bool exists = is_valid_project_dir(project_dir);
        auto ini = exists ? read_ini_file(project_ini) : IniFile{};
        const auto portable = exists ? detect_scene_folder(project_dir) : std::nullopt;
        result.push_back({
            {"name", portable ? portable->scene_name
                               : (exists ? ini_value(ini, "Project", "name", path_to_utf8(project_dir.filename()))
                                         : path_to_utf8(project_dir.filename()))},
            {"path", path_to_utf8(project_dir)},
            {"if_exists", exists},
            {"legacy", exists && !portable},
            {"last_edited", exists ? ini_value(ini, "Project", "last_opened", "-") : "-"},
        });
    }
    return result;
}

nlohmann::json active_project_info_json() {
    auto& state = native_editor_state();
    const auto project_path = normalize_route(
        state.project_path.empty() ? resolve_active_project_path(nlohmann::json::array())
                                   : state.project_path);
    if (project_path.empty()) {
        throw std::runtime_error("No active project path");
    }

    const auto project_root = path_from_utf8(project_path);
    if (const auto portable = detect_scene_folder(project_root)) {
        const auto scene_ini = read_ini_file(portable->scene_file);
        return {
            {"name", portable->scene_name},
            {"entrance_scene", "scene.ini"},
            {"core_version", ini_value(scene_ini, "scene", "core_version")},
            {"create_time", ini_value(scene_ini, "scene", "create_time")},
            {"last_opened", ini_value(scene_ini, "scene", "last_opened")},
            {"project_path", path_to_utf8(project_root)},
        };
    }
    const auto project_ini = read_ini_file(project_root / "project.ini");
    const auto project = project_ini.contains("project") ? project_ini.at("project") : IniSection{};
    const auto value = [&](const std::string& key, const std::string& fallback = {}) {
        const auto it = project.find(to_lower_ascii(key));
        return it == project.end() ? fallback : it->second;
    };

    return {
        {"name", value("name", project_root.filename().string())},
        {"mode", value("mode", "3d")},
        {"entrance_scene", value("entrance_scene", choose_single_scene_route(project_root, project_ini))},
        {"core_version", value("core_version", value("version", ""))},
        {"create_time", value("create_time", value("created_at", ""))},
        {"last_opened", value("last_opened", value("last_open_time", ""))},
        {"project_path", path_to_utf8(project_root)},
    };
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

nlohmann::json build_network_session_info(
    const std::shared_ptr<Corona::Systems::NetworkSystem>& sys) {
    nlohmann::json payload;
    payload["ok"] = true;
    payload["active"] =
        sys->session_state() == Corona::Systems::NetworkSystem::SessionState::Active;
    payload["role"] = std::string(sys->session_role_name());
    payload["peer_count"] = static_cast<int>(sys->peer_count());
    payload["host_address"] = sys->host_address();
    payload["host_port"] = sys->host_port();
    payload["listen_port"] = sys->session_port();
    payload["local_ip"] = detect_wlan_ipv4();
    return payload;
}

void emit_lanchat_event_json(const std::string& event_json) {
    if (event_json.empty()) {
        return;
    }
    nlohmann::json event_payload = nlohmann::json::object();
    try {
        event_payload = nlohmann::json::parse(event_json);
        if (!event_payload.is_object()) {
            event_payload = {{"payload", event_payload}};
        }
    } catch (...) {
        event_payload = {{"raw", event_json}};
    }
    emit_editor_api_event("LANChat.event", event_payload);
}

nlohmann::json build_lanchat_members(
    const std::vector<Corona::Network::LanChatMember>& members) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& member : members) {
        result.push_back(member.nickname);
    }
    return result;
}

nlohmann::json build_lanchat_member_details(
    const std::vector<Corona::Network::LanChatMember>& members) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& member : members) {
        result.push_back({
            {"member_id", member.member_id},
            {"nickname", member.nickname},
            {"status", member.status},
        });
    }
    return result;
}

nlohmann::json build_lanchat_history(
    const std::vector<Corona::Network::LanChatMessage>& history) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& message : history) {
        result.push_back({
            {"message_id", message.message_id},
            {"sender_id", message.sender_id},
            {"room_id", message.room_id},
            {"seq", message.seq},
            {"from", message.sender_name},
            {"text", message.text},
            {"ts", message.timestamp_ms / 1000},
            {"sender_type", message.sender_type},
            {"message_kind", message.message_kind},
            {"target_agent_id", message.target_agent_id},
            {"source_user_id", message.source_user_id},
            {"correlation_id", message.correlation_id},
            {"metadata_json", message.metadata_json},
        });
    }
    return result;
}

nlohmann::json build_lanchat_history_rooms(
    const std::vector<Corona::Network::LanChatHistoryRoomSummary>& rooms) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& room : rooms) {
        const std::string load_id = room.session_id.empty() ? room.room_id : room.session_id;
        result.push_back({
            {"room_id", load_id},
            {"session_id", load_id},
            {"display_room_id", room.room_id},
            {"message_count", room.message_count},
            {"last_timestamp_ms", room.last_timestamp_ms},
            {"last_ts", room.last_timestamp_ms / 1000},
            {"last_sender_name", room.last_sender_name},
            {"last_text", room.last_text},
        });
    }
    return result;
}

nlohmann::json build_lanchat_agents(
    const std::vector<Corona::Network::LanChatAgent>& agents) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& agent : agents) {
        result.push_back({
            {"agent_id", agent.agent_id},
            {"name", agent.name},
            {"persona", agent.persona},
            {"owner", agent.owner_id},
        });
    }
    return result;
}

std::string make_agent_id(const std::string& owner, const std::string& name) {
    static uint64_t counter = 0;
    std::ostringstream out;
    out << "agent-" << std::hash<std::string>{}(owner + ":" + name) << "-" << ++counter;
    return out.str();
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

NativeResult dispatch_method(const std::string& module,
                             const NativeMethodTable& methods,
                             const NativeRequest& request,
                             const NativeContext& context) {
    const auto it = methods.find(request.function);
    if (it == methods.end()) {
        return native_failure("Unknown " + module + " function: " + request.function);
    }
    try {
        return it->second(request, context);
    } catch (const std::exception& e) {
        return native_failure(e.what(), 2);
    } catch (...) {
        return native_failure(module + " native handler error", 2);
    }
}

NativeResult script_method(const NativeRequest& request, const NativeContext&) {
    return invoke_python_script_service(request);
}

std::shared_ptr<Corona::Systems::NetworkSystem> require_network_system() {
    return get_network_system();
}

}  // namespace

std::string create_editor_actor_from_python(const std::string& scene_name,
                                            const std::string& asset_path,
                                            const std::string& actor_type,
                                            const std::string& actor_data_json) {
    try {
        nlohmann::json actor_data = nlohmann::json::object();
        if (!actor_data_json.empty()) {
            actor_data = nlohmann::json::parse(actor_data_json);
            if (!actor_data.is_object()) {
                actor_data = nlohmann::json::object();
            }
        }

        const auto result = create_native_editor_actor(scene_name, asset_path, actor_type, actor_data);
        if (!result.success) {
            return nlohmann::json{
                {"status", "error"},
                {"message", result.error},
                {"error", result.error},
            }.dump();
        }
        return result.data.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
            {"error", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "create_editor_actor native handler error"},
            {"error", "create_editor_actor native handler error"},
        }.dump();
    }
}

std::string remove_editor_actor_from_python(const std::string& scene_name,
                                            const std::string& actor_name) {
    try {
        const auto result = remove_native_editor_actor(scene_name, actor_name);
        if (!result.success) {
            return nlohmann::json{
                {"status", "error"},
                {"message", result.error},
                {"error", result.error},
            }.dump();
        }
        return result.data.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
            {"error", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "remove_editor_actor native handler error"},
            {"error", "remove_editor_actor native handler error"},
        }.dump();
    }
}

std::string get_editor_actor_bounds_from_python(const std::string& scene_name,
                                                const std::string& actor_name) {
    try {
        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);
        auto* actor = find_native_actor(*scene, actor_name);
        if (!actor) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Actor not found: " + actor_name},
            }.dump();
        }
        const auto aabb = native_actor_world_aabb(*actor);
        if (!aabb) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Actor has no native bounds: " + actor_name},
            }.dump();
        }
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"actor", actor_to_json(*scene, *actor)},
            {"aabb", aabb_to_json(*aabb)},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "get_editor_actor_bounds native handler error"},
        }.dump();
    }
}

std::string get_editor_actor_geometry_status_from_python(const std::string& scene_name,
                                                         const std::string& actor_name) {
    try {
        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);
        auto* actor = find_native_actor(*scene, actor_name);
        if (!actor) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Actor not found: " + actor_name},
            }.dump();
        }
        const bool valid = actor->geometry && actor->geometry->is_valid();
        const auto render_status = actor->geometry
            ? actor->geometry->get_render_status()
            : Corona::API::GeometryRenderStatus{};
        const auto gpu_state = valid ? render_status.gpu_build_state : std::string("Invalid");
        const auto mesh_count = valid ? render_status.mesh_count : 0;
        const auto model_id = valid ? actor->geometry->get_model_id() : 0;
        const bool render_ready = actor->actor_type == "audio"
            ? valid && gpu_state == "Ready"
            : valid && render_status.ready;
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"actor", actor->name},
            {"actor_type", actor->actor_type},
            {"ready", render_ready},
            {"failed", gpu_state == "Failed"},
            {"gpu_build_state", gpu_state},
            {"mesh_count", mesh_count},
            {"render_status_observed", render_status.observed},
            {"renderable_mesh_count", render_status.renderable_mesh_count},
            {"invalid_mesh_count", render_status.invalid_mesh_count},
            {"model_id", model_id},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "get_editor_actor_geometry_status native handler error"},
        }.dump();
    }
}

std::string get_editor_scene_snapshot_from_python(const std::string& scene_name) {
    try {
        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);
        nlohmann::json actors = nlohmann::json::array();
        for (const auto& actor : scene->actors) {
            actors.push_back(actor_to_json(*scene, actor));
        }
        const auto scene_aabb = native_scene_world_aabb(*scene);
        nlohmann::json cameras = nlohmann::json::array();
        for (const auto& camera : scene->cameras) {
            cameras.push_back(camera_to_json(camera));
        }
        const auto active_index = scene->cameras.empty()
                                      ? 0
                                      : std::min(scene->active_camera_index, scene->cameras.size() - 1);
        const auto active_camera = scene->cameras.empty()
                                       ? nlohmann::json(nullptr)
                                       : camera_to_json(scene->cameras[active_index]);
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"scene_name", scene->name},
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

std::string set_editor_actor_transform_from_python(const std::string& scene_name,
                                                   const std::string& actor_name,
                                                   const std::string& transform_json) {
    try {
        nlohmann::json transform_data = nlohmann::json::object();
        if (!transform_json.empty()) {
            transform_data = nlohmann::json::parse(transform_json);
            if (!transform_data.is_object()) {
                transform_data = nlohmann::json::object();
            }
        }
        const auto result = set_native_editor_actor_transform(scene_name, actor_name, transform_data);
        if (!result.success) {
            return nlohmann::json{
                {"status", "error"},
                {"message", result.error},
                {"error", result.error},
            }.dump();
        }
        return result.data.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
            {"error", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "set_editor_actor_transform native handler error"},
            {"error", "set_editor_actor_transform native handler error"},
        }.dump();
    }
}

std::string set_editor_camera_transform_from_python(const std::string& scene_name,
                                                    const std::string& camera_name,
                                                    const std::string& camera_data_json) {
    try {
        nlohmann::json camera_data = nlohmann::json::object();
        if (!camera_data_json.empty()) {
            camera_data = nlohmann::json::parse(camera_data_json);
            if (!camera_data.is_object()) {
                camera_data = nlohmann::json::object();
            }
        }

        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        if (!scene_route.empty() && scene_route != scene->route) {
            scene = reload_native_editor_scene("", scene_route);
        }
        auto* camera = find_native_camera(*scene, camera_name);
        if (!camera || !camera->engine_camera) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Native editor camera unavailable"},
            }.dump();
        }

        const auto position = camera_data.contains("position")
                                  ? json_float3_value(camera_data["position"]).value_or(camera->engine_camera->get_position())
                                  : camera->engine_camera->get_position();
        const auto forward = camera_data.contains("forward")
                                 ? json_float3_value(camera_data["forward"]).value_or(camera->engine_camera->get_forward())
                                 : camera->engine_camera->get_forward();
        const auto world_up = camera_data.contains("world_up")
                                  ? json_float3_value(camera_data["world_up"]).value_or(camera->engine_camera->get_world_up())
                                  : camera_data.contains("up")
                                      ? json_float3_value(camera_data["up"]).value_or(camera->engine_camera->get_world_up())
                                      : camera->engine_camera->get_world_up();
        const auto fov = json_float_value(camera_data, "fov", camera->engine_camera->get_fov());
        camera->engine_camera->set(position, forward, world_up, fov);

        const bool persist = json_bool_value(camera_data, "persist", true);
        if (persist) {
            persist_native_scene_cameras(*scene);
        }
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"camera", camera_to_json(*camera)},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "set_editor_camera_transform native handler error"},
        }.dump();
    }
}

std::string get_editor_scene_bounds_from_python(const std::string& scene_name) {
    try {
        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);
        const auto aabb = native_scene_world_aabb(*scene);
        if (!aabb) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Scene has no native actor bounds"},
            }.dump();
        }
        return nlohmann::json{
            {"status", "success"},
            {"scene", scene->route},
            {"aabb", aabb_to_json(*aabb)},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "get_editor_scene_bounds native handler error"},
        }.dump();
    }
}

std::string capture_editor_camera_view_from_python(const std::string& scene_name,
                                                   const std::string& camera_name,
                                                   const std::string& camera_data_json,
                                                   const std::string& output_path) {
    try {
        nlohmann::json camera_data = nlohmann::json::object();
        if (!camera_data_json.empty()) {
            camera_data = nlohmann::json::parse(camera_data_json);
            if (!camera_data.is_object()) {
                camera_data = nlohmann::json::object();
            }
        }

        auto* scene = ensure_native_editor_scene();
        const auto scene_route = normalize_route(scene_name);
        scene = resolve_native_editor_scene_request(scene, scene_route);

        auto* camera = ensure_native_editor_camera(*scene, camera_name, camera_data);
        if (!camera || !camera->engine_camera) {
            return nlohmann::json{
                {"status", "error"},
                {"message", "Native editor camera unavailable"},
            }.dump();
        }

        const auto position = camera_data.contains("position")
                                  ? json_float3_value(camera_data["position"]).value_or(camera->engine_camera->get_position())
                                  : camera->engine_camera->get_position();
        const auto forward = camera_data.contains("forward")
                                 ? json_float3_value(camera_data["forward"]).value_or(camera->engine_camera->get_forward())
                                 : camera->engine_camera->get_forward();
        const auto world_up = camera_data.contains("world_up")
                                  ? json_float3_value(camera_data["world_up"]).value_or(camera->engine_camera->get_world_up())
                                  : camera->engine_camera->get_world_up();
        const auto fov = json_float_value(camera_data, "fov", camera->engine_camera->get_fov());
        camera->engine_camera->set(position, forward, world_up, fov);
        camera->width = std::max(json_int_value(camera_data, "width", camera->width), 1);
        camera->height = std::max(json_int_value(camera_data, "height", camera->height), 1);
        camera->engine_camera->set_size(camera->width, camera->height);
        const auto output_mode = json_string_value(camera_data, {"output_mode"});
        camera->engine_camera->set_output_mode(output_mode.empty() ? "base_color" : output_mode);
        camera->engine_camera->set_ssao_enabled(
            json_bool_value(camera_data, "ssao_enabled", camera->engine_camera->get_ssao_enabled()));
        camera->engine_camera->set_offscreen_capture_mode(true);
        camera->engine_camera->set_surface(0);

        const bool saved = camera->engine_camera->save_screenshot_sync(output_path);
        return nlohmann::json{
            {"status", saved ? "success" : "error"},
            {"ok", saved},
            {"scene", scene->route},
            {"path", output_path},
            {"camera", camera_to_json(*camera)},
            {"message", saved ? "" : "Screenshot save failed"},
        }.dump();
    } catch (const std::exception& e) {
        return nlohmann::json{
            {"status", "error"},
            {"message", e.what()},
        }.dump();
    } catch (...) {
        return nlohmann::json{
            {"status", "error"},
            {"message", "capture_editor_camera_view native handler error"},
        }.dump();
    }
}

void register_project_launcher_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"browse_folder", [](const NativeRequest& request, const NativeContext&) {
            auto default_path = path_from_utf8(arg_string(request.args, 0));
            if (default_path.empty()) {
                default_path = path_from_utf8(settings_value(
                    "General",
                    "default_path",
                    path_to_utf8(runtime_data_dir())));
            }
            const auto selected = browse_folder_native(default_path, L"选择项目目录");
            return native_success(selected ? path_to_utf8(*selected) : std::string{});
        }},
        {"get_default_project_path", [](const NativeRequest&, const NativeContext&) {
            const auto value = settings_value("General", "default_path", path_to_utf8(runtime_data_dir()));
            return native_success(value);
        }},
        {"get_app_version", [](const NativeRequest&, const NativeContext&) {
            return native_success(settings_value("General", "version", "1.2.0"));
        }},
        {"get_recent_projects", [](const NativeRequest&, const NativeContext&) {
            return native_success(recent_projects_native());
        }},
        {"get_project_load_status", [](const NativeRequest&, const NativeContext&) {
            return native_success(project_resource_load_status());
        }},
        {"create_project", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            const auto name = data.value("name", std::string{"New_Corona_Project"});
            const auto base_text = data.value(
                "path",
                settings_value("General", "default_path", path_to_utf8(runtime_data_dir())));
            const auto base_dir = path_from_utf8(base_text);
            const auto target = base_dir / path_from_utf8(name);
            if (!create_scene_folder(target, name)) {
                throw std::runtime_error("Unable to create portable scene folder: " + path_to_utf8(target));
            }
            update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}});
            return native_success(path_to_utf8(target));
        }},
        {"create_world_project", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            const auto world_type = data.value("mode", std::string{"creative"});
            const auto prompt = data.value("prompt", std::string{});
            const bool story = world_type == "story";
            const std::string display_base = story ? "剧情世界" : "创造世界";
            const std::string dir_base = story ? "story_world" : "creative_world";
            const auto base_dir = runtime_data_dir();
            std::filesystem::create_directories(base_dir);
            int index = 1;
            std::filesystem::path target;
            do {
                target = base_dir / (dir_base + "_" + std::to_string(index++));
            } while (std::filesystem::exists(target));
            const auto display_name = display_base + "_" + std::to_string(index - 1);
            if (!create_scene_folder(target, display_name)) {
                throw std::runtime_error("Unable to create portable world scene: " + path_to_utf8(target));
            }
            update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}});
            replace_ini_section_from_map(target / "scene.ini", "world",
                                         {{"type", world_type}, {"prompt", prompt}});
            return native_success({{"name", display_name}, {"path", path_to_utf8(target)}});
        }},
        {"create_multiplayer_project", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            const std::string role = data.value("role", std::string{"guest"}) == "host" ? "host" : "guest";
            const std::string display_base = role == "host" ? "联机房主" : "联机加入";
            const std::string dir_base = role == "host" ? "multiplayer_host" : "multiplayer_guest";
            const auto base_dir = runtime_data_dir();
            std::filesystem::create_directories(base_dir);
            int index = 1;
            std::filesystem::path target;
            do {
                target = base_dir / (dir_base + "_" + std::to_string(index++));
            } while (std::filesystem::exists(target));
            const auto display_name = display_base + "_" + std::to_string(index - 1);
            if (!create_scene_folder(target, display_name)) {
                throw std::runtime_error("Unable to create portable multiplayer scene: " + path_to_utf8(target));
            }
            update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}});
            replace_ini_section_from_map(target / "scene.ini", "multiplayer", {{"role", role}});
            return native_success({{"name", display_name}, {"path", path_to_utf8(target)}, {"role", role}});
        }},
        {"open_project", [](const NativeRequest& request, const NativeContext&) {
            const auto path = normalize_route(arg_string(request.args, 0));
            const auto options = arg_object(request.args, 1);
            const auto load_policy = options.value("load_policy", std::string{"prompt"});
            CFW_LOG_INFO("[ProjectLauncher] open_project request path='{}'", path);
            if (path.empty()) {
                return native_success({{"ok", false}, {"path", std::string{}}, {"status", "cancelled"}});
            }
            auto prepared = prepare_archive_load(path, load_policy);
            if (prepared.service_ok && prepared.snapshot.value("archive_type", std::string{}) == "vision_json") {
                try {
                    const auto imported = create_vision_project_native(path_from_utf8(path));
                    prepared = prepare_archive_load(path_to_utf8(imported), load_policy);
                } catch (const std::exception& error) {
                    return native_success({
                        {"ok", false}, {"status", "invalid_archive"}, {"path", path},
                        {"diagnostics", nlohmann::json::array({{
                            {"severity", "error"}, {"recoverable", false},
                            {"stage", "vision_import"}, {"code", "VISION_IMPORT_FAILED"},
                            {"message", error.what()}, {"path", path},
                        }})},
                    });
                }
            }
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
            update_editor_settings_section("General", {{"last_project", state.project_path}});
            add_recent_project_native(state.scene->project_root);
            std::size_t unresolved_count = 0;
            for (const auto& actor : state.scene->actors) {
                if (actor.load_status != ActorLoadStatus::Loaded) ++unresolved_count;
            }
            CFW_LOG_INFO("[ProjectLauncher] open_project opened path='{}' status='{}'",
                         state.project_path, prepared.status);
            emit_editor_api_event("ProjectLauncher.projectOpened", {
                {"path", state.project_path}, {"status", prepared.status},
            });
            return native_success({
                {"ok", true}, {"status", prepared.status}, {"path", state.project_path},
                {"legacy", prepared.snapshot.value("project", nlohmann::json::object())
                               .value("legacy", false)},
                {"degraded", prepared.status == "opened_degraded" || unresolved_count > 0},
                {"unresolved_actor_count", unresolved_count},
                {"diagnostics", prepared.diagnostics},
                {"scene", {{"path", state.scene->route}, {"name", state.scene->name}}},
            });
        }},
        {"open_project_file", [](const NativeRequest&, const NativeContext&) {
            const auto selected = open_project_file_native();
            if (!selected) {
                return native_success(nlohmann::json::object());
            }
            return native_success({
                {"name", path_to_utf8(selected->stem())},
                {"path", path_to_utf8(*selected)},
            });
        }},
        {"choose_portable_scene_target", [](const NativeRequest&, const NativeContext&) {
            const auto selected = choose_portable_scene_target_native();
            return native_success(selected ? path_to_utf8(*selected) : std::string{});
        }},
        {"validate_portable_scene", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            auto root_text = data.value("path", std::string{});
            if (root_text.empty()) root_text = resolve_active_project_path(nlohmann::json::array());
            const auto root = canonical_project_dir_for_settings(path_from_utf8(root_text));
            std::string format_status = "invalid";
            SceneFolders::SceneValidationResult validation;
            if (detect_scene_folder(root)) {
                format_status = "portable_v1";
                validation = SceneFolders::validate_portable_scene(root, data.value("verifyHashes", true));
            } else if (std::filesystem::is_regular_file(root / "scene.ini")) {
                const auto ini = read_ini_file(root / "scene.ini");
                const auto type = ini_value(ini, "format", "type");
                const auto version = ini_value(ini, "format", "version");
                format_status = type == "corona_scene_folder" && version != "1"
                                    ? "unsupported_version"
                                    : "malformed";
                validation.diagnostics.push_back({
                    format_status, format_status == "unsupported_version"
                                       ? "Portable scene version is unsupported"
                                       : "Portable scene metadata is malformed",
                    root / "scene.ini"});
            } else if (std::filesystem::is_regular_file(root / "project.ini")) {
                format_status = "legacy";
                validation.diagnostics.push_back(
                    {"legacy_read_only", "Legacy projects are read-only", root / "project.ini"});
            }
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const auto& item : validation.diagnostics) {
                diagnostics.push_back({{"code", item.code}, {"message", item.message},
                                       {"path", path_to_utf8(item.path)}, {"actor", item.actor},
                                       {"field", item.field}});
            }
            return native_success({{"ok", validation.ok()}, {"status", format_status},
                                   {"path", path_to_utf8(root)}, {"assetCount", validation.asset_count},
                                   {"totalBytes", validation.total_bytes},
                                   {"diagnostics", std::move(diagnostics)}});
        }},
        {"import_portable_asset", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            auto root_text = data.value("path", std::string{});
            if (root_text.empty()) root_text = resolve_active_project_path(nlohmann::json::array());
            const auto root = canonical_project_dir_for_settings(path_from_utf8(root_text));
            if (!detect_scene_folder(root)) {
                return native_success({{"ok", false}, {"diagnostics", nlohmann::json::array({{
                    {"code", "invalid_scene_format"}, {"message", "Expected a portable scene folder"},
                    {"path", path_to_utf8(root)}, {"actor", ""}, {"field", ""}}})}});
            }
            auto source = path_from_utf8(
                data.value("sourcePath", data.value("source_path", std::string{})));
            if (source.is_relative()) source = root / source;
            const auto category_text = data.value("category", std::string{});
            const auto category_lower = to_lower_ascii(category_text);
            static const std::map<std::string, std::string> categories{
                {"images", "Images"}, {"audio", "Audio"}, {"scripts", "Scripts"},
                {"terrain", "Terrain"}, {"vision", "Vision"}};
            SceneAssetStore store(root);
            SceneFolders::ImportResult imported;
            if (category_lower == "model" || category_lower == "models") {
                imported = store.import_model(source);
            } else if (const auto category = categories.find(category_lower); category != categories.end()) {
                imported = store.import_file(source, category->second);
            } else {
                imported.diagnostics.push_back(
                    {"unsupported_category", "Unsupported portable asset category", source, {}, "category"});
            }
            if (imported.ok() && !store.write_manifest()) {
                imported.diagnostics.push_back(
                    {"manifest_write_failed", "Unable to update portable asset manifest", root});
            }
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const auto& item : imported.diagnostics) {
                diagnostics.push_back({{"code", item.code}, {"message", item.message},
                                       {"path", path_to_utf8(item.path)}, {"actor", item.actor},
                                       {"field", item.field}});
            }
            return native_success({{"ok", imported.ok()}, {"path", imported.main_route},
                                   {"type", imported.type}, {"bundleSha256", imported.bundle_sha256},
                                   {"diagnostics", std::move(diagnostics)}});
        }},
        {"cleanup_portable_scene_assets", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            auto root_text = data.value("path", std::string{});
            if (root_text.empty()) root_text = resolve_active_project_path(nlohmann::json::array());
            const auto root = canonical_project_dir_for_settings(path_from_utf8(root_text));
            const bool dry_run = data.value("dryRun", true);
            const auto cleaned = SceneFolders::cleanup_portable_scene_assets(root, dry_run);
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const auto& item : cleaned.diagnostics) {
                diagnostics.push_back({{"code", item.code}, {"message", item.message},
                                       {"path", path_to_utf8(item.path)}, {"actor", item.actor},
                                       {"field", item.field}});
            }
            return native_success({{"ok", cleaned.ok()}, {"dryRun", dry_run},
                                   {"removedBundles", cleaned.removed_bundles},
                                   {"removedFiles", cleaned.removed_files},
                                   {"reclaimedBytes", cleaned.reclaimed_bytes},
                                   {"diagnostics", std::move(diagnostics)}});
        }},
        {"migrate_legacy_scene", [](const NativeRequest& request, const NativeContext&) {
            const auto data = arg_object(request.args, 0);
            const auto source_path = path_from_utf8(
                data.value("sourcePath", data.value("source_path", std::string{})));
            const auto target_path = path_from_utf8(
                data.value("targetPath", data.value("target_path", std::string{})));
            const auto scene_name = data.value("sceneName", data.value("scene_name", std::string{}));
            const auto migrated = migrate_legacy_scene({source_path, target_path, scene_name});
            nlohmann::json diagnostics = nlohmann::json::array();
            for (const auto& diagnostic : migrated.diagnostics) {
                diagnostics.push_back({
                    {"code", diagnostic.code},
                    {"message", diagnostic.message},
                    {"path", path_to_utf8(diagnostic.path)},
                    {"actor", diagnostic.actor},
                    {"field", diagnostic.field},
                });
            }
            if (!migrated.ok()) {
                return native_success({{"ok", false}, {"path", std::string{}},
                                       {"diagnostics", std::move(diagnostics)}});
            }
            try {
                migrate_legacy_embedded_vision_document(source_path, migrated.root);
            } catch (const std::exception& error) {
                std::error_code cleanup_ec;
                std::filesystem::remove_all(migrated.root, cleanup_ec);
                diagnostics.push_back({{"code", "vision_migration_failed"},
                                       {"message", error.what()},
                                       {"path", path_to_utf8(source_path)},
                                       {"actor", std::string{}},
                                       {"field", "vision_document.data"}});
                return native_success({{"ok", false}, {"path", std::string{}},
                                       {"diagnostics", std::move(diagnostics)}});
            }
            return native_success({{"ok", true}, {"path", path_to_utf8(migrated.root)},
                                   {"diagnostics", std::move(diagnostics)}});
        }},
        {"set_project_mode", [](const NativeRequest&, const NativeContext&) {
            return native_success(true);
        }},
    };

    registry.register_module("ProjectLauncher", [](const NativeRequest& request,
                                                   const NativeContext& context) {
        const auto it = methods.find(request.function);
        if (it == methods.end()) {
            return native_unhandled();
        }
        try {
            return it->second(request, context);
        } catch (const std::exception& e) {
            if (request.function == "open_project") {
                CFW_LOG_ERROR("[ProjectLauncher] open_project failed: {}", e.what());
            }
            return native_failure(e.what(), 2);
        } catch (...) {
            if (request.function == "open_project") {
                CFW_LOG_ERROR("[ProjectLauncher] open_project failed: unknown error");
            }
            return native_failure("ProjectLauncher native handler error", 2);
        }
    });
}

void register_main_view_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"get_menu_data", script_method},
        {"import_resource_file", script_method},
        {"on_init", [](const NativeRequest&, const NativeContext&) {
            auto& state = native_editor_state();
            if (!state.scene) {
                return native_failure("No committed native editor scene; open a project first", 404);
            }
            return native_success(make_on_init_payload(*state.scene));
        }},
        {"run_project", script_method},
        {"scene_save", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            if (!scene_route.empty() && scene_route != scene->route) {
                scene = reload_native_editor_scene("", scene_route);
            }
            const auto snapshot = arg_object(request.args, 1);
            if (!snapshot.empty()) {
                if (snapshot.contains("name") && snapshot["name"].is_string()) {
                    scene->name = snapshot["name"].get<std::string>();
                }
                if (snapshot.contains("core_version") && snapshot["core_version"].is_string()) {
                    scene->core_version = snapshot["core_version"].get<std::string>();
                }
                if (snapshot.contains("script_path") && snapshot["script_path"].is_string()) {
                    scene->script_path = normalize_route(snapshot["script_path"].get<std::string>());
                }
                if (snapshot.contains("terrain") && snapshot["terrain"].is_object()) {
                    const auto& terrain = snapshot["terrain"];
                    scene->terrain_path = normalize_route(terrain.value("path", scene->terrain_path));
                    scene->terrain_type = terrain.value("type", scene->terrain_type);
                }
                if (snapshot.contains("vision") && snapshot["vision"].is_object()) {
                    const auto& vision = snapshot["vision"];
                    scene->vision_storage = vision.value("storage", scene->vision_storage);
                    scene->vision_source_id = vision.value("source_id", scene->vision_source_id);
                    scene->vision_import_mode = vision.value("import_mode", scene->vision_import_mode);
                }
                if (snapshot.contains("vision_document") && snapshot["vision_document"].is_object()) {
                    const auto& document = snapshot["vision_document"];
                    const auto data = document.value("data", std::string{});
                    if (!data.empty()) {
                        scene->vision_document_version = document.value("version", std::string(VISION_DOCUMENT_VERSION));
                        scene->vision_document_encoding = document.value("encoding", std::string(VISION_DOCUMENT_ENCODING));
                        scene->vision_document_asset_root = document.value("asset_root", std::string{});
                        scene->vision_document_data = data;
                    }
                }
            }
            try {
                persist_native_scene_common(*scene);
            } catch (const PortableSceneValidationError& error) {
                return native_success({{"ok", false}, {"status", "error"},
                                       {"message", error.what()},
                                       {"diagnostics", error.diagnostics()}});
            }
            nlohmann::json save_diagnostics = scene->load_diagnostics;
            std::size_t unresolved_count = 0;
            for (const auto& actor : scene->actors) {
                if (actor.load_status == ActorLoadStatus::Loaded) continue;
                ++unresolved_count;
                const bool already_reported = std::any_of(
                    save_diagnostics.begin(), save_diagnostics.end(),
                    [&](const nlohmann::json& diagnostic) {
                        return diagnostic.value("actor_guid", std::string{}) == actor.actor_guid &&
                               diagnostic.value("code", std::string{}) == actor.load_error_code;
                    });
                if (!already_reported) {
                    save_diagnostics.push_back({
                        {"code", actor.load_error_code}, {"message", actor.load_error_message},
                        {"actor_guid", actor.actor_guid}, {"actor_name", actor.name},
                        {"resource_path", actor.route},
                    });
                }
            }
            return native_success({
                {"ok", true},
                {"status", "success"},
                {"filepath", path_to_utf8(resolve_project_path(scene->project_root, scene->route))},
                {"format", "corona_scene"},
                {"degraded", !save_diagnostics.empty()},
                {"unresolved_actor_count", unresolved_count},
                {"diagnostics", std::move(save_diagnostics)},
            });
        }},
        {"update_view_tool_state", script_method},
    };

    registry.register_module("MainView", [](const NativeRequest& request,
                                            const NativeContext& context) {
        const auto it = methods.find(request.function);
        if (it == methods.end()) {
            return native_unhandled();
        }
        try {
            return it->second(request, context);
        } catch (const std::exception& e) {
            return native_failure(e.what(), 2);
        } catch (...) {
            return native_failure("MainView native handler error", 2);
        }
    });
}

void register_project_settings_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"browse_scene_file", script_method},
        {"get_active_project_info", [](const NativeRequest&, const NativeContext&) {
            return native_success(active_project_info_json());
        }},
        {"save_active_project_info", script_method},
    };

    registry.register_module("ProjectSettings", [](const NativeRequest& request,
                                                   const NativeContext& context) {
        const auto it = methods.find(request.function);
        if (it == methods.end()) {
            return native_unhandled();
        }
        try {
            return it->second(request, context);
        } catch (const std::exception& e) {
            return native_failure(e.what(), 2);
        } catch (...) {
            return native_failure("ProjectSettings native handler error", 2);
        }
    });
}

void register_scene_datas_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"get_scene", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            return native_success(scene_to_json(*scene));
        }},
        {"get_actor", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto actor_name = arg_string(request.args, 1);
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor) {
                return native_failure("Actor not found: " + actor_name, 2);
            }
            return native_success(actor_to_json(*scene, *actor));
        }},
        {"actor_operation", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto actor_name = arg_string(request.args, 1);
            const auto operation = arg_string(request.args, 2);
            const auto vector = request.args.is_array() && request.args.size() > 3
                                    ? request.args[3]
                                    : nlohmann::json::array();
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor) {
                return native_failure("Actor not found: " + actor_name, 2);
            }
            auto result = apply_actor_operation(*scene, *actor, operation, vector);
            if (result.handled && result.success) {
                persist_native_scene_actors(*scene);
            }
            return result;
        }},
        {"save_actor", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto actor_name = arg_string(request.args, 1);
            if (!actor_name.empty() && !find_native_actor(*scene, actor_name)) {
                return native_failure("Actor not found: " + actor_name, 2);
            }
            persist_native_scene_actors(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"actor", actor_name},
            });
        }},
        {"select_model_file", script_method},
    };

    registry.register_module("SceneDatas", [](const NativeRequest& request,
                                              const NativeContext& context) {
        const auto it = methods.find(request.function);
        if (it == methods.end()) {
            return native_unhandled();
        }
        try {
            return it->second(request, context);
        } catch (const std::exception& e) {
            return native_failure(e.what(), 2);
        } catch (...) {
            return native_failure("SceneDatas native handler error", 2);
        }
    });
}

void register_scene_tools_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"close_camera_view", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return camera_not_found_result(camera_name);
            }
            camera->view_open = false;
            camera->engine_camera->set_view_state(false, camera->view_x, camera->view_y,
                                                  camera->view_width, camera->view_height,
                                                  camera->move_speed);
            camera->engine_camera->set_surface(0);
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"create_camera_view", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            auto& camera = create_native_camera_view(*scene, arg_string(request.args, 1));
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera", camera_to_json(camera)},
            });
        }},
        {"create_scene", script_method},
        {"delete_camera", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto camera_name = arg_string(request.args, 1);
            if (scene->cameras.size() <= 1) {
                return native_failure("A scene must keep at least one camera", 2);
            }
            const auto it = std::find_if(scene->cameras.begin(), scene->cameras.end(), [&](const NativeEditorCamera& camera) {
                return camera.name == camera_name || camera.camera_id == camera_name ||
                       (camera.engine_camera && std::to_string(camera.engine_camera->get_handle()) == camera_name);
            });
            if (it == scene->cameras.end()) {
                return camera_not_found_result(camera_name);
            }
            if (!it->deletable) {
                return native_failure("The main camera cannot be deleted", 2);
            }
            const auto removed_id = it->camera_id;
            if (it->engine_camera) {
                it->engine_camera->set_surface(0);
                if (scene->engine_scene) {
                    scene->engine_scene->remove_camera(it->engine_camera.get());
                }
            }
            const auto removed_index = static_cast<size_t>(std::distance(scene->cameras.begin(), it));
            scene->cameras.erase(it);
            if (scene->active_camera_index >= scene->cameras.size()) {
                scene->active_camera_index = scene->cameras.empty() ? 0 : scene->cameras.size() - 1;
            } else if (removed_index < scene->active_camera_index) {
                --scene->active_camera_index;
            }
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera_id", removed_id},
            });
        }},
        {"list_scene_tree", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            nlohmann::json actors = nlohmann::json::array();
            for (const auto& actor : scene->actors) {
                actors.push_back({
                    {"name", actor.name},
                    {"path", actor.route},
                    {"type", actor.actor_type},
                    {"visible", actor.optics ? actor.optics->get_visible() : true},
                    {"handle", actor.engine_actor ? actor.engine_actor->get_handle() : 0},
                    {"actor_guid", actor.actor_guid},
                    {"vision_proxy", false},
                    {"audio_resource_id", actor.actor_type == "audio"
                                              ? std::to_string(actor.audio_resource_id)
                                              : std::string{}},
                });
            }
            nlohmann::json cameras = nlohmann::json::array();
            for (const auto& camera : scene->cameras) {
                cameras.push_back(camera_to_json(camera));
            }
            const bool has_project_sidecar =
                scene->vision_storage == "project_sidecar" &&
                !sanitize_vision_source_id(scene->vision_source_id).empty();
            return native_success({
                {"actors", actors},
                {"cameras", cameras},
                {"vision", {
                    {"enabled", has_project_sidecar || !scene->vision_source_path.empty()},
                    {"storage", scene->vision_storage},
                    {"source_id", scene->vision_source_id},
                    {"source_path", scene->vision_source_path},
                    {"import_mode", scene->vision_import_mode},
                    {"binding_count", 0},
                    {"unsupported_count", 0},
                }},
            });
        }},
        {"list_actor_tree", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            nlohmann::json actors = nlohmann::json::array();
            for (const auto& actor : scene->actors) {
                actors.push_back(actor_to_json(*scene, actor));
            }
            return native_success(actors);
        }},
        {"reload_scene", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_route = arg_string(request.args, 0);
            const auto project_path = arg_string(request.args, 1);
            auto* scene = reload_native_editor_scene(project_path, scene_route);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"actor_count", scene->actors.size()},
                {"camera_count", scene->cameras.size()},
            });
        }},
        {"rebind_actor_resource", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto actor_guid = arg_string(request.args, 1);
            auto source_path = path_from_utf8(arg_string(request.args, 2));
            if (source_path.is_relative()) source_path = scene->project_root / source_path;
            if (!std::filesystem::is_regular_file(source_path)) {
                return native_success({
                    {"ok", false}, {"status", "resource_error"},
                    {"diagnostics", nlohmann::json::array({{
                        {"code", "RESOURCE_NOT_FOUND"}, {"recoverable", true},
                        {"actor_guid", actor_guid}, {"path", path_to_utf8(source_path)},
                        {"message", "Replacement resource does not exist"},
                    }})},
                });
            }
            auto* actor = find_native_actor_by_guid(*scene, actor_guid);
            if (!actor) return native_failure("Actor GUID not found: " + actor_guid, 404);
            if (!snapshot_actor_resource_supported(*actor, source_path)) {
                return native_success({
                    {"ok", false}, {"status", "resource_error"},
                    {"diagnostics", nlohmann::json::array({{
                        {"code", "UNSUPPORTED_RESOURCE_TYPE"}, {"recoverable", true},
                        {"actor_guid", actor_guid}, {"path", path_to_utf8(source_path)},
                        {"message", "Replacement resource type is unsupported"},
                    }})},
                });
            }

            std::string route;
            auto resolved_path = std::filesystem::weakly_canonical(source_path);
            if (detect_scene_folder(scene->project_root)) {
                std::error_code ec;
                auto relative = std::filesystem::relative(resolved_path, scene->project_root, ec);
                if (ec || relative.empty() || relative.generic_string().rfind("..", 0) == 0) {
                    SceneAssetStore store(scene->project_root);
                    auto imported = store.import_model(resolved_path);
                    if (!imported.ok() || !store.write_manifest()) {
                        return native_success({
                            {"ok", false}, {"status", "resource_error"},
                            {"diagnostics", nlohmann::json::array({{
                                {"code", "RESOURCE_IMPORT_FAILED"}, {"recoverable", true},
                                {"actor_guid", actor_guid}, {"path", path_to_utf8(resolved_path)},
                                {"message", "Replacement resource could not be imported"},
                            }})},
                        });
                    }
                    route = imported.main_route;
                    resolved_path = resolve_project_path(scene->project_root, route);
                } else {
                    route = path_to_utf8(relative);
                }
            } else {
                std::error_code ec;
                const auto relative = std::filesystem::relative(resolved_path, scene->project_root, ec);
                route = ec ? path_to_utf8(resolved_path) : path_to_utf8(relative);
            }

            const auto actor_index = static_cast<std::size_t>(actor - scene->actors.data());
            auto replacement_data = actor->persisted_snapshot;
            replacement_data["route"] = route;
            replacement_data["asset_path"] = path_to_utf8(resolved_path);
            replacement_data["transform"] = {
                {"position", actor->geometry ? actor->geometry->get_position() : actor->position},
                {"rotation", actor->geometry ? actor->geometry->get_rotation() : actor->rotation},
                {"scale", actor->geometry ? actor->geometry->get_scale() : actor->scale},
            };
            try {
                auto replacement = native_actor_from_snapshot(replacement_data);
                auto& loaded = add_native_actor_to_scene(*scene, std::move(replacement), resolved_path);
                loaded.load_status = ActorLoadStatus::Loaded;
                loaded.load_error_code.clear();
                loaded.load_error_message.clear();
                if (loaded.optics) loaded.optics->set_visible(loaded.persisted_visible);
                if (loaded.mechanics && loaded.actor_type != "ui_image") {
                    loaded.mechanics->set_physics_enabled(loaded.persisted_physics_enabled);
                    loaded.mechanics->set_collision_shape(loaded.persisted_collision_type);
                }
                if (scene->actors[actor_index].engine_actor) {
                    scene->engine_scene->remove_actor(scene->actors[actor_index].engine_actor.get());
                }
                scene->actors.erase(scene->actors.begin() + static_cast<std::ptrdiff_t>(actor_index));
                nlohmann::json retained_diagnostics = nlohmann::json::array();
                for (const auto& diagnostic : scene->load_diagnostics) {
                    const bool resolved_main_resource =
                        diagnostic.value("actor_guid", std::string{}) == actor_guid &&
                        diagnostic.value("code", std::string{}) != "ATTACHMENT_RESOURCE_NOT_FOUND";
                    if (!resolved_main_resource) retained_diagnostics.push_back(diagnostic);
                }
                scene->load_diagnostics = std::move(retained_diagnostics);
                persist_native_scene_actors(*scene);
                auto* rebound = find_native_actor_by_guid(*scene, actor_guid);
                return native_success({
                    {"ok", true}, {"status", "loaded"},
                    {"actor", rebound ? actor_to_json(*scene, *rebound) : nlohmann::json::object()},
                });
            } catch (const std::exception& error) {
                return native_success({
                    {"ok", false}, {"status", "resource_error"},
                    {"diagnostics", nlohmann::json::array({{
                        {"code", "MODEL_DECODE_FAILED"}, {"recoverable", true},
                        {"actor_guid", actor_guid}, {"path", path_to_utf8(resolved_path)},
                        {"message", error.what()},
                    }})},
                });
            }
        }},
        {"sun_direction", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            auto* scene = ensure_native_editor_scene();
            scene = resolve_native_editor_scene_request(scene, scene_route);

            scene->sun_enabled = arg_bool(request.args, 1, true);
            const auto direction_arg = request.args.is_array() && request.args.size() > 2
                                           ? request.args[2]
                                           : nlohmann::json::array();
            std::array<float, 3> direction{
                json_float_at(direction_arg, 0, scene->sun_direction[0]),
                json_float_at(direction_arg, 1, scene->sun_direction[1]),
                json_float_at(direction_arg, 2, scene->sun_direction[2]),
            };
            const float length_sq =
                direction[0] * direction[0] +
                direction[1] * direction[1] +
                direction[2] * direction[2];
            if (scene->sun_enabled && length_sq < 1.0e-8f) {
                direction = {1.0f, 1.0f, 1.0f};
            }
            scene->sun_direction = direction;

            apply_native_scene_environment(*scene);
            persist_native_scene_environment(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"sun", {{"enabled", scene->sun_enabled}, {"direction", scene->sun_direction}}},
            });
        }},
        {"floor_grid", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            auto* scene = ensure_native_editor_scene();
            scene = resolve_native_editor_scene_request(scene, scene_route);

            scene->floor_grid_enabled = arg_bool(request.args, 1, true);
            apply_native_scene_environment(*scene);
            persist_native_scene_environment(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"grid", {{"enabled", scene->floor_grid_enabled}}},
            });
        }},
        {"create_actor", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_route = arg_string(request.args, 0);
            const auto source_path = arg_string(request.args, 1);
            auto actor_type = normalize_route(arg_string(request.args, 2, "model"));
            const auto actor_data = arg_object(request.args, 3);
            return create_native_editor_actor(scene_route, source_path, actor_type, actor_data);
        }},
        {"rename_actor", [](const NativeRequest& request, const NativeContext& context) {
            auto* scene = scene_for_request_route(request);
            const auto actor_name = arg_string(request.args, 1);
            const auto new_name = trim_ascii(arg_string(request.args, 2));
            if (new_name.empty()) {
                return native_failure("Actor name cannot be empty", 2);
            }
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor) {
                return native_failure("Actor not found: " + actor_name, 2);
            }
            const auto duplicate = std::any_of(scene->actors.begin(), scene->actors.end(), [&](const NativeEditorActor& other) {
                return &other != actor && other.name == new_name;
            });
            if (duplicate) {
                return native_failure("Actor name already exists: " + new_name, 2);
            }
            const auto old_name = actor->name;
            actor->name = new_name;
            sync_native_actor_to_embedded_vision_document(*scene, *actor);
            persist_native_scene_actors(*scene);
            emit_actor_change(context, *scene, *actor);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"old_name", old_name},
                {"new_name", actor->name},
                {"actor", actor_to_json(*scene, *actor)},
            });
        }},
        {"list_camera_views", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            nlohmann::json cameras = nlohmann::json::array();
            for (const auto& camera : scene->cameras) {
                cameras.push_back(camera_to_json(camera));
            }
            return native_success({{"cameras", cameras}});
        }},
        {"is_vision_available", [](const NativeRequest&, const NativeContext&) {
            return native_success({{"available", Corona::API::is_vision_available()}});
        }},
        {"load_vision_scene", [](const NativeRequest& request, const NativeContext&) {
            Corona::API::load_vision_scene(arg_string(request.args, 0));
            return native_success({{"status", "success"}});
        }},
        {"select_screenshot_path", script_method},
        {"save_screenshot", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto output_path = arg_string(request.args, 1);
            if (output_path.empty()) {
                return native_failure("Screenshot path is required", 2);
            }

            const auto camera_name = arg_string(request.args, 2);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            camera->engine_camera->set_size(std::max(camera->width, 1), std::max(camera->height, 1));
            const bool saved = camera->engine_camera->save_screenshot_sync(output_path);
            if (!saved) {
                return native_failure("Screenshot save failed", 2);
            }

            return native_success({
                {"status", "success"},
                {"ok", true},
                {"scene", scene->route},
                {"path", output_path},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"set_render_backend", [](const NativeRequest& request, const NativeContext&) {
            const auto mode = arg_string(request.args, 0, "native");
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 1));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 2);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            camera->engine_camera->set_render_backend(mode);
            const auto actual = camera->engine_camera->get_render_backend();
            return native_success({
                {"status", "success"},
                {"mode", actual},
                {"fallback", mode == "vision" && actual != "vision"},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"get_render_backend", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            return native_success({
                {"status", "success"},
                {"mode", camera->engine_camera->get_render_backend()},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"set_vision_render_mode", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 1);
            const auto mode = arg_string(request.args, 2, "path_tracing");
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            camera->engine_camera->set_vision_render_mode(mode);
            return native_success({
                {"status", "success"},
                {"mode", camera->engine_camera->get_vision_render_mode()},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"get_vision_render_mode", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            return native_success({
                {"status", "success"},
                {"mode", camera->engine_camera->get_vision_render_mode()},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"set_ssat_view_viewer", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 1);
            const auto mode = arg_string(request.args, 2);
            if (mode != "interlaced" && mode != "final_view") {
                return native_failure(
                    "Invalid SSAT viewer mode: " + mode +
                        "; expected 'interlaced' or 'final_view'",
                    1);
            }
            if (request.args.size() <= 3 ||
                (request.args[3].is_number_integer() && request.args[3].get<int64_t>() < 0)) {
                return native_failure("SSAT viewer view_index must be a non-negative integer", 1);
            }
            const auto requested_index = std::min<std::uint64_t>(
                arg_uint64(request.args, 3, 0),
                std::numeric_limits<std::uint32_t>::max());
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            camera->engine_camera->set_ssat_view_viewer(
                mode, static_cast<std::uint32_t>(requested_index));
            const auto viewer = camera->engine_camera->get_ssat_view_viewer();
            return native_success({
                {"status", viewer.status},
                {"supported", viewer.supported},
                {"pending", viewer.pending},
                {"mode", viewer.mode},
                {"view_index", viewer.view_index},
                {"view_count", viewer.view_count},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"get_ssat_view_viewer", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto scene_route = normalize_route(arg_string(request.args, 0));
            scene = resolve_native_editor_scene_request(scene, scene_route);

            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            const auto viewer = camera->engine_camera->get_ssat_view_viewer();
            return native_success({
                {"status", viewer.status},
                {"supported", viewer.supported},
                {"pending", viewer.pending},
                {"mode", viewer.mode},
                {"view_index", viewer.view_index},
                {"view_count", viewer.view_count},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"set_output_mode", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            const auto mode = arg_string(request.args, 2, "final_color");
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            camera->engine_camera->set_output_mode(mode);
            return native_success({{"status", "success"}, {"mode", camera->engine_camera->get_output_mode()}});
        }},
        {"set_physics_params", script_method},
        {"get_output_mode", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            return native_success({{"status", "success"}, {"mode", camera->engine_camera->get_output_mode()}});
        }},
        {"get_physics_params", script_method},
        {"set_shadow_cascade_debug", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            const bool enabled = arg_bool(request.args, 2, false);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            camera->engine_camera->set_shadow_cascade_debug(enabled);
            return native_success({
                {"status", "success"},
                {"enabled", camera->engine_camera->get_shadow_cascade_debug()},
            });
        }},
        {"get_shadow_cascade_debug", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            return native_success({
                {"status", "success"},
                {"enabled", camera->engine_camera->get_shadow_cascade_debug()},
            });
        }},
        {"set_ssao_enabled", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            const bool enabled = arg_bool(request.args, 2, true);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            camera->engine_camera->set_ssao_enabled(enabled);
            return native_success({
                {"status", "success"},
                {"enabled", camera->engine_camera->get_ssao_enabled()},
            });
        }},
        {"get_ssao_enabled", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }
            return native_success({
                {"status", "success"},
                {"enabled", camera->engine_camera->get_ssao_enabled()},
            });
        }},
        {"open_actor", [](const NativeRequest& request, const NativeContext& context) {
            auto* scene = ensure_native_editor_scene();
            const auto actor_name = arg_string(request.args, 1);
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor) {
                return native_failure("Actor not found: " + actor_name, 2);
            }
            emit_actor_change(context, *scene, *actor);
            return native_success({{"status", "success"}, {"actor", actor_to_json(*scene, *actor)}});
        }},
        {"select_actor", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_name = arg_string(request.args, 0);
            const auto actor_type = arg_string(request.args, 1, "actor");
            const auto actor_name = arg_string(request.args, 2);
            nlohmann::json payload = {
                {"actor_type", actor_type},
                {"scene", scene_name},
                {"actor", actor_name},
            };
            emit_editor_api_event("SceneTools.actorSelectionChanged", payload);
            return native_success({
                {"status", "success"},
                {"scene", scene_name},
                {"actor_type", actor_type},
                {"actor", actor_name},
            });
        }},
        {"open_camera_view", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return camera_not_found_result(camera_name);
            }
            camera->view_open = true;
            camera->engine_camera->set_view_state(true, camera->view_x, camera->view_y,
                                                  camera->view_width, camera->view_height,
                                                  camera->move_speed);
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"focus_actor", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto actor_name = arg_string(request.args, 1);
            const auto camera_name = arg_string(request.args, 2);
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor || !actor->geometry) {
                return native_failure("Actor not found or has no geometry: " + actor_name, 2);
            }
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return native_failure("Camera not found: " + camera_name, 2);
            }

            const auto aabb = actor->geometry->get_aabb();
            const auto pos = actor->geometry->get_position();
            const auto scale = actor->geometry->get_scale();
            const std::array<float, 3> center{
                pos[0] + ((aabb[0] + aabb[3]) * 0.5f) * scale[0],
                pos[1] + ((aabb[1] + aabb[4]) * 0.5f) * scale[1],
                pos[2] + ((aabb[2] + aabb[5]) * 0.5f) * scale[2],
            };
            const float dx = std::abs((aabb[3] - aabb[0]) * scale[0]);
            const float dy = std::abs((aabb[4] - aabb[1]) * scale[1]);
            const float dz = std::abs((aabb[5] - aabb[2]) * scale[2]);
            const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float distance = std::max(diagonal * 2.0f, 1.0f);
            const std::array<float, 3> camera_position{center[0], center[1], center[2] - distance};
            const std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
            const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
            camera->engine_camera->set(camera_position, forward, up, camera->engine_camera->get_fov());
            return native_success({
                {"status", "success"},
                {"center", center},
                {"distance", distance},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"remove_actor", [](const NativeRequest& request, const NativeContext&) {
            const auto scene_route = arg_string(request.args, 0);
            const auto actor_name = arg_string(request.args, 1);
            return remove_native_editor_actor(scene_route, actor_name);
        }},
        {"rename_camera_view", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto camera_name = arg_string(request.args, 1);
            const auto new_name = trim_ascii(arg_string(request.args, 2));
            if (new_name.empty()) {
                return native_failure("Camera name cannot be empty", 2);
            }
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return camera_not_found_result(camera_name);
            }
            const bool duplicate = std::any_of(scene->cameras.begin(), scene->cameras.end(), [&](const NativeEditorCamera& other) {
                return &other != camera && other.name == new_name;
            });
            if (duplicate) {
                return native_failure("Camera name already exists: " + new_name, 2);
            }
            camera->name = new_name;
            if (camera->camera_id.empty() || camera->camera_id == scene->route + "#" + camera_name) {
                camera->camera_id = scene->route + "#" + new_name;
            }
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera", camera_to_json(*camera)},
            });
        }},
        {"pick_actor_at_pixel", [](const NativeRequest& request, const NativeContext& context) {
            auto* scene = ensure_native_editor_scene();
            auto* camera = find_native_camera(*scene, {});
            if (!camera || !camera->engine_camera) {
                return native_failure("No active camera available", 2);
            }
            const float x = arg_float_value(request.args, 1);
            const float y = arg_float_value(request.args, 2);
            const float viewport_width = arg_float_value(request.args, 3, static_cast<float>(camera->width));
            const float viewport_height = arg_float_value(request.args, 4, static_cast<float>(camera->height));
            if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
                return native_failure("Invalid viewport size", 2);
            }
            const int pick_x = static_cast<int>(x * static_cast<float>(camera->width) / viewport_width);
            const int pick_y = static_cast<int>(y * static_cast<float>(camera->height) / viewport_height);
            if (pick_x < 0 || pick_x >= camera->width || pick_y < 0 || pick_y >= camera->height) {
                return native_success({{"status", "miss"}});
            }
            const auto handle = camera->engine_camera->pick_actor_at_pixel(pick_x, pick_y);
            if (handle == 0) {
                return native_success({{"status", "pending"}});
            }
            auto* actor = find_native_actor(*scene, std::to_string(handle));
            if (!actor) {
                return native_success({{"status", "miss"}, {"handle", handle}});
            }
            emit_actor_change(context, *scene, *actor);
            return native_success({
                {"status", "success"},
                {"actor", actor_to_json(*scene, *actor)},
            });
        }},
        {"play_audio", [](const NativeRequest& request, const NativeContext&) {
            auto* event_bus = Corona::Kernel::KernelContext::instance().event_bus();
            if (!event_bus) {
                return native_failure("event_bus unavailable", 2);
            }
            const uint64_t rid = arg_uint64(request.args, 0);
            if (rid == 0) {
                return native_failure("invalid resource_id", 2);
            }
            const bool loop = arg_bool(request.args, 1, false);
            event_bus->publish<::Corona::Events::PlayAudioEvent>({rid, loop});

            nlohmann::json payload;
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"stop_audio", [](const NativeRequest& request, const NativeContext&) {
            auto* event_bus = Corona::Kernel::KernelContext::instance().event_bus();
            if (!event_bus) {
                return native_failure("event_bus unavailable", 2);
            }
            const uint64_t rid = arg_uint64(request.args, 0);
            if (rid == 0) {
                return native_failure("invalid resource_id", 2);
            }
            event_bus->publish<::Corona::Events::StopAudioEvent>({rid});

            nlohmann::json payload;
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"actor_play_audio", [](const NativeRequest& request, const NativeContext&) {
            // 在指定 actor 的世界位置播放其绑定的音频（空间音频）。
            auto* scene = ensure_native_editor_scene();
            const auto actor_name = arg_string(request.args, 0);
            const bool loop = arg_bool(request.args, 1, false);
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor || !actor->acoustics) {
                return native_failure("actor not found or has no acoustics component", 2);
            }
            actor->acoustics->play(loop);

            nlohmann::json payload;
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"actor_stop_audio", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = ensure_native_editor_scene();
            const auto actor_name = arg_string(request.args, 0);
            auto* actor = find_native_actor(*scene, actor_name);
            if (!actor || !actor->acoustics) {
                return native_failure("actor not found or has no acoustics component", 2);
            }
            actor->acoustics->stop();

            nlohmann::json payload;
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"update_camera_view", [](const NativeRequest& request, const NativeContext&) {
            auto* scene = scene_for_request_route(request);
            const auto camera_name = arg_string(request.args, 1);
            auto* camera = find_native_camera(*scene, camera_name);
            if (!camera || !camera->engine_camera) {
                return camera_not_found_result(camera_name);
            }
            const auto state = request.args.is_array() && request.args.size() > 2 && request.args[2].is_object()
                                   ? request.args[2]
                                   : nlohmann::json::object();
            camera->view_open = json_bool_value(state, "view_open", camera->view_open);
            camera->view_x = json_int_value(state, "view_x", camera->view_x);
            camera->view_y = json_int_value(state, "view_y", camera->view_y);
            camera->view_width = std::max(json_int_value(state, "view_width", camera->view_width), 1);
            camera->view_height = std::max(json_int_value(state, "view_height", camera->view_height), 1);
            camera->move_speed = json_float_value(state, "move_speed", camera->move_speed);
            camera->width = std::max(json_int_value(state, "width", camera->width), 1);
            camera->height = std::max(json_int_value(state, "height", camera->height), 1);
            camera->engine_camera->set_size(camera->width, camera->height);
            camera->engine_camera->set_view_state(camera->view_open, camera->view_x, camera->view_y,
                                                  camera->view_width, camera->view_height,
                                                  camera->move_speed);
            persist_native_scene_cameras(*scene);
            return native_success({
                {"status", "success"},
                {"scene", scene->route},
                {"camera", camera_to_json(*camera)},
            });
        }},
    };

    registry.register_module("SceneTools", [](const NativeRequest& request,
                                              const NativeContext& context) {
        const auto it = methods.find(request.function);
        if (it == methods.end()) {
            return native_unhandled();
        }
        try {
            return it->second(request, context);
        } catch (const std::exception& e) {
            return native_failure(e.what(), 2);
        } catch (...) {
            return native_failure("SceneTools native handler error", 2);
        }
    });
}

void register_editor_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"register_callback", [](const NativeRequest& request, const NativeContext& context) {
            const auto event_name = arg_string(request.args, 0);
            const auto callback_spec = arg_object(request.args, 1);
            const auto token = EditorApiCallbackRegistry::instance().register_cef_callback(
                event_name,
                callback_spec,
                context);
            if (token == 0) {
                return native_failure(event_name + " is not a defined Editor API event", 404);
            }
            return native_success({{"callback_token", token}}, "editor-api-callback");
        }},
        {"unregister_callback", [](const NativeRequest& request, const NativeContext&) {
            const auto token = arg_uint64(request.args, 0);
            const auto removed = EditorApiCallbackRegistry::instance().unregister(token);
            return native_success({{"removed", removed}}, "editor-api-callback");
        }},
        {"list_methods", [](const NativeRequest&, const NativeContext&) {
            nlohmann::json methods_json = nlohmann::json::array();
            for (const auto& spec : EditorApiRegistry::instance().list_methods()) {
                methods_json.push_back(editor_api_method_to_json(spec));
            }
            return native_success({
                {"methods", methods_json},
            });
        }},
        {"list_events", [](const NativeRequest&, const NativeContext&) {
            nlohmann::json events_json = nlohmann::json::array();
            for (const auto& spec : EditorApiRegistry::instance().list_events()) {
                events_json.push_back(editor_api_event_to_json(spec));
            }
            return native_success({
                {"events", events_json},
            });
        }},
    };

    registry.register_module("EditorApi", [](const NativeRequest& request,
                                             const NativeContext& context) {
        return dispatch_method("EditorApi", methods, request, context);
    });
}

void register_python_script_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable ai_tool_methods = {
        {"generate_hint", script_method},
        {"read_local_file_as_base64", script_method},
        {"send_message_to_ai_stream", script_method},
        {"submit_request", script_method},
    };
    static const NativeMethodTable corona_editor_methods = {
        {"close_process", script_method},
    };
    static const NativeMethodTable file_manager_methods = {
        {"create_file", script_method},
        {"create_folder", script_method},
        {"delete_item", script_method},
        {"get_file_tree", script_method},
        {"get_files", script_method},
        {"get_project_info", script_method},
        {"open_file", script_method},
        {"rename_item", script_method},
    };
    static const NativeMethodTable resource_search_methods = {
        {"focus_actor", script_method},
        {"fuzzy_search", script_method},
        {"get_stats", script_method},
        {"image_search", script_method},
        {"list_types", script_method},
        {"mark_index_dirty", script_method},
        {"prepare_index", script_method},
        {"rebuild_index", script_method},
    };
    static const NativeMethodTable scratch_tool_methods = {
        {"execute_python_code", script_method},
        {"get_game_preview_status", script_method},
        {"get_script_status", script_method},
        {"key_event", script_method},
        {"key_release", script_method},
        {"load_blockly_target", script_method},
        {"mouse_event", script_method},
        {"save_blockly_target", script_method},
        {"start_game_preview", script_method},
        {"stop_game_preview", script_method},
        {"stop_script_execution", script_method},
    };

    auto register_script_module = [&](const char* module, const NativeMethodTable& methods) {
        const auto* method_table = &methods;
        registry.register_module(module, [module, method_table](const NativeRequest& request,
                                                                const NativeContext& context) {
            return dispatch_method(module, *method_table, request, context);
        });
    };

    register_script_module("AITool", ai_tool_methods);
    register_script_module("CoronaEditor", corona_editor_methods);
    register_script_module("FileManager", file_manager_methods);
    register_script_module("ResourceSearch", resource_search_methods);
    register_script_module("ScratchTool", scratch_tool_methods);
}

void register_network_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"start_session", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const std::string name = arg_string(request.args, 0);
            const uint64_t project_id = arg_uint64(request.args, 1);
            const uint16_t port = arg_uint16(request.args, 2, 27960);
            const auto role = request.args.size() > 3
                ? parse_network_session_role(request.args[3])
                : Corona::Systems::NetworkSystem::SessionRole::Host;

            const bool ok = sys->start_session(name, project_id, port, role);
            auto payload = build_network_session_info(sys);
            payload["ok"] = ok;
            return native_success(payload);
        }},
        {"stop_session", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->stop_session();
            return native_success({{"ok", true}});
        }},
        {"get_peer_count", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success(build_network_session_info(sys));
        }},
        {"get_session_info", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success(build_network_session_info(sys));
        }},
        {"connect_to_peer", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const std::string ip = arg_string(request.args, 0);
            const uint16_t port = arg_uint16(request.args, 1, 27960);
            const std::string peer_name = arg_string(request.args, 2);
            const bool ok = sys->connect_to_peer(ip, port, peer_name);
            auto payload = build_network_session_info(sys);
            payload["ok"] = ok;
            return native_success(payload);
        }},
        {"set_project_root", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const std::string root = arg_string(request.args, 0);
            if (!root.empty()) {
                sys->set_project_root(root);
            }
            return native_success({{"ok", true}});
        }},
        {"poll_pending_actor_create", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string actor_guid, scene_name, model_path, actor_json;
            Corona::Network::ActorCreatePacked packed;
            if (sys->pop_pending_actor_create(actor_guid, scene_name, model_path,
                                               &packed, sizeof(packed), &actor_json)) {
                payload["has_pending"] = true;
                payload["actor_guid"] = actor_guid;
                payload["scene_name"] = scene_name;
                payload["model_path"] = model_path;
                nlohmann::json actor_data = nlohmann::json::object();
                if (!actor_json.empty()) {
                    try {
                        actor_data = nlohmann::json::parse(actor_json);
                        if (!actor_data.is_object()) {
                            actor_data = nlohmann::json::object();
                        }
                    } catch (const nlohmann::json::parse_error&) {
                        actor_data = nlohmann::json::object();
                    }
                }
                actor_data["geometry"]["position"] = {
                    packed.transform[0], packed.transform[1], packed.transform[2]
                };
                actor_data["geometry"]["rotation"] = {
                    packed.transform[3], packed.transform[4], packed.transform[5]
                };
                actor_data["geometry"]["scale"] = {
                    packed.transform[6], packed.transform[7], packed.transform[8]
                };
                payload["actor_data"] = actor_data;
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"poll_pending_actor_transform", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string actor_guid, scene_name, source_user_id, correlation_id;
            float transform[9] = {0,0,0, 0,0,0, 1,1,1};
            if (sys->pop_pending_actor_transform_update(
                    actor_guid, scene_name, transform, 9,
                    source_user_id, correlation_id)) {
                payload["has_pending"] = true;
                payload["actor_guid"] = actor_guid;
                payload["scene_name"] = scene_name;
                payload["source_user_id"] = source_user_id;
                payload["correlation_id"] = correlation_id;
                payload["geometry"]["position"] = {transform[0], transform[1], transform[2]};
                payload["geometry"]["rotation"] = {transform[3], transform[4], transform[5]};
                payload["geometry"]["scale"] = {transform[6], transform[7], transform[8]};
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"poll_pending_actor_delete", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string actor_guid, scene_name, actor_name;
            if (sys->pop_pending_actor_delete(actor_guid, scene_name, actor_name)) {
                payload["has_pending"] = true;
                payload["actor_guid"] = actor_guid;
                payload["scene_name"] = scene_name;
                payload["actor_name"] = actor_name;
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"poll_pending_actor_scene_snapshot_request", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string scene_name;
            if (sys->pop_pending_actor_scene_snapshot_request(scene_name)) {
                payload["has_pending"] = true;
                payload["scene_name"] = scene_name;
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"poll_pending_actor_scene_snapshot", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string scene_name, snapshot_json;
            if (sys->pop_pending_actor_scene_snapshot(scene_name, snapshot_json)) {
                payload["has_pending"] = true;
                payload["scene_name"] = scene_name;
                payload["snapshot_json"] = snapshot_json;
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"poll_pending_actor_state_update", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            nlohmann::json payload;
            std::string actor_guid, scene_name, actor_json;
            if (sys->pop_pending_actor_state_update(actor_guid, scene_name, actor_json)) {
                payload["has_pending"] = true;
                payload["actor_guid"] = actor_guid;
                payload["scene_name"] = scene_name;
                payload["actor_json"] = actor_json;
            } else {
                payload["has_pending"] = false;
            }
            payload["ok"] = true;
            return native_success(payload);
        }},
        {"set_sync_paused", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->set_sync_paused(arg_bool(request.args, 0, false));
            return native_success({{"ok", true}});
        }},
        {"register_actor_identity", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const std::string actor_guid = arg_string(request.args, 0);
            const std::uintptr_t actor_handle = arg_uintptr(request.args, 1);
            const bool locally_owned = arg_bool(request.args, 2, true);
            const bool ok = sys->register_actor_identity(actor_guid, actor_handle, locally_owned);
            return native_success({{"ok", ok}});
        }},
        {"claim_actor_ownership", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const bool ok = sys->claim_actor_ownership(arg_string(request.args, 0));
            return native_success({{"ok", ok}});
        }},
        {"broadcast_actor_transform", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const std::string actor_guid = arg_string(request.args, 0);
            const std::string scene_name = arg_string(request.args, 1);
            float transform[9] = {0,0,0, 0,0,0, 1,1,1};
            std::string source_user_id;
            std::string correlation_id;
            const auto actor_data = arg_object(request.args, 2);
            if (!actor_data.empty()) {
                read_transform_from_actor_json(actor_data, transform);
                source_user_id = actor_data.value("source_user_id", "");
                correlation_id = actor_data.value("correlation_id", "");
            }
            sys->broadcast_actor_transform_update(
                actor_guid, scene_name, transform, source_user_id, correlation_id);
            return native_success({{"ok", true}});
        }},
        {"broadcast_actor_delete", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->broadcast_actor_delete(
                arg_string(request.args, 0),
                arg_string(request.args, 1),
                arg_string(request.args, 2));
            return native_success({{"ok", true}});
        }},
        {"request_actor_scene_snapshot", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->request_actor_scene_snapshot(arg_string(request.args, 0));
            return native_success({{"ok", true}});
        }},
        {"broadcast_actor_scene_snapshot", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            std::string snapshot_json;
            if (request.args.size() > 1) {
                snapshot_json = request.args[1].is_string()
                    ? request.args[1].get<std::string>()
                    : request.args[1].dump();
            }
            sys->broadcast_actor_scene_snapshot(arg_string(request.args, 0), snapshot_json);
            return native_success({{"ok", true}});
        }},
        {"broadcast_actor_state_update", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            std::string actor_json;
            if (request.args.size() > 2) {
                actor_json = request.args[2].is_string()
                    ? request.args[2].get<std::string>()
                    : request.args[2].dump();
            }
            sys->broadcast_actor_state_update(
                arg_string(request.args, 0),
                arg_string(request.args, 1),
                actor_json);
            return native_success({{"ok", true}});
        }},
        {"broadcast_actor_create", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            std::string actor_guid = arg_string(request.args, 0);
            const std::string scene_name = arg_string(request.args, 1);
            const std::string model_path = arg_string(request.args, 2);
            float transform[9] = {0,0,0, 0,0,0, 1,1,1};
            std::vector<std::string> dependency_paths;
            std::string actor_json;
            const auto actor_data = arg_object(request.args, 3);
            if (!actor_data.empty()) {
                actor_json = actor_data.dump();
                if (actor_guid.empty() &&
                    actor_data.contains("actor_guid") &&
                    actor_data["actor_guid"].is_string()) {
                    actor_guid = actor_data["actor_guid"].get<std::string>();
                }
                if (actor_data.contains("model_dependencies") &&
                    actor_data["model_dependencies"].is_array()) {
                    for (const auto& dep : actor_data["model_dependencies"]) {
                        if (dep.is_string()) {
                            dependency_paths.push_back(dep.get<std::string>());
                        }
                    }
                }
                read_transform_from_actor_json(actor_data, transform);
            }
            if (actor_guid.empty()) {
                actor_guid = scene_name + ":" + model_path;
            }

            Corona::Network::ActorCreatePacked opt;
            std::memset(&opt, 0, sizeof(opt));
            opt.visible = true;
            opt.bEnableLighting = true;
            opt.metallic = 0.0f;
            opt.roughness = 0.5f;
            opt.specular = 0.5f;
            opt.specularTint = 0.0f;
            opt.sheen = 0.0f;
            opt.sheenTint = 0.5f;
            opt.clearcoat = 0.0f;
            opt.clearcoatGloss = 1.0f;
            opt.ambient[0] = 0.2f; opt.ambient[1] = 0.2f; opt.ambient[2] = 0.2f;
            opt.diffuse[0] = 0.8f; opt.diffuse[1] = 0.8f; opt.diffuse[2] = 0.8f;
            opt.specular_color[0] = 1.0f; opt.specular_color[1] = 1.0f; opt.specular_color[2] = 1.0f;
            opt.shininess = 32.0f;

            sys->broadcast_actor_create(actor_guid, scene_name, model_path,
                                        dependency_paths, transform,
                                        &opt, sizeof(opt), actor_json);
            return native_success({{"ok", true}});
        }},
    };

    registry.register_module("Network", [](const NativeRequest& request,
                                           const NativeContext& context) {
        return dispatch_method("Network", methods, request, context);
    });
}

void register_lanchat_api_handlers(NativeApiRegistry& registry) {
    static const NativeMethodTable methods = {
        {"start_room", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->set_lanchat_event_callback(emit_lanchat_event_json);
            const auto payload_arg = arg_object(request.args, 0);
            const std::string room = payload_arg.value("room", "");
            const uint16_t port = payload_arg.value("port", 27960);
            const std::string nickname = payload_arg.value("nickname", "房主");
            const bool restore_history = payload_arg.value("restore_history", false);
            const std::string history_room = payload_arg.value("history_room", room);
            const std::string host_nickname = nickname.empty() ? "房主" : nickname;
            const bool ok = sys->lanchat_start_room(room, host_nickname, port);
            bool restored_history = false;
            if (ok && restore_history) {
                restored_history = sys->lanchat_restore_history_room(history_room);
            }
            const uint16_t actual_port = sys->session_port() != 0 ? sys->session_port() : port;
            nlohmann::json data;
            data["ok"] = ok;
            data["you"] = host_nickname;
            data["ip"] = detect_wlan_ipv4();
            data["port"] = actual_port;
            data["room"] = room;
            data["peer_id"] = sys->local_peer_id();
            data["members"] = build_lanchat_members(sys->lanchat_members());
            data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
            data["history"] = build_lanchat_history(sys->lanchat_history());
            data["agents"] = build_lanchat_agents(sys->lanchat_agents());
            data["restored_history"] = restored_history;
            return native_success(data);
        }},
        {"start_local_room", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->set_lanchat_event_callback(emit_lanchat_event_json);
            const auto payload_arg = arg_object(request.args, 0);
            const std::string room = payload_arg.value("room", "");
            const std::string nickname = payload_arg.value("nickname", "房主");
            const bool restore_history = payload_arg.value("restore_history", false);
            const std::string history_room = payload_arg.value("history_room", room);
            const std::string host_nickname = nickname.empty() ? "房主" : nickname;
            const bool ok = sys->lanchat_start_local_room(room, host_nickname);
            bool restored_history = false;
            if (ok && restore_history) {
                restored_history = sys->lanchat_restore_history_room(history_room);
            }
            nlohmann::json data;
            data["ok"] = ok;
            data["you"] = host_nickname;
            data["ip"] = "";
            data["port"] = 0;
            data["room"] = room;
            data["mode"] = "single";
            data["peer_id"] = "local-single-player";
            data["members"] = build_lanchat_members(sys->lanchat_members());
            data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
            data["history"] = build_lanchat_history(sys->lanchat_history());
            data["agents"] = build_lanchat_agents(sys->lanchat_agents());
            data["restored_history"] = restored_history;
            return native_success(data);
        }},
        {"stop_room", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->lanchat_leave_room();
            sys->stop_session();
            return native_success({{"ok", true}});
        }},
        {"stop_local_room", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->lanchat_stop_local_room();
            return native_success({{"ok", true}});
        }},
        {"get_history", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success({
                {"ok", true},
                {"history", build_lanchat_history(sys->lanchat_history())},
            });
        }},
        {"list_history_rooms", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success({
                {"ok", true},
                {"rooms", build_lanchat_history_rooms(sys->lanchat_history_rooms())},
            });
        }},
        {"load_history_room", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const auto payload_arg = arg_object(request.args, 0);
            const std::string room = payload_arg.value("room", "");
            nlohmann::json data;
            data["ok"] = !room.empty();
            data["room"] = room;
            data["history"] = room.empty()
                ? nlohmann::json::array()
                : build_lanchat_history(sys->lanchat_load_history_room(room));
            data["agents"] = room.empty()
                ? nlohmann::json::array()
                : build_lanchat_agents(sys->lanchat_load_history_agents(room));
            if (room.empty()) {
                data["error"] = "ROOM_REQUIRED";
            }
            return native_success(data);
        }},
        {"join_room", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->set_lanchat_event_callback(emit_lanchat_event_json);
            const auto payload_arg = arg_object(request.args, 0);
            const std::string ip = payload_arg.value("ip", "");
            const uint16_t port = payload_arg.value("port", 27960);
            const std::string room = payload_arg.value("room", "");
            const std::string nickname = payload_arg.value("nickname", "Guest");
            const bool ok = sys->lanchat_join_room(ip, port, room, nickname);
            nlohmann::json data;
            data["ok"] = ok;
            data["you"] = nickname;
            data["peer_id"] = sys->local_peer_id();
            data["port"] = sys->host_port() != 0 ? sys->host_port() : port;
            data["members"] = build_lanchat_members(sys->lanchat_members());
            data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
            data["history"] = build_lanchat_history(sys->lanchat_history());
            data["agents"] = build_lanchat_agents(sys->lanchat_agents());
            if (!ok) {
                data["error"] = "JOIN_FAILED";
            }
            return native_success(data);
        }},
        {"leave_room", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            sys->lanchat_leave_room();
            return native_success({{"ok", true}});
        }},
        {"send_message", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const auto payload_arg = arg_object(request.args, 0);
            const std::string text = payload_arg.value("text", "");
            const std::string message_kind = payload_arg.value("message_kind", "chat");
            const std::string target_agent_id = payload_arg.value("target_agent_id", "");
            const std::string source_user_id = payload_arg.value("source_user_id", "");
            const std::string correlation_id = payload_arg.value("correlation_id", "");
            std::string metadata_json;
            if (payload_arg.contains("metadata_json")) {
                metadata_json = payload_arg.value("metadata_json", "");
            } else if (payload_arg.contains("metadata")) {
                metadata_json = payload_arg["metadata"].dump();
            }
            const auto result = sys->lanchat_send_message_ex(
                text, message_kind, target_agent_id, source_user_id,
                correlation_id, metadata_json);
            nlohmann::json data;
            data["ok"] = result.accepted;
            if (result.accepted) {
                data["message_id"] = result.message.message_id;
                data["seq"] = result.message.seq;
            } else {
                data["error"] = result.error.empty() ? "SEND_FAILED" : result.error;
            }
            return native_success(data);
        }},
        {"add_agent", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const auto payload_arg = arg_object(request.args, 0);
            const std::string name = payload_arg.value("name", "Agent");
            const std::string persona = payload_arg.value("persona", "");
            const std::string peer_id = sys->local_peer_id().empty()
                ? "local-single-player"
                : sys->local_peer_id();
            const std::string agent_id = make_agent_id(peer_id, name);
            const auto result = sys->lanchat_register_agent(agent_id, name, persona);
            nlohmann::json data;
            data["ok"] = result.ok;
            data["agent_id"] = agent_id;
            data["name"] = name;
            if (!result.ok) {
                data["error"] = result.error;
            }
            return native_success(data);
        }},
        {"remove_agent", [](const NativeRequest& request, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            const auto payload_arg = arg_object(request.args, 0);
            const auto result = sys->lanchat_remove_agent(payload_arg.value("agent_id", ""));
            nlohmann::json data;
            data["ok"] = result.ok;
            if (!result.ok) {
                data["error"] = result.error;
            }
            return native_success(data);
        }},
        {"list_agents", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success({
                {"ok", true},
                {"agents", build_lanchat_agents(sys->lanchat_agents())},
            });
        }},
        {"get_local_ip", [](const NativeRequest&, const NativeContext&) {
            auto sys = require_network_system();
            if (!sys) {
                return native_failure("NetworkSystem unavailable", 2);
            }
            return native_success({
                {"ok", true},
                {"ip", detect_wlan_ipv4()},
                {"port", sys->session_port() != 0 ? sys->session_port() : 27960},
            });
        }},
    };

    registry.register_module("LANChat", [](const NativeRequest& request,
                                           const NativeContext& context) {
        return dispatch_method("LANChat", methods, request, context);
    });
}

}  // namespace Corona::Systems::UI
