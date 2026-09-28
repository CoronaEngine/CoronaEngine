/// @file actor_cache.cpp
/// @brief ActorCache 实现 + ActorStreamingRecord JSON 序列化

#include <corona/systems/geometry/actor_cache.h>

#include <horizon/core/logging.h>

#include <nlohmann/json.hpp>

#include <sstream>
#include <cstdlib>

namespace Corona::Cache {

namespace {

/// 将 std::uintptr_t 格式化为 "0x" + 十六进制
[[nodiscard]] std::string handle_to_hex(std::uintptr_t h) {
    std::ostringstream oss;
    oss << "0x" << std::hex << h;
    return oss.str();
}

/// 将 std::uint64_t 格式化为 "0x" + 十六进制
[[nodiscard]] std::string rid_to_hex(std::uint64_t rid) {
    std::ostringstream oss;
    oss << "0x" << std::hex << rid;
    return oss.str();
}

/// 从十六进制字符串解析 std::uintptr_t（可选 "0x" 前缀）
[[nodiscard]] std::uintptr_t parse_hex_ptr(const std::string& s) {
    if (s.empty()) return 0;
    const char* start = s.c_str();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        start = s.c_str() + 2;
    return static_cast<std::uintptr_t>(std::strtoull(start, nullptr, 16));
}

/// 从十六进制字符串解析 std::uint64_t（可选 "0x" 前缀）
[[nodiscard]] std::uint64_t parse_hex_u64(const std::string& s) {
    if (s.empty()) return 0;
    const char* start = s.c_str();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        start = s.c_str() + 2;
    return std::strtoull(start, nullptr, 16);
}

}  // namespace

// ============================================================================
// ActorStreamingRecord JSON 序列化
// ============================================================================

std::string ActorStreamingRecord::to_json() const {
    nlohmann::json j;

    // 身份句柄（hex，仅用于诊断）
    j["scene"] = handle_to_hex(scene);
    j["actor"] = handle_to_hex(actor);

    // model_path
    j["model_path"] = model_path.u8string();

    // profile_handles → hex 字符串数组
    auto& j_profiles = (j["profile_handles"] = nlohmann::json::array());
    for (auto h : profile_handles)
        j_profiles.push_back(handle_to_hex(h));

    // geometry_handles → hex 字符串数组
    auto& j_geoms = (j["geometry_handles"] = nlohmann::json::array());
    for (auto h : geometry_handles)
        j_geoms.push_back(handle_to_hex(h));

    // resource_ids → hex 字符串数组
    auto& j_rids = (j["resource_ids"] = nlohmann::json::array());
    for (auto rid : resource_ids)
        j_rids.push_back(rid_to_hex(rid));

    // ModelTransform（沿用现有扁平键名，与 v1 兼容）
    j["pos_x"] = transform.position.x;
    j["pos_y"] = transform.position.y;
    j["pos_z"] = transform.position.z;
    j["rot_x"] = transform.euler_rotation.x;
    j["rot_y"] = transform.euler_rotation.y;
    j["rot_z"] = transform.euler_rotation.z;
    j["scl_x"] = transform.scale.x;
    j["scl_y"] = transform.scale.y;
    j["scl_z"] = transform.scale.z;

    // 布尔标志
    j["body_type"] = std::string(body_type_to_string(body_type));
    j["optics_visible"]  = optics_visible;
    j["follow_camera"]   = follow_camera;
    j["pinned"]          = pinned;

    // 优先级
    j["priority"] = priority;

    return j.dump();
}

std::optional<ActorStreamingRecord> ActorStreamingRecord::from_json(const std::string& json) {
    try {
        nlohmann::json j = nlohmann::json::parse(json);
        ActorStreamingRecord rec;

        rec.scene = parse_hex_ptr(j.value("scene", "0x0"));
        rec.actor = parse_hex_ptr(j.value("actor", "0x0"));

        rec.model_path = j.value("model_path", "");

        if (j.contains("profile_handles") && j["profile_handles"].is_array()) {
            for (const auto& v : j["profile_handles"])
                rec.profile_handles.push_back(parse_hex_ptr(v.get<std::string>()));
        }
        if (j.contains("geometry_handles") && j["geometry_handles"].is_array()) {
            for (const auto& v : j["geometry_handles"])
                rec.geometry_handles.push_back(parse_hex_ptr(v.get<std::string>()));
        }
        if (j.contains("resource_ids") && j["resource_ids"].is_array()) {
            for (const auto& v : j["resource_ids"])
                rec.resource_ids.push_back(parse_hex_u64(v.get<std::string>()));
        }

        // Transform
        rec.transform.position.x        = j.value("pos_x", 0.0f);
        rec.transform.position.y        = j.value("pos_y", 0.0f);
        rec.transform.position.z        = j.value("pos_z", 0.0f);
        rec.transform.euler_rotation.x  = j.value("rot_x", 0.0f);
        rec.transform.euler_rotation.y  = j.value("rot_y", 0.0f);
        rec.transform.euler_rotation.z  = j.value("rot_z", 0.0f);
        rec.transform.scale.x           = j.value("scl_x", 1.0f);
        rec.transform.scale.y           = j.value("scl_y", 1.0f);
        rec.transform.scale.z           = j.value("scl_z", 1.0f);

        // body_type：优先读新字段；旧存档只有 physics_enabled bool 时做迁移
        if (j.contains("body_type") && j["body_type"].is_string()) {
            rec.body_type = body_type_from_string(j["body_type"].get<std::string>());
        } else {
            // 向后兼容：physics_enabled=true → Dynamic, false → Static
            rec.body_type = j.value("physics_enabled", false) ? BodyType::Dynamic : BodyType::Static;
        }
        rec.optics_visible  = j.value("optics_visible", true);
        rec.follow_camera   = j.value("follow_camera", false);
        rec.pinned          = j.value("pinned", false);
        rec.priority        = j.value("priority", 0.0f);

        return rec;
    } catch (const nlohmann::json::exception& e) {
        CFW_LOG_ERROR("[ActorCache] Failed to parse ActorStreamingRecord JSON: {}", e.what());
        return std::nullopt;
    }
}

// ============================================================================
// ActorCache
// ============================================================================

ActorCache::ActorCache(size_t mem_capacity, size_t disk_capacity,
                       std::filesystem::path disk_dir)
    : cache_(mem_capacity, disk_capacity, std::move(disk_dir)) {}

std::string ActorCache::make_key(std::uintptr_t handle) {
    // 将指针地址格式化为 16 进制字符串
    std::ostringstream oss;
    oss << "actor_0x" << std::hex << handle;
    return oss.str();
}

std::uintptr_t ActorCache::parse_key(const std::string& key) {
    // 反向解析 make_key 产生的 key
    // 格式: "actor_0x<hex>"
    auto pos = key.find("0x");
    if (pos == std::string::npos) return 0;
    try {
        return static_cast<std::uintptr_t>(std::stoull(key.substr(pos + 2), nullptr, 16));
    } catch (...) {
        return 0;
    }
}

bool ActorCache::put(std::uintptr_t actor_handle, const ActorStreamingRecord& record) {
    std::string key = make_key(actor_handle);
    std::string json = record.to_json();
    CFW_LOG_NOTICE("[ActorCache] Storing actor 0x{:x} ({} bytes)", actor_handle, json.size());
    return cache_.put(key, json.data(), json.size());
}

std::optional<ActorStreamingRecord> ActorCache::get(std::uintptr_t actor_handle) {
    std::string key = make_key(actor_handle);
    auto item = cache_.get(key);
    if (!item) return std::nullopt;

    std::string json(item->data.data(), item->data.size());
    auto rec = ActorStreamingRecord::from_json(json);
    if (rec) {
        CFW_LOG_NOTICE("[ActorCache] Cache hit for actor 0x{:x}", actor_handle);
    }
    return rec;
}

void ActorCache::remove(std::uintptr_t actor_handle) {
    cache_.erase(make_key(actor_handle));
}

bool ActorCache::contains(std::uintptr_t actor_handle) {
    return cache_.contains(make_key(actor_handle));
}

size_t ActorCache::memory_used() const {
    return cache_.memory_used();
}

size_t ActorCache::memory_capacity() const {
    return cache_.memory_capacity();
}

size_t ActorCache::disk_used() {
    return cache_.disk_used();
}

size_t ActorCache::disk_capacity() const {
    return cache_.disk_capacity();
}

void ActorCache::set_evict_callback(std::function<void(std::uintptr_t, const std::string&)> cb) {
    cache_.set_evict_callback(
        [cb = std::move(cb)](const std::string& key, const std::vector<char>& data) {
            auto handle = parse_key(key);
            if (handle != 0 && cb) {
                std::string json(data.data(), data.size());
                cb(handle, json);
            }
        });
}

}  // namespace Corona::Cache