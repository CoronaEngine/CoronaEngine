#include "editor_archive_snapshot.h"

#include "animation_config.h"
#include "editor_actor_projection.h"
#include "ik_chain_config.h"
#include "model_node_config.h"
#include "native_material_config.h"
#include "editor_paths.h"
#include "editor_request_values.h"
#include "editor_scene_access.h"
#include "editor_text_utils.h"

#include <corona/resource/types/scene.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Corona::Systems::UI {

namespace {

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

}  // namespace

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

}
  // namespace Corona::Systems::UI
