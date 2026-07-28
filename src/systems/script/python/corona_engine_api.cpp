#include "horizon.h"
#include <corona/events/acoustics_system_events.h>
#include <corona/events/display_system_events.h>
#include <corona/events/optics_system_events.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/script/corona_engine_api.h>
#include <corona/systems/script/camera_follow_controller.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/optics/optics_system.h>
#include <corona/utils/path_utils.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "corona/resource/types/audio.h"
#include "corona/resource/types/image.h"
#include "corona/resource/types/video.h"

namespace {
namespace H = Corona::Horizon;

std::atomic<void*> g_default_surface{nullptr};

template <typename T>
H::HardwareBuffer make_horizon_buffer(const std::vector<T>& data,
                                      H::BufferUsageFlags usage,
                                      std::string name = {}) {
    H::HardwareBufferDesc desc;
    desc.element_count = data.size();
    desc.element_size = static_cast<uint32_t>(sizeof(T));
    desc.usage = usage;
    desc.debug_name = std::move(name);
    return H::HardwareBuffer(desc, std::as_bytes(std::span<const T>(data.data(), data.size())));
}

H::HardwareImageDesc make_sampled_texture_desc(uint32_t width,
                                               uint32_t height,
                                               H::Format format,
                                               std::string name = {}) {
    return H::HardwareImageDesc::texture_2d(
        width,
        height,
        format,
        H::ImageUsageFlags::Sampled | H::ImageUsageFlags::TransferDst,
        std::move(name));
}

// 把一个已导入的图片资源（Resource::Image）同步上传为一张可采样的 HardwareImage。
// 复用模型加载里 Geometry::Geometry 的图片转换逻辑（压缩分支 + 1/3/4 通道→RGBA8_SRGB），
// 但做成单图、自包含的同步上传（内部完成 commit + wait_idle），
// 供 from_image() 等「程序化几何 + 图片贴图」路径复用，避免重复维护转换代码。
// 成功返回 true 并写入 out；失败返回 false（out 保持不变，调用方应回退占位纹理）。
[[nodiscard]] bool upload_image_to_texture(Corona::Resource::TResourceID image_id,
                                           H::HardwareImage& out) {
    using namespace Corona;
    if (image_id == 0) {
        return false;
    }
    auto texture_data =
        Resource::ResourceManager::get_instance().acquire_read<Resource::Image>(image_id);
    if (!texture_data) {
        CFW_LOG_WARNING("[upload_image_to_texture] acquire_read<Image> failed for id={}", image_id);
        return false;
    }
    const int tex_width = texture_data->get_width();
    const int tex_height = texture_data->get_height();
    const int tex_channels = texture_data->get_channels();

    H::Format format = H::Format::SRGBA8_UNORM;
    // staging 数据必须存活到 copyFrom + commit 完成。
    std::vector<unsigned char> staging;
    const unsigned char* data_ptr = nullptr;

    if (texture_data->is_compressed()) {
        const auto& compressed = texture_data->get_compressed_data();
        if (compressed.format == Resource::CompressedData::Format::BC1) {
            format = H::Format::BC1_UNORM_SRGB;
        } else if (compressed.format == Resource::CompressedData::Format::BC3) {
            format = H::Format::BC3_UNORM_SRGB;
        } else if (compressed.format == Resource::CompressedData::Format::ASTC_4x4) {
            CFW_LOG_WARNING("[upload_image_to_texture] ASTC_4x4 texture is not supported by current Horizon main format enum");
            return false;
        } else {
            CFW_LOG_WARNING("[upload_image_to_texture] Unsupported compressed format");
            return false;
        }
        staging.assign(compressed.data.begin(), compressed.data.end());
        data_ptr = staging.data();
    } else {
        unsigned char* src_data = texture_data->get_data();
        if (src_data == nullptr || tex_width <= 0 || tex_height <= 0 || tex_channels <= 0) {
            CFW_LOG_WARNING("[upload_image_to_texture] invalid image data ({}x{}, channels={})",
                            tex_width, tex_height, tex_channels);
            return false;
        }
        const size_t pixel_count = static_cast<size_t>(tex_width) * tex_height;
        staging.resize(pixel_count * 4);
        if (tex_channels == 4) {
            std::copy(src_data, src_data + pixel_count * 4, staging.begin());
        } else if (tex_channels == 3) {
            for (size_t i = 0; i < pixel_count; ++i) {
                staging[i * 4 + 0] = src_data[i * 3 + 0];
                staging[i * 4 + 1] = src_data[i * 3 + 1];
                staging[i * 4 + 2] = src_data[i * 3 + 2];
                staging[i * 4 + 3] = 255;
            }
        } else if (tex_channels == 1) {
            for (size_t i = 0; i < pixel_count; ++i) {
                staging[i * 4 + 0] = src_data[i];
                staging[i * 4 + 1] = src_data[i];
                staging[i * 4 + 2] = src_data[i];
                staging[i * 4 + 3] = 255;
            }
        } else {
            CFW_LOG_WARNING("[upload_image_to_texture] Unsupported channel count: {}", tex_channels);
            return false;
        }
        data_ptr = staging.data();
    }

    out = H::HardwareImage(make_sampled_texture_desc(
        static_cast<uint32_t>(tex_width),
        static_cast<uint32_t>(tex_height),
        format,
        "script.image_texture"));
    if (!out) {
        return false;
    }
    return out.write_bytes(std::as_bytes(std::span<const unsigned char>(data_ptr, staging.size())));
}

std::uintptr_t resolve_camera_handle(std::uintptr_t camera_handle) {
    if (camera_handle != 0) {
        return camera_handle;
    }

    std::uintptr_t fallback = 0;
    for (const auto& scene : Corona::SharedDataHub::instance().scene_storage()) {
        if (scene.active_camera_handle != 0) {
            if (scene.enabled) {
                return scene.active_camera_handle;
            }
            if (fallback == 0) {
                fallback = scene.active_camera_handle;
            }
        } else if (!scene.camera_handles.empty() && fallback == 0) {
            fallback = scene.camera_handles.front();
        }
    }
    return fallback;
}

Corona::CameraVisionRenderMode parse_vision_render_mode(const std::string& mode) {
    std::string value = mode;
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    std::replace(value.begin(), value.end(), '-', '_');
    if (value == "svgf" || value == "vision_svgf") {
        return Corona::CameraVisionRenderMode::SVGF;
    }
    if (value == "ssat" || value == "vision_ssat") {
        return Corona::CameraVisionRenderMode::SSAT;
    }
    return Corona::CameraVisionRenderMode::PathTracing;
}

std::string vision_render_mode_to_string(Corona::CameraVisionRenderMode mode) {
    switch (mode) {
        case Corona::CameraVisionRenderMode::SVGF:
            return "svgf";
        case Corona::CameraVisionRenderMode::SSAT:
            return "ssat";
        case Corona::CameraVisionRenderMode::PathTracing:
        default:
            return "path_tracing";
    }
}

bool parse_ssat_view_viewer_mode(const std::string& mode,
                                 Corona::SsatViewViewerMode& out) {
    if (mode == "interlaced") {
        out = Corona::SsatViewViewerMode::Interlaced;
        return true;
    }
    if (mode == "final_view") {
        out = Corona::SsatViewViewerMode::FinalView;
        return true;
    }
    return false;
}

Corona::API::SsatViewViewerStatus make_ssat_view_viewer_status(
    std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    Corona::API::SsatViewViewerStatus result;
    if (resolved_handle == 0) {
        return result;
    }

    const auto state = Corona::SharedDataHub::instance().ssat_view_viewer_state(resolved_handle);
    result.status = state.pending ? "pending" : (state.supported ? "ready" : "unsupported");
    result.supported = state.supported;
    result.pending = state.pending;
    result.mode = state.mode == Corona::SsatViewViewerMode::FinalView
                      ? "final_view"
                      : "interlaced";
    result.view_index = state.effective_view_index;
    result.view_count = state.view_count;
    return result;
}
}

// ########################
//          Scene
// ########################
Corona::API::Scene::Scene()
    : handle_(0) {
    handle_ = SharedDataHub::instance().scene_storage().allocate();
}

Corona::API::Scene::~Scene() {
    if (handle_ != 0) {
        SharedDataHub::instance().scene_storage().deallocate(handle_);
        handle_ = 0;
    }
}

void Corona::API::Scene::set_environment(Environment* env) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Scene::set_environment] Invalid scene handle");
        return;
    }

    if (env == nullptr) {
        CFW_LOG_WARNING("[Scene::set_environment] Null environment pointer");
        return;
    }

    const auto environment_handle = env->get_handle();
    if (environment_handle == 0) {
        CFW_LOG_WARNING("[Scene::set_environment] Invalid environment handle (0)");
        return;
    }

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->environment = environment_handle;
        environment_ = env;
    } else {
        CFW_LOG_ERROR("[Scene::set_environment] Failed to acquire write access to scene storage");
    }
}

Corona::API::Environment* Corona::API::Scene::get_environment() {
    return environment_;
}

bool Corona::API::Scene::has_environment() const {
    return environment_ != nullptr;
}

void Corona::API::Scene::remove_environment() {
    if (handle_ == 0) return;

    auto* previous_environment = environment_;

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->environment = 0;
        environment_ = nullptr;
    } else {
        environment_ = previous_environment;
        CFW_LOG_ERROR("[Scene::remove_environment] Failed to acquire write access to scene storage, rolled back local environment removal");
    }
}

void Corona::API::Scene::add_actor(Actor* actor) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Scene::add_actor] Invalid scene handle");
        return;
    }

    if (actor == nullptr) {
        CFW_LOG_WARNING("[Scene::add_actor] Null actor pointer");
        return;
    }

    const auto actor_handle = actor->get_handle();
    if (actor_handle == 0) {
        CFW_LOG_WARNING("[Scene::add_actor] Invalid actor handle (0)");
        return;
    }

    const auto actor_insert_result = actors_index_.insert(actor);
    if (!actor_insert_result.second) {
        CFW_LOG_WARNING("[Scene::add_actor] Actor already exists in scene, handle: {}", actor_handle);
        return;
    }

    actors_.push_back(actor);

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->actor_handles.push_back(actor_handle);
    } else {
        actors_.pop_back();
        actors_index_.erase(actor);
        CFW_LOG_ERROR("[Scene::add_actor] Failed to acquire write access to scene storage, rolled back local actor insertion");
    }
}

void Corona::API::Scene::remove_actor(Actor* actor) {
    if (handle_ == 0) return;

    if (actor == nullptr) {
        CFW_LOG_WARNING("[Scene::remove_actor] Null actor pointer");
        return;
    }

    auto actor_it = std::find(actors_.begin(), actors_.end(), actor);
    const bool existed_in_vector = actor_it != actors_.end();
    const std::size_t actor_pos = existed_in_vector
                                      ? static_cast<std::size_t>(std::distance(actors_.begin(), actor_it))
                                      : 0;
    const bool existed_in_index = actors_index_.contains(actor);

    if (!existed_in_vector && !existed_in_index) {
        CFW_LOG_WARNING("[Scene::remove_actor] Actor not found in scene, handle: {}", actor->get_handle());
        return;
    }

    if (existed_in_vector) {
        actors_.erase(actor_it);
    }
    if (existed_in_index) {
        actors_index_.erase(actor);
    }

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        std::erase(accessor->actor_handles, actor->get_handle());
    } else {
        if (existed_in_vector) {
            actors_.insert(std::next(actors_.begin(), static_cast<std::vector<Actor*>::difference_type>(actor_pos)), actor);
        }
        if (existed_in_index) {
            actors_index_.insert(actor);
        }
        CFW_LOG_ERROR("[Scene::remove_actor] Failed to acquire write access to scene storage, rolled back local actor removal");
    }
}

void Corona::API::Scene::clear_actors() {
    if (handle_ == 0) return;

    const auto actors_backup = actors_;
    const auto actors_index_backup = actors_index_;

    actors_.clear();
    actors_index_.clear();

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->actor_handles.clear();
    } else {
        actors_ = actors_backup;
        actors_index_ = actors_index_backup;
        CFW_LOG_ERROR("[Scene::clear_actors] Failed to acquire write access to scene storage, rolled back local actor clear");
    }
}

std::size_t Corona::API::Scene::actor_count() const {
    return actors_.size();
}

bool Corona::API::Scene::has_actor(const Actor* actor) const {
    if (actor == nullptr) return false;
    return actors_index_.contains(actor);
}

void Corona::API::Scene::add_camera(Camera* camera) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Scene::add_camera] Invalid scene handle");
        return;
    }

    if (camera == nullptr) {
        CFW_LOG_WARNING("[Scene::add_camera] Null camera pointer");
        return;
    }

    const auto camera_handle = camera->get_handle();
    if (camera_handle == 0) {
        CFW_LOG_WARNING("[Scene::add_camera] Invalid camera handle (0)");
        return;
    }

    const auto camera_insert_result = cameras_index_.insert(camera);
    if (!camera_insert_result.second) {
        CFW_LOG_WARNING("[Scene::add_camera] Camera already exists in scene, handle: {}", camera_handle);
        return;
    }

    cameras_.push_back(camera);

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->camera_handles.push_back(camera_handle);
        if (accessor->active_camera_handle == 0) {
            accessor->active_camera_handle = camera_handle;
        }
    } else {
        // Roll back local state to keep Scene cache and shared storage consistent.
        cameras_.pop_back();
        cameras_index_.erase(camera);
        CFW_LOG_ERROR("[Scene::add_camera] Failed to acquire write access to scene storage, rolled back local camera insertion");
    }
}

void Corona::API::Scene::remove_camera(Camera* camera) {
    if (handle_ == 0) return;

    if (camera == nullptr) {
        CFW_LOG_WARNING("[Scene::remove_camera] Null camera pointer");
        return;
    }

    auto camera_it = std::find(cameras_.begin(), cameras_.end(), camera);
    const bool existed_in_vector = camera_it != cameras_.end();
    const std::size_t camera_pos = existed_in_vector
                                       ? static_cast<std::size_t>(std::distance(cameras_.begin(), camera_it))
                                       : 0;
    const bool existed_in_index = cameras_index_.contains(camera);

    if (!existed_in_vector && !existed_in_index) {
        CFW_LOG_WARNING("[Scene::remove_camera] Camera not found in scene");
        return;
    }

    if (existed_in_vector) {
        cameras_.erase(camera_it);
    }
    if (existed_in_index) {
        cameras_index_.erase(camera);
    }

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        std::erase(accessor->camera_handles, camera->get_handle());
        if (accessor->active_camera_handle == camera->get_handle()) {
            accessor->active_camera_handle = accessor->camera_handles.empty()
                                                 ? 0
                                                 : accessor->camera_handles.front();
        }
    } else {
        if (existed_in_vector) {
            cameras_.insert(std::next(cameras_.begin(), static_cast<std::vector<Camera*>::difference_type>(camera_pos)), camera);
        }
        if (existed_in_index) {
            cameras_index_.insert(camera);
        }
        CFW_LOG_ERROR("[Scene::remove_camera] Failed to acquire write access to scene storage, rolled back local camera removal");
    }
}

void Corona::API::Scene::clear_cameras() {
    if (handle_ == 0) return;

    const auto cameras_backup = cameras_;
    const auto cameras_index_backup = cameras_index_;

    cameras_.clear();
    cameras_index_.clear();

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->camera_handles.clear();
        accessor->active_camera_handle = 0;
    } else {
        cameras_ = cameras_backup;
        cameras_index_ = cameras_index_backup;
        CFW_LOG_ERROR("[Scene::clear_cameras] Failed to acquire write access to scene storage, rolled back local camera clear");
    }
}

void Corona::API::Scene::set_active_camera(Camera* camera) {
    if (handle_ == 0) return;

    if (camera == nullptr || !has_camera(camera)) {
        CFW_LOG_WARNING("[Scene::set_active_camera] Camera not found in scene");
        return;
    }

    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->active_camera_handle = camera->get_handle();
    } else {
        CFW_LOG_ERROR("[Scene::set_active_camera] Failed to acquire write access to scene storage");
    }
}

std::uintptr_t Corona::API::Scene::get_active_camera_handle() const {
    if (handle_ == 0) return 0;

    if (auto accessor = SharedDataHub::instance().scene_storage().try_acquire_read(handle_)) {
        if (accessor->active_camera_handle != 0) {
            return accessor->active_camera_handle;
        }
        return accessor->camera_handles.empty() ? 0 : accessor->camera_handles.front();
    }

    CFW_LOG_ERROR("[Scene::get_active_camera_handle] Failed to acquire read access to scene storage");
    return 0;
}

std::size_t Corona::API::Scene::camera_count() const {
    return cameras_.size();
}

bool Corona::API::Scene::has_camera(const Camera* camera) const {
    if (camera == nullptr) return false;
    return cameras_index_.contains(camera);
}

std::array<float, 6> Corona::API::Scene::get_aabb() const {
    if (handle_ == 0) {
        return {0, 0, 0, 0, 0, 0};
    }
    if (auto accessor = SharedDataHub::instance().scene_storage().try_acquire_read(handle_)) {
        return {accessor->min_world.x, accessor->min_world.y, accessor->min_world.z,
                accessor->max_world.x, accessor->max_world.y, accessor->max_world.z};
    }
    return {0, 0, 0, 0, 0, 0};
}

void Corona::API::Scene::set_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Scene::set_enabled] Invalid scene handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->enabled = enabled;
    } else {
        CFW_LOG_ERROR("[Scene::set_enabled] Failed to acquire write access to scene storage");
    }
}

bool Corona::API::Scene::is_enabled() const {
    if (handle_ == 0) return false;
    if (auto accessor = SharedDataHub::instance().scene_storage().try_acquire_read(handle_)) {
        return accessor->enabled;
    }
    return false;
}

void Corona::API::Scene::set_simulation_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Scene::set_simulation_enabled] Invalid scene handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().scene_storage().acquire_write(handle_)) {
        accessor->simulation_enabled = enabled;
    } else {
        CFW_LOG_ERROR("[Scene::set_simulation_enabled] Failed to acquire write access to scene storage");
    }
}

bool Corona::API::Scene::is_simulation_enabled() const {
    if (handle_ == 0) return false;
    if (auto accessor = SharedDataHub::instance().scene_storage().try_acquire_read(handle_)) {
        return accessor->simulation_enabled;
    }
    return false;
}

// ########################
//      Environment
// ########################
Corona::API::Environment::Environment()
    : handle_(0) {
    handle_ = SharedDataHub::instance().environment_storage().allocate();
    if (handle_ != 0) {
        if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
            accessor->sun_position.x = 1.0f;
            accessor->sun_position.y = 1.0f;
            accessor->sun_position.z = 1.0f;
            accessor->floor_grid_enabled = 1;
        }
    }
}

Corona::API::Environment::~Environment() {
    if (handle_ != 0) {
        SharedDataHub::instance().environment_storage().deallocate(handle_);
        handle_ = 0;
    }
}

void Corona::API::Environment::set_sun_direction(const std::array<float, 3>& direction) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_sun_direction] Invalid environment handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->sun_position.x = direction[0];
        accessor->sun_position.y = direction[1];
        accessor->sun_position.z = direction[2];
    } else {
        CFW_LOG_ERROR("[Environment::set_sun_direction] Failed to acquire write access to environment storage");
    }
}

std::array<float, 3> Corona::API::Environment::get_sun_direction() const {
    if (handle_ == 0) return {1.0f, 1.0f, 1.0f};
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return {accessor->sun_position.x, accessor->sun_position.y, accessor->sun_position.z};
    }
    return {1.0f, 1.0f, 1.0f};
}

void Corona::API::Environment::set_sun_intensity(float intensity) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_sun_intensity] Invalid environment handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->sun_intensity = intensity;
    } else {
        CFW_LOG_ERROR("[Environment::set_sun_intensity] Failed to acquire write access to environment storage");
    }
}

float Corona::API::Environment::get_sun_intensity() const {
    if (handle_ == 0) return 10.0f;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->sun_intensity;
    }
    return 10.0f;
}

void Corona::API::Environment::set_sky_intensity(float intensity) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_sky_intensity] Invalid environment handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->sky_intensity = intensity;
    } else {
        CFW_LOG_ERROR("[Environment::set_sky_intensity] Failed to acquire write access to environment storage");
    }
}

float Corona::API::Environment::get_sky_intensity() const {
    if (handle_ == 0) return 20.0f;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->sky_intensity;
    }
    return 20.0f;
}

void Corona::API::Environment::set_floor_grid(bool enabled) const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_floor_grid] Invalid environment handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->floor_grid_enabled = enabled ? 1u : 0u;
    } else {
        CFW_LOG_ERROR("[Environment::set_floor_grid] Failed to acquire write access to environment storage");
    }
}

bool Corona::API::Environment::get_floor_grid() const {
    if (handle_ == 0) return true;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->floor_grid_enabled;
    }
    return true;
}

std::uintptr_t Corona::API::Environment::get_handle() const {
    return handle_;
}

void Corona::API::Environment::set_gravity(const std::array<float, 3>& gravity) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_gravity] Invalid environment handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->gravity.x = gravity[0];
        accessor->gravity.y = gravity[1];
        accessor->gravity.z = gravity[2];
    }
}

std::array<float, 3> Corona::API::Environment::get_gravity() const {
    if (handle_ == 0) return {0.0f, -9.8f, 0.0f};
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return {accessor->gravity.x, accessor->gravity.y, accessor->gravity.z};
    }
    return {0.0f, -9.8f, 0.0f};
}

void Corona::API::Environment::set_floor_y(float y) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_floor_y] Invalid environment handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->floor_y = y;
    }
}

float Corona::API::Environment::get_floor_y() const {
    if (handle_ == 0) return 0.0f;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->floor_y;
    }
    return 0.0f;
}

void Corona::API::Environment::set_floor_restitution(float restitution) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_floor_restitution] Invalid environment handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->floor_restitution = restitution;
    }
}

float Corona::API::Environment::get_floor_restitution() const {
    if (handle_ == 0) return 0.6f;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->floor_restitution;
    }
    return 0.6f;
}

void Corona::API::Environment::set_fixed_dt(float dt) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Environment::set_fixed_dt] Invalid environment handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().environment_storage().acquire_write(handle_)) {
        accessor->fixed_dt = dt;
    }
}

float Corona::API::Environment::get_fixed_dt() const {
    if (handle_ == 0) return 1.0f / 60.0f;
    if (auto accessor = SharedDataHub::instance().environment_storage().try_acquire_read(handle_)) {
        return accessor->fixed_dt;
    }
    return 1.0f / 60.0f;
}

// ########################
//         Geometry
// ########################
Corona::API::Geometry::Geometry(const std::string& model_path) {
    // 保存路径供 Actor 标识和 GeometrySystem 资源加载/卸载使用
    model_path_ = Utils::utf8_to_path(model_path);

    // ---- 全异步加载（方案 A）：构造函数不做任何磁盘 IO / 解析 ----
    // 此前这里同步 import_sync（磁盘 + assimp 解析，数百 ms~数秒），由于 Python
    // Geometry() 通常在 CEF UI 线程（OnQuery 持 GIL）同步执行，会独占该线程，
    // 导致"加载一个模型时无法打开页面加载其他模型"。
    //
    // 现改为：仅分配三个 SharedDataHub 槽（微秒级），记录 model_path 并标记
    // PendingImport，立即返回。实际 import + GPU 构建由 GeometrySystem::update()
    // 在引擎线程承接（PendingImport → import_async → PendingBuild → 构建 → Ready）。
    //
    // 契约变化：构造后 model_id / AABB **尚未就绪**（get_aabb() 暂返回 0，
    // Mechanics AABB 暂为 0）。GeometrySystem 在 import 完成后回填 MechanicsDevice
    // 的 AABB，八叉树每帧重读自然自愈；前端读 AABB 的逻辑需容忍"最终一致"。
    model_resource_handle_ = SharedDataHub::instance().model_resource_storage().allocate();
    if (auto handle = SharedDataHub::instance().model_resource_storage().acquire_write(model_resource_handle_)) {
        handle->model_id = 0;  // 待 import 完成后由 GeometrySystem 填入
    } else {
        CFW_LOG_ERROR("[Geometry::Geometry] Failed to acquire write access to model resource storage");
        SharedDataHub::instance().model_resource_storage().deallocate(model_resource_handle_);
        model_resource_handle_ = 0;
        return;
    }

    transform_handle_ = SharedDataHub::instance().model_transform_storage().allocate();

    handle_ = SharedDataHub::instance().geometry_storage().allocate();
    if (auto handle = SharedDataHub::instance().geometry_storage().acquire_write(handle_)) {
        handle->transform_handle = transform_handle_;
        handle->model_resource_handle = model_resource_handle_;
        handle->mesh_handles.clear();  // 留空，待 GeometrySystem 异步构建
        handle->model_path_utf8 = model_path;  // 供 GeometrySystem 异步 import
        handle->gpu_build_state = GeometryDevice::GpuBuildState::PendingImport;
    } else {
        CFW_LOG_CRITICAL("[Geometry::Geometry] Failed to acquire write access to geometry storage");
        // 清理已分配的资源
        SharedDataHub::instance().model_transform_storage().deallocate(transform_handle_);
        SharedDataHub::instance().model_resource_storage().deallocate(model_resource_handle_);
        SharedDataHub::instance().geometry_storage().deallocate(handle_);
        handle_ = 0;
        transform_handle_ = 0;
        model_resource_handle_ = 0;
        return;
    }
}

Corona::API::Geometry::~Geometry() {
    if (handle_ != 0) {
        SharedDataHub::instance().geometry_storage().deallocate(handle_);
    }
    if (transform_handle_ != 0) {
        SharedDataHub::instance().model_transform_storage().deallocate(transform_handle_);
    }
    if (model_resource_handle_ != 0) {
        SharedDataHub::instance().model_resource_storage().deallocate(model_resource_handle_);
    }
}

Corona::API::Geometry::Geometry(Corona::API::Geometry&& other) noexcept
    : handle_(other.handle_),
      transform_handle_(other.transform_handle_),
      model_resource_handle_(other.model_resource_handle_) {
    // 置空源对象，避免移动后两个对象的析构函数重复 deallocate 同一 handle。
    other.handle_ = 0;
    other.transform_handle_ = 0;
    other.model_resource_handle_ = 0;
}

Corona::API::Geometry& Corona::API::Geometry::operator=(Corona::API::Geometry&& other) noexcept {
    if (this != &other) {
        if (handle_ != 0) {
            SharedDataHub::instance().geometry_storage().deallocate(handle_);
        }
        if (transform_handle_ != 0) {
            SharedDataHub::instance().model_transform_storage().deallocate(transform_handle_);
        }
        if (model_resource_handle_ != 0) {
            SharedDataHub::instance().model_resource_storage().deallocate(model_resource_handle_);
        }
        handle_ = other.handle_;
        transform_handle_ = other.transform_handle_;
        model_resource_handle_ = other.model_resource_handle_;
        other.handle_ = 0;
        other.transform_handle_ = 0;
        other.model_resource_handle_ = 0;
    }
    return *this;
}

Corona::API::Geometry Corona::API::Geometry::from_image(const std::string& image_path) {
    using namespace Corona;
    Geometry geo;  // 默认构造：所有 handle 为 0，失败时直接返回空 Geometry。

    auto image_id =
        Resource::ResourceManager::get_instance().import_sync(Utils::utf8_to_path(image_path));
    if (image_id == 0) {
        CFW_LOG_CRITICAL("[Geometry::from_image] Failed to import image: {}", image_path);
        return geo;
    }

    // 读取图片尺寸以按宽高比构造不拉伸的 quad（高度归一为 1，宽度 = aspect）。
    float aspect = 1.0f;
    if (auto image = Resource::ResourceManager::get_instance().acquire_read<Resource::Image>(image_id)) {
        const int w = image->get_width();
        const int h = image->get_height();
        if (w > 0 && h > 0) {
            aspect = static_cast<float>(w) / static_cast<float>(h);
        }
    }

    // 程序化 quad（两个三角形）。XY 平面、法线 +Z、UV 满铺 [0,1]。
    // 顶点格式严格匹配 Resource::Vertex（position/normal/tex_coords，#pragma pack(1)），
    // 以便与现有 vertexBuffer / vertexStorageBuffer 及 Vision adapter 兼容。
    const float hw = aspect * 0.5f;
    const float hh = 0.5f;
    const std::vector<Resource::Vertex> vertices = {
        {{-hw, -hh, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},  // 左下
        {{ hw, -hh, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},  // 右下
        {{ hw,  hh, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},  // 右上
        {{-hw,  hh, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},  // 左上
    };
    const std::vector<std::uint16_t> indices = {0, 1, 2, 0, 2, 3};

    MeshDevice dev{};
    dev.vertexBuffer = make_horizon_buffer(
        vertices,
        H::BufferUsageFlags::TransferDst | H::BufferUsageFlags::Vertex,
        "script.image.vertex");
    dev.indexBuffer = make_horizon_buffer(
        indices,
        H::BufferUsageFlags::TransferDst | H::BufferUsageFlags::Index,
        "script.image.index");
    dev.vertexStorageBuffer = make_horizon_buffer(
        vertices,
        H::BufferUsageFlags::TransferSrc | H::BufferUsageFlags::TransferDst | H::BufferUsageFlags::Storage,
        "script.image.vertex_storage");
    dev.indexStorageBuffer = make_horizon_buffer(
        indices,
        H::BufferUsageFlags::TransferSrc | H::BufferUsageFlags::TransferDst | H::BufferUsageFlags::Storage,
        "script.image.index_storage");
    dev.materialIndex = 0;
    dev.materialColor = {1.0f, 1.0f, 1.0f, 1.0f};
    dev.vertex_count = static_cast<std::uint32_t>(vertices.size());
    dev.index_count = static_cast<std::uint32_t>(indices.size());
    dev.max_index = 3;

    if (!upload_image_to_texture(image_id, dev.textureBuffer)) {
        // 上传失败：回退 1x1 白占位，几何仍可显示（白底），避免整体导入失败。
        static const unsigned char white_pixel[4] = {255, 255, 255, 255};
        dev.textureBuffer = H::HardwareImage(make_sampled_texture_desc(
            1,
            1,
            H::Format::SRGBA8_UNORM,
            "script.image.placeholder_texture"));
        (void)dev.textureBuffer.write_bytes(
            std::as_bytes(std::span<const unsigned char>(white_pixel, 4)));
        CFW_LOG_WARNING("[Geometry::from_image] texture upload failed, using white placeholder: {}",
                        image_path);
    }

    // ---- GPU 显存记账（P0：mesh + texture）----
    // mesh：顶点/索引各两份缓冲；texture：按 extent 估算 RGBA8（占位 1x1 可忽略）。
    dev.mesh_mem = Corona::Memory::GpuMemToken(
        Corona::Memory::ResKind::Mesh,
        2u * vertices.size() * sizeof(Resource::Vertex) +
        2u * indices.size()  * sizeof(std::uint16_t));
    if (dev.textureBuffer) {
        const auto ext = dev.textureBuffer.extent();
        dev.tex_mem = Corona::Memory::GpuMemToken(
            Corona::Memory::ResKind::Texture,
            static_cast<std::size_t>(ext.width) * ext.height * 4);
    }

    std::vector<MeshDevice> mesh_devices;
    mesh_devices.emplace_back(std::move(dev));

    // 图片无 Resource::Scene，model_resource_handle 保持 0：Vision adapter 会回退到
    // 从 vertexBuffer 拷回 CPU mesh 的路径（load_cpu_mesh_from_buffers），无需 model resource。
    geo.transform_handle_ = SharedDataHub::instance().model_transform_storage().allocate();
    geo.handle_ = SharedDataHub::instance().geometry_storage().allocate();
    if (auto handle = SharedDataHub::instance().geometry_storage().acquire_write(geo.handle_)) {
        handle->transform_handle = geo.transform_handle_;
        handle->model_resource_handle = 0;
        handle->mesh_handles = std::move(mesh_devices);
    } else {
        CFW_LOG_CRITICAL("[Geometry::from_image] Failed to acquire write access to geometry storage");
        SharedDataHub::instance().model_transform_storage().deallocate(geo.transform_handle_);
        SharedDataHub::instance().geometry_storage().deallocate(geo.handle_);
        geo.handle_ = 0;
        geo.transform_handle_ = 0;
    }
    return geo;
}

void Corona::API::Geometry::set_position(const std::array<float, 3>& pos) {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::set_position] Invalid transform handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_write(transform_handle_)) {
        accessor->position.x = pos[0];
        accessor->position.y = pos[1];
        accessor->position.z = pos[2];
    } else {
        CFW_LOG_ERROR("[Geometry::set_position] Failed to acquire write access to transform storage");
    }
}

void Corona::API::Geometry::set_rotation(const std::array<float, 3>& euler) {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::set_rotation] Invalid transform handle");
        return;
    }

    // 直接写入容器中的局部旋转参数（欧拉角 ZYX 顺序）
    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_write(transform_handle_)) {
        accessor->euler_rotation.x = euler[0];  // Pitch
        accessor->euler_rotation.y = euler[1];  // Yaw
        accessor->euler_rotation.z = euler[2];  // Roll
    } else {
        CFW_LOG_ERROR("[Geometry::set_rotation] Failed to acquire write access to transform storage");
    }
}

void Corona::API::Geometry::set_scale(const std::array<float, 3>& scl) {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::set_scale] Invalid transform handle");
        return;
    }

    // 直接写入容器中的局部缩放参数
    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_write(transform_handle_)) {
        accessor->scale.x = scl[0];
        accessor->scale.y = scl[1];
        accessor->scale.z = scl[2];
    } else {
        CFW_LOG_ERROR("[Geometry::set_scale] Failed to acquire write access to transform storage");
    }
}

void Corona::API::Geometry::set_native_local_correction(const std::array<float, 3>& offset, float scale) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::set_native_local_correction] Invalid geometry handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().geometry_storage().acquire_write(handle_)) {
        accessor->native_local_correction_offset.x = offset[0];
        accessor->native_local_correction_offset.y = offset[1];
        accessor->native_local_correction_offset.z = offset[2];
        accessor->native_local_correction_scale = scale;
    } else {
        CFW_LOG_ERROR("[Geometry::set_native_local_correction] Failed to acquire write access to geometry storage");
    }
}

std::array<float, 3> Corona::API::Geometry::get_position() const {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::get_position] Invalid transform handle");
        return {0.0f, 0.0f, 0.0f};
    }

    // 从容器中读取局部位置参数
    std::array<float, 3> result = {0.0f, 0.0f, 0.0f};
    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_read(transform_handle_)) {
        result[0] = accessor->position.x;
        result[1] = accessor->position.y;
        result[2] = accessor->position.z;
    } else {
        CFW_LOG_ERROR("[Geometry::get_position] Failed to acquire read access to transform storage");
    }

    return result;
}

std::array<float, 3> Corona::API::Geometry::get_rotation() const {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::get_rotation] Invalid transform handle");
        return {0.0f, 0.0f, 0.0f};
    }

    // 从容器中读取局部旋转参数（欧拉角 ZYX 顺序）
    std::array<float, 3> result = {0.0f, 0.0f, 0.0f};
    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_read(transform_handle_)) {
        result[0] = accessor->euler_rotation.x;  // Pitch
        result[1] = accessor->euler_rotation.y;  // Yaw
        result[2] = accessor->euler_rotation.z;  // Roll
    } else {
        CFW_LOG_ERROR("[Geometry::get_rotation] Failed to acquire read access to transform storage");
    }

    return result;
}

std::array<float, 3> Corona::API::Geometry::get_scale() const {
    if (transform_handle_ == 0) {
        CFW_LOG_WARNING("[Geometry::get_scale] Invalid transform handle");
        return {1.0f, 1.0f, 1.0f};
    }

    // 从容器中读取局部缩放参数
    std::array<float, 3> result = {1.0f, 1.0f, 1.0f};
    if (auto accessor = SharedDataHub::instance().model_transform_storage().acquire_read(transform_handle_)) {
        result[0] = accessor->scale.x;
        result[1] = accessor->scale.y;
        result[2] = accessor->scale.z;
    } else {
        CFW_LOG_ERROR("[Geometry::get_scale] Failed to acquire read access to transform storage");
    }

    return result;
}

std::uintptr_t Corona::API::Geometry::get_handle() const {
    return handle_;
}

const std::filesystem::path& Corona::API::Geometry::get_model_path() const {
    return model_path_;
}

bool Corona::API::Geometry::is_valid() const {
    return handle_ != 0 && transform_handle_ != 0;
}

std::string Corona::API::Geometry::get_gpu_build_state() const {
    if (handle_ == 0) {
        return "Invalid";
    }
    if (auto geom = SharedDataHub::instance().geometry_storage().try_acquire_read(handle_)) {
        switch (geom->gpu_build_state) {
            case GeometryDevice::GpuBuildState::Ready:
                return "Ready";
            case GeometryDevice::GpuBuildState::PendingImport:
                return "PendingImport";
            case GeometryDevice::GpuBuildState::PendingBuild:
                return "PendingBuild";
            case GeometryDevice::GpuBuildState::Failed:
                return "Failed";
        }
    }
    return "Unavailable";
}

std::size_t Corona::API::Geometry::get_mesh_count() const {
    if (handle_ == 0) {
        return 0;
    }
    if (auto geom = SharedDataHub::instance().geometry_storage().try_acquire_read(handle_)) {
        return geom->mesh_handles.size();
    }
    return 0;
}

std::uint64_t Corona::API::Geometry::get_model_id() const {
    if (handle_ == 0) {
        return 0;
    }
    if (auto geom = SharedDataHub::instance().geometry_storage().try_acquire_read(handle_)) {
        if (geom->model_resource_handle != 0) {
            if (auto res = SharedDataHub::instance().model_resource_storage().try_acquire_read(geom->model_resource_handle)) {
                return res->model_id;
            }
        }
    }
    return 0;
}

Corona::API::GeometryRenderStatus Corona::API::Geometry::get_render_status() const {
    GeometryRenderStatus status;
    status.gpu_build_state = get_gpu_build_state();
    status.mesh_count = get_mesh_count();
    status.failed = status.gpu_build_state == "Failed";
    if (handle_ == 0) {
        return status;
    }

    auto* system_manager = Kernel::KernelContext::instance().system_manager();
    if (!system_manager) {
        return status;
    }
    auto geometry_system = std::dynamic_pointer_cast<Systems::GeometrySystem>(
        system_manager->get_system("Geometry"));
    if (!geometry_system) {
        return status;
    }

    const auto slots = geometry_system->query_mesh_slots(handle_);
    status.observed = true;
    status.renderable_mesh_count = static_cast<std::size_t>(std::count_if(
        slots.begin(), slots.end(), [](const auto& slot) { return slot.valid; }));
    status.invalid_mesh_count = slots.size() - status.renderable_mesh_count;
    status.ready = status.gpu_build_state == "Ready" &&
                   status.mesh_count > 0 &&
                   slots.size() == status.mesh_count &&
                   status.invalid_mesh_count == 0;
    return status;
}

std::array<float, 6> Corona::API::Geometry::get_aabb() const {
    if (auto geom = SharedDataHub::instance().geometry_storage().try_acquire_read(handle_)) {
        if (auto res = SharedDataHub::instance().model_resource_storage().try_acquire_read(geom->model_resource_handle)) {
            if (res->model_id) {
                if (auto scene = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(res->model_id)) {
                    auto aabb_min = scene->get_scene_aabb().min;
                    auto aabb_max = scene->get_scene_aabb().max;
                    return {aabb_min[0], aabb_min[1], aabb_min[2], aabb_max[0], aabb_max[1], aabb_max[2]};
                }
            }
        }
    }
    return {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
}

std::uintptr_t Corona::API::Geometry::get_transform_handle() const {
    return transform_handle_;
}

std::uintptr_t Corona::API::Geometry::get_model_resource_handle() const {
    return model_resource_handle_;
}

// ########################
//         Optics
// ########################
Corona::API::Optics::Optics(Geometry& geo)
    : geometry_(&geo), handle_(0) {
    if (!geo.is_valid()) {
        CFW_LOG_WARNING("[Optics::Optics] Invalid geometry; optics component not created");
        return;
    }

    handle_ = SharedDataHub::instance().optics_storage().allocate();
    if (auto accessor = SharedDataHub::instance().optics_storage().acquire_write(handle_)) {
        accessor->geometry_handle = geo.get_handle();
    } else {
        CFW_LOG_ERROR("[Optics::Optics] Failed to acquire write access to optics storage");
        SharedDataHub::instance().optics_storage().deallocate(handle_);
        handle_ = 0;
    }
}

Corona::API::Optics::~Optics() {
    if (handle_ != 0) {
        SharedDataHub::instance().optics_storage().deallocate(handle_);
    }
}

std::uintptr_t Corona::API::Optics::get_handle() const {
    return handle_;
}

Corona::API::Geometry* Corona::API::Optics::get_geometry() const {
    return geometry_;
}

void Corona::API::Optics::set_visible(bool visible) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->visible = visible;
}
bool Corona::API::Optics::get_visible() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->visible;
    return true;
}

void Corona::API::Optics::set_lighting_enabled(bool enabled) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->bEnableLighting = enabled;  // 设置光照开关
}
bool Corona::API::Optics::get_lighting_enabled() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->bEnableLighting;  // 读取光照开关状态
    return true;
}

void Corona::API::Optics::set_metallic(float metallic) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->metallic = metallic;
}
float Corona::API::Optics::get_metallic() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->metallic;
    return 0.0f;
}
void Corona::API::Optics::set_roughness(float roughness) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->roughness = roughness;
}
float Corona::API::Optics::get_roughness() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->roughness;
    return 0.5f;
}
void Corona::API::Optics::set_subsurface(float subsurface) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->subsurface = subsurface;
}
float Corona::API::Optics::get_subsurface() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->subsurface;
    return 0.0f;
}
void Corona::API::Optics::set_specular(float specular) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->specular = specular;
}
float Corona::API::Optics::get_specular() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->specular;
    return 0.5f;
}
void Corona::API::Optics::set_specular_tint(float specularTint) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->specularTint = specularTint;
}
float Corona::API::Optics::get_specular_tint() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->specularTint;
    return 0.0f;
}
void Corona::API::Optics::set_anisotropic(float anisotropic) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->anisotropic = anisotropic;
}
float Corona::API::Optics::get_anisotropic() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->anisotropic;
    return 0.0f;
}
void Corona::API::Optics::set_sheen(float sheen) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->sheen = sheen;
}
float Corona::API::Optics::get_sheen() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->sheen;
    return 0.0f;
}
void Corona::API::Optics::set_sheen_tint(float sheenTint) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->sheenTint = sheenTint;
}
float Corona::API::Optics::get_sheen_tint() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->sheenTint;
    return 0.5f;
}
void Corona::API::Optics::set_clearcoat(float clearcoat) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->clearcoat = clearcoat;
}
float Corona::API::Optics::get_clearcoat() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->clearcoat;
    return 0.0f;
}
void Corona::API::Optics::set_clearcoat_gloss(float clearcoatGloss) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->clearcoatGloss = clearcoatGloss;
}
float Corona::API::Optics::get_clearcoat_gloss() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->clearcoatGloss;
    return 1.0f;
}
void Corona::API::Optics::set_ambient(const std::array<float, 3>& ambient) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->ambient = {ambient[0], ambient[1], ambient[2]};
}
std::array<float, 3> Corona::API::Optics::get_ambient() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return {r->ambient.x, r->ambient.y, r->ambient.z};
    return {0.2f, 0.2f, 0.2f};
}
void Corona::API::Optics::set_diffuse(const std::array<float, 3>& diffuse) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->diffuse = {diffuse[0], diffuse[1], diffuse[2]};
}
std::array<float, 3> Corona::API::Optics::get_diffuse() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return {r->diffuse.x, r->diffuse.y, r->diffuse.z};
    return {0.8f, 0.8f, 0.8f};
}
void Corona::API::Optics::set_specular_color(const std::array<float, 3>& specular) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->specular_color = {specular[0], specular[1], specular[2]};
}
std::array<float, 3> Corona::API::Optics::get_specular_color() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return {r->specular_color.x, r->specular_color.y, r->specular_color.z};
    return {1.0f, 1.0f, 1.0f};
}
void Corona::API::Optics::set_shininess(float shininess) {
    if (auto w = SharedDataHub::instance().optics_storage().acquire_write(handle_)) w->shininess = shininess;
}
float Corona::API::Optics::get_shininess() const {
    if (auto r = SharedDataHub::instance().optics_storage().try_acquire_read(handle_)) return r->shininess;
    return 32.0f;
}

// ########################
//       Mechanics
// ########################
Corona::API::Mechanics::Mechanics(Geometry& geo)
    : geometry_(&geo), handle_(0) {
    if (!geo.is_valid()) {
        CFW_LOG_WARNING("[Mechanics::Mechanics] Invalid geometry; mechanics component not created");
        return;
    }

    // 获取模型的包围盒信息
    ktm::fvec3 max_xyz;
    max_xyz.x = 0.0f;
    max_xyz.y = 0.0f;
    max_xyz.z = 0.0f;

    ktm::fvec3 min_xyz;
    min_xyz.x = 0.0f;
    min_xyz.y = 0.0f;
    min_xyz.z = 0.0f;

    if (auto geom_handle = SharedDataHub::instance().geometry_storage().try_acquire_read(geo.get_handle())) {
        if (auto res_handle = SharedDataHub::instance().model_resource_storage().try_acquire_read(geom_handle->model_resource_handle)) {
            if (res_handle->model_id) {
                if (auto scene = Resource::ResourceManager::get_instance().acquire_read<Resource::Scene>(res_handle->model_id)) {
                    auto max = scene->get_scene_aabb().max;
                    auto min = scene->get_scene_aabb().min;
                    max_xyz.x = max[0];
                    max_xyz.y = max[1];
                    max_xyz.z = max[2];
                    min_xyz.x = min[0];
                    min_xyz.y = min[1];
                    min_xyz.z = min[2];
                } else {
                    CFW_LOG_WARNING("[Mechanics::Mechanics] Failed to acquire scene resource; using default AABB");
                }
            }
        } else {
            CFW_LOG_WARNING("[Mechanics::Mechanics] Failed to read model resource; using default AABB");
        }
    } else {
        CFW_LOG_WARNING("[Mechanics::Mechanics] Failed to read geometry; using default AABB");
    }

    // 创建 MechanicsDevice
    handle_ = SharedDataHub::instance().mechanics_storage().allocate();
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->geometry_handle = geo.get_handle();
        accessor->max_xyz = max_xyz;
        accessor->min_xyz = min_xyz;
    } else {
        CFW_LOG_ERROR("[Mechanics::Mechanics] Failed to acquire write access to mechanics storage");
        SharedDataHub::instance().mechanics_storage().deallocate(handle_);
        handle_ = 0;
    }
}

Corona::API::Mechanics::~Mechanics() {
    if (handle_ != 0) {
        SharedDataHub::instance().mechanics_storage().deallocate(handle_);
    }
}

std::uintptr_t Corona::API::Mechanics::get_handle() const {
    return handle_;
}

Corona::API::Geometry* Corona::API::Mechanics::get_geometry() const {
    return geometry_;
}

void Corona::API::Mechanics::set_mass(float mass) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_mass] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->mass = mass;
    }
}

float Corona::API::Mechanics::get_mass() const {
    if (handle_ == 0) return 1.0f;
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        return accessor->mass;
    }
    return 1.0f;
}

void Corona::API::Mechanics::set_restitution(float restitution) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_restitution] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->restitution = restitution;
    }
}

float Corona::API::Mechanics::get_restitution() const {
    if (handle_ == 0) return 0.8f;
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        return accessor->restitution;
    }
    return 0.8f;
}

void Corona::API::Mechanics::set_damping(float damping) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_damping] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->damping = damping;
    }
}

float Corona::API::Mechanics::get_damping() const {
    if (handle_ == 0) return 0.99f;
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        return accessor->damping;
    }
    return 0.99f;
}

void Corona::API::Mechanics::set_physics_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_physics_enabled] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->physics_enabled = enabled;
    }
}

bool Corona::API::Mechanics::get_physics_enabled() const {
    if (handle_ == 0) return true;
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        return accessor->physics_enabled;
    }
    return true;
}

void Corona::API::Mechanics::set_collision_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_collision_enabled] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->collision_shape = enabled ? CollisionShape::Box : CollisionShape::None;
    }
}

bool Corona::API::Mechanics::get_collision_enabled() const {
    if (handle_ == 0) return true;
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        return accessor->collision_shape != CollisionShape::None;
    }
    return true;
}

void Corona::API::Mechanics::set_collision_shape(std::string_view shape) {
    if (handle_ == 0) return;
    CollisionShape value = CollisionShape::Box;
    if (shape == "none") value = CollisionShape::None;
    else if (shape == "mesh") value = CollisionShape::Mesh;
    else if (shape != "box") {
        CFW_LOG_WARNING("[Mechanics::set_collision_shape] Invalid shape '{}'; using box", shape);
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->collision_shape = value;
    }
}

std::string Corona::API::Mechanics::get_collision_shape() const {
    if (handle_ != 0) {
        if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
            switch (accessor->collision_shape) {
                case CollisionShape::None: return "none";
                case CollisionShape::Mesh: return "mesh";
                case CollisionShape::Box: return "box";
            }
        }
    }
    return "box";
}

void Corona::API::Mechanics::set_linear_lock(bool lock_x, bool lock_y, bool lock_z) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_linear_lock] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        uint8_t mask = 0;
        if (lock_x) mask |= 0b001;
        if (lock_y) mask |= 0b010;
        if (lock_z) mask |= 0b100;
        accessor->linear_lock_mask = mask;
    }
}

std::tuple<bool, bool, bool> Corona::API::Mechanics::get_linear_lock() const {
    if (handle_ == 0) return {false, false, false};
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        uint8_t mask = accessor->linear_lock_mask;
        return {(mask & 0b001) != 0, (mask & 0b010) != 0, (mask & 0b100) != 0};
    }
    return {false, false, false};
}

void Corona::API::Mechanics::set_angular_lock(bool lock_x, bool lock_y, bool lock_z) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_angular_lock] Invalid mechanics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        uint8_t mask = 0;
        if (lock_x) mask |= 0b001;
        if (lock_y) mask |= 0b010;
        if (lock_z) mask |= 0b100;
        accessor->angular_lock_mask = mask;
    }
}

std::tuple<bool, bool, bool> Corona::API::Mechanics::get_angular_lock() const {
    if (handle_ == 0) return {false, false, false};
    if (auto accessor = SharedDataHub::instance().mechanics_storage().try_acquire_read(handle_)) {
        uint8_t mask = accessor->angular_lock_mask;
        return {(mask & 0b001) != 0, (mask & 0b010) != 0, (mask & 0b100) != 0};
    }
    return {false, false, false};
}

void Corona::API::Mechanics::set_collision_callback(
    std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> callback) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_collision_callback] Invalid mechanics handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        // Store a callback that accepts std::array<float,3> to match nanobind-convertible types.
        accessor->collision_callback = [callback](std::uintptr_t other, bool began, const std::array<float, 3>& normal_arr, const std::array<float, 3>& point_arr) {
            if (callback) {
                callback(other, began, normal_arr, point_arr);
            }
        };
    } else {
        CFW_LOG_ERROR("[Mechanics::set_collision_callback] Failed to acquire write access to mechanics storage");
    }
}

void Corona::API::Mechanics::set_on_move_callback(
    std::function<void()> callback) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Mechanics::set_on_move_callback] Invalid mechanics handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().mechanics_storage().acquire_write(handle_)) {
        accessor->on_move_callback = std::move(callback);
        CFW_LOG_DEBUG("[Mechanics::set_on_move_callback] Callback set for handle {}", handle_);
    } else {
        CFW_LOG_ERROR("[Mechanics::set_on_move_callback] Failed to acquire write access");
    }
}

// ########################
//       Acoustics
// ########################
Corona::API::Acoustics::Acoustics(Geometry& geo)
    : geometry_(&geo), handle_(0) {
    if (!geo.is_valid()) {
        CFW_LOG_WARNING("[Acoustics::Acoustics] Invalid geometry; acoustics component not created");
        return;
    }

    handle_ = SharedDataHub::instance().acoustics_storage().allocate();
    if (auto accessor = SharedDataHub::instance().acoustics_storage().acquire_write(handle_)) {
        accessor->geometry_handle = geo.get_handle();
    } else {
        CFW_LOG_ERROR("[Acoustics::Acoustics] Failed to acquire write access to acoustics storage");
        SharedDataHub::instance().acoustics_storage().deallocate(handle_);
        handle_ = 0;
    }
}

Corona::API::Acoustics::~Acoustics() {
    if (handle_ != 0) {
        SharedDataHub::instance().acoustics_storage().deallocate(handle_);
    }
}

void Corona::API::Acoustics::set_volume(float volume) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Acoustics::set_volume] Invalid acoustics handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().acoustics_storage().acquire_write(handle_)) {
        accessor->volume = volume;
    } else {
        CFW_LOG_ERROR("[Acoustics::set_volume] Failed to acquire write access to acoustics storage");
    }
}

float Corona::API::Acoustics::get_volume() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Acoustics::get_volume] Invalid acoustics handle");
        return 0.0f;
    }

    float result = 0.0f;
    if (auto accessor = SharedDataHub::instance().acoustics_storage().acquire_read(handle_)) {
        result = accessor->volume;
    } else {
        CFW_LOG_ERROR("[Acoustics::get_volume] Failed to acquire read access to acoustics storage");
    }
    return result;
}

void Corona::API::Acoustics::set_audio_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Acoustics::set_audio_enabled] Invalid acoustics handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().acoustics_storage().acquire_write(handle_)) {
        accessor->audio_enabled = enabled;
    } else {
        CFW_LOG_ERROR("[Acoustics::set_audio_enabled] Failed to acquire write access to acoustics storage");
    }
}

bool Corona::API::Acoustics::get_audio_enabled() const {
    if (handle_ == 0) return true;
    if (auto accessor = SharedDataHub::instance().acoustics_storage().try_acquire_read(handle_)) {
        return accessor->audio_enabled;
    }
    return true;
}

void Corona::API::Acoustics::set_audio_resource(std::uint64_t resource_id) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Acoustics::set_audio_resource] Invalid acoustics handle");
        return;
    }
    if (auto accessor = SharedDataHub::instance().acoustics_storage().acquire_write(handle_)) {
        accessor->resource_id = resource_id;
    } else {
        CFW_LOG_ERROR("[Acoustics::set_audio_resource] Failed to acquire write access to acoustics storage");
    }
}

std::uint64_t Corona::API::Acoustics::get_audio_resource() const {
    if (handle_ == 0) return 0;
    if (auto accessor = SharedDataHub::instance().acoustics_storage().try_acquire_read(handle_)) {
        return accessor->resource_id;
    }
    return 0;
}

void Corona::API::Acoustics::play(bool loop) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Acoustics::play] Invalid acoustics handle");
        return;
    }
    std::uint64_t rid = get_audio_resource();
    if (rid == 0) {
        CFW_LOG_WARNING("[Acoustics::play] No audio resource bound to this component");
        return;
    }
    auto* event_bus = Kernel::KernelContext::instance().event_bus();
    if (!event_bus) {
        CFW_LOG_ERROR("[Acoustics::play] event_bus not available");
        return;
    }
    // 传自身 handle_ 作 acoustics_handle → 空间播放，声学线程按它解析位置。
    event_bus->publish<Events::PlayAudioEvent>({rid, loop, handle_});
}

void Corona::API::Acoustics::stop() {
    if (handle_ == 0) {
        return;
    }
    std::uint64_t rid = get_audio_resource();
    auto* event_bus = Kernel::KernelContext::instance().event_bus();
    if (!event_bus) {
        CFW_LOG_ERROR("[Acoustics::stop] event_bus not available");
        return;
    }
    event_bus->publish<Events::StopAudioEvent>({rid, handle_});
}

std::uintptr_t Corona::API::Acoustics::get_handle() const {
    return handle_;
}

Corona::API::Geometry* Corona::API::Acoustics::get_geometry() const {
    return geometry_;
}

// ########################
//          Actor
// ########################
Corona::API::Actor::Actor()
    : handle_(0), active_profile_handle_(0), next_profile_handle_(1) {
    handle_ = SharedDataHub::instance().actor_storage().allocate();
}

Corona::API::Actor::~Actor() {
    for (const auto& [_, storage_profile_handle] : profile_storage_handles_) {
        if (storage_profile_handle != 0) {
            SharedDataHub::instance().profile_storage().deallocate(storage_profile_handle);
        }
    }
    profile_storage_handles_.clear();

    if (handle_ != 0) {
        SharedDataHub::instance().clear_actor_metadata(handle_);
        SharedDataHub::instance().actor_storage().deallocate(handle_);
    }
}

Corona::API::Actor::Profile* Corona::API::Actor::add_profile(const Profile& profile) {
    if (!profile.geometry) {
        CFW_LOG_CRITICAL("[Actor::add_profile] Profile must have a valid Geometry");
        return nullptr;
    }
    if (!profile.geometry->is_valid()) {
        CFW_LOG_CRITICAL("[Actor::add_profile] Profile Geometry failed to load");
        return nullptr;
    }

    if (profile.optics && profile.optics->geometry_ != profile.geometry) {
        CFW_LOG_CRITICAL("[Actor::add_profile] Optics references a different Geometry");
        return nullptr;
    }

    if (profile.mechanics && profile.mechanics->geometry_ != profile.geometry) {
        CFW_LOG_CRITICAL("[Actor::add_profile] Mechanics references a different Geometry");
        return nullptr;
    }

    if (profile.acoustics && profile.acoustics->geometry_ != profile.geometry) {
        CFW_LOG_CRITICAL("[Actor::add_profile] Acoustics references a different Geometry");
        return nullptr;
    }

    std::uintptr_t profile_handle = next_profile_handle_++;
    profiles_[profile_handle] = profile;

    if (active_profile_handle_ == 0) {
        active_profile_handle_ = profile_handle;
    }

    // 将 Profile 写入引擎侧 ProfileStorage，ActorDevice 只保存 ProfileStorage 的句柄
    std::uintptr_t storage_profile_handle = SharedDataHub::instance().profile_storage().allocate();
    if (auto p = SharedDataHub::instance().profile_storage().acquire_write(storage_profile_handle)) {
        // Actor 作为组件类的 friend，可以读取组件句柄；但不能直接调用 Geometry 的受保护 get_handle()
        p->optics_handle = profile.optics ? profile.optics->get_handle() : 0;
        p->acoustics_handle = profile.acoustics ? profile.acoustics->get_handle() : 0;
        p->mechanics_handle = profile.mechanics ? profile.mechanics->get_handle() : 0;
        p->geometry_handle = 0;
    } else {
        CFW_LOG_ERROR("[Actor::add_profile] Failed to acquire write access to profile storage");
        SharedDataHub::instance().profile_storage().deallocate(storage_profile_handle);
        storage_profile_handle = 0;
    }
    profile_storage_handles_[profile_handle] = storage_profile_handle;

    if (handle_ != 0) {
        if (auto accessor = SharedDataHub::instance().actor_storage().acquire_write(handle_)) {
            if (storage_profile_handle != 0) {
                accessor->profile_handles.push_back(storage_profile_handle);
            }
            // 将 Geometry 的 model_path 同步到 ActorDevice，
            // GeometrySystem 的 load/unload 路径依赖此字段
            accessor->model_path = profile.geometry->get_model_path();
        } else {
            CFW_LOG_ERROR("[Actor::add_profile] Failed to acquire write access to actor storage");
        }
    }

    return &profiles_[profile_handle];
}

void Corona::API::Actor::remove_profile(const Profile* profile) {
    if (handle_ == 0 || profile == nullptr) {
        CFW_LOG_WARNING("[Actor::remove_profile] Invalid actor handle or null profile");
        return;
    }

    std::uintptr_t profile_handle = 0;
    for (const auto& [handle, prof] : profiles_) {
        if (&prof == profile) {
            profile_handle = handle;
            break;
        }
    }

    if (profile_handle == 0) {
        CFW_LOG_WARNING("[Actor::remove_profile] Profile not found in this actor");
        return;
    }

    auto it = profiles_.find(profile_handle);
    if (it == profiles_.end()) {
        return;
    }

    const auto storage_profile_it = profile_storage_handles_.find(profile_handle);
    const std::uintptr_t storage_profile_handle =
        storage_profile_it != profile_storage_handles_.end() ? storage_profile_it->second : 0;

    profiles_.erase(it);
    profile_storage_handles_.erase(profile_handle);

    if (active_profile_handle_ == profile_handle) {
        if (!profiles_.empty()) {
            active_profile_handle_ = profiles_.begin()->first;
        } else {
            active_profile_handle_ = 0;
        }
    }

    if (auto accessor = SharedDataHub::instance().actor_storage().acquire_write(handle_)) {
        if (storage_profile_handle != 0) {
            std::erase(accessor->profile_handles, storage_profile_handle);
        }
    }

    if (storage_profile_handle != 0) {
        SharedDataHub::instance().profile_storage().deallocate(storage_profile_handle);
    }
}

void Corona::API::Actor::set_active_profile(const Profile* profile) {
    if (profile == nullptr) {
        CFW_LOG_WARNING("[Actor::set_active_profile] Null profile pointer");
        return;
    }

    for (const auto& [handle, prof] : profiles_) {
        if (&prof == profile) {
            active_profile_handle_ = handle;
            return;
        }
    }

    CFW_LOG_WARNING("[Actor::set_active_profile] Profile not found in this actor");
}

Corona::API::Actor::Profile* Corona::API::Actor::get_active_profile() {
    if (active_profile_handle_ == 0) return nullptr;
    auto it = profiles_.find(active_profile_handle_);
    return (it != profiles_.end()) ? &it->second : nullptr;
}

std::size_t Corona::API::Actor::profile_count() const {
    return profiles_.size();
}

void Corona::API::Actor::set_follow_camera(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::set_follow_camera] Invalid actor handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().actor_storage().acquire_write(handle_)) {
        accessor->follow_camera = enabled;
    } else {
        CFW_LOG_ERROR("[Actor::set_follow_camera] Failed to acquire write access to actor storage");
    }
}

bool Corona::API::Actor::get_follow_camera() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::get_follow_camera] Invalid actor handle");
        return false;
    }

    if (auto accessor = SharedDataHub::instance().actor_storage().try_acquire_read(handle_)) {
        return accessor->follow_camera;
    }

    CFW_LOG_ERROR("[Actor::get_follow_camera] Failed to acquire read access to actor storage");
    return false;
}

void Corona::API::Actor::set_actor_guid(const std::string& actor_guid) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::set_actor_guid] Invalid actor handle");
        return;
    }

    SharedDataHub::instance().set_actor_guid(handle_, actor_guid);
}

std::string Corona::API::Actor::get_actor_guid() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::get_actor_guid] Invalid actor handle");
        return {};
    }

    return SharedDataHub::instance().actor_guid(handle_);
}

void Corona::API::Actor::set_external_vision_binding(const std::string& source_path,
                                                     const std::string& shape_guid,
                                                     int shape_index,
                                                     const std::string& json_path,
                                                     const std::string& shape_type,
                                                     const std::string& shape_identity_key,
                                                     const std::string& model_path,
                                                     bool visible) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::set_external_vision_binding] Invalid actor handle");
        return;
    }

    ExternalVisionBindingDevice binding{};
    binding.enabled = true;
    binding.visible = visible;
    binding.source_path = source_path;
    binding.shape_guid = shape_guid;
    binding.shape_index = shape_index;
    binding.json_path = json_path;
    binding.shape_type = shape_type;
    binding.shape_identity_key = shape_identity_key;
    binding.model_path = model_path;
    SharedDataHub::instance().set_external_vision_binding(handle_, std::move(binding));
}

void Corona::API::Actor::clear_external_vision_binding() {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::clear_external_vision_binding] Invalid actor handle");
        return;
    }

    SharedDataHub::instance().clear_external_vision_binding(handle_);
}

bool Corona::API::Actor::has_external_vision_binding() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Actor::has_external_vision_binding] Invalid actor handle");
        return false;
    }

    return SharedDataHub::instance().has_external_vision_binding(handle_);
}

std::uintptr_t Corona::API::Actor::get_handle() const {
    return handle_;
}

// ########################
//          Camera
// ########################
Corona::API::Camera::Camera()
    : handle_(0) {
    ktm::fvec3 pos_vec;
    pos_vec.x = 0.0f;
    pos_vec.y = 0.0f;
    pos_vec.z = -5.0f;

    ktm::fvec3 fwd_vec;
    fwd_vec.x = 0.0f;
    fwd_vec.y = 0.0f;
    fwd_vec.z = 1.0f;

    ktm::fvec3 up_vec;
    up_vec.x = 0.0f;
    up_vec.y = 1.0f;
    up_vec.z = 0.0f;

    float fov = 45.0f;

    auto actor_pick_handle = SharedDataHub::instance().actor_pick_storage().allocate();
    handle_ = SharedDataHub::instance().camera_storage().allocate();
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_write(handle_)) {
        accessor->position = pos_vec;
        accessor->forward = fwd_vec;
        accessor->world_up = up_vec;
        accessor->fov = fov;
        accessor->width = static_cast<std::uint32_t>(width_);
        accessor->height = static_cast<std::uint32_t>(height_);
        accessor->aspect = static_cast<float>(width_) / static_cast<float>(height_);
        accessor->surface = get_default_surface();
        accessor->actor_pick_handle = actor_pick_handle;
    } else {
        CFW_LOG_ERROR("[Camera::Camera] Failed to acquire write access to camera storage");
        SharedDataHub::instance().camera_storage().deallocate(handle_);
        SharedDataHub::instance().actor_pick_storage().deallocate(actor_pick_handle);
        handle_ = 0;
    }
}

Corona::API::Camera::Camera(const std::array<float, 3>& position, const std::array<float, 3>& forward, const std::array<float, 3>& world_up, float fov)
    : handle_(0) {
    ktm::fvec3 pos_vec;
    pos_vec.x = position[0];
    pos_vec.y = position[1];
    pos_vec.z = position[2];

    ktm::fvec3 fwd_vec;
    fwd_vec.x = forward[0];
    fwd_vec.y = forward[1];
    fwd_vec.z = forward[2];

    ktm::fvec3 up_vec;
    up_vec.x = world_up[0];
    up_vec.y = world_up[1];
    up_vec.z = world_up[2];

    auto actor_pick_handle = SharedDataHub::instance().actor_pick_storage().allocate();
    handle_ = SharedDataHub::instance().camera_storage().allocate();
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_write(handle_)) {
        accessor->position = pos_vec;
        accessor->forward = fwd_vec;
        accessor->world_up = up_vec;
        accessor->fov = fov;
        accessor->width = static_cast<std::uint32_t>(width_);
        accessor->height = static_cast<std::uint32_t>(height_);
        accessor->aspect = static_cast<float>(width_) / static_cast<float>(height_);
        accessor->surface = get_default_surface();
        accessor->actor_pick_handle = actor_pick_handle;
    } else {
        CFW_LOG_ERROR("[Camera::Camera] Failed to acquire write access to camera storage");
        SharedDataHub::instance().camera_storage().deallocate(handle_);
        SharedDataHub::instance().actor_pick_storage().deallocate(actor_pick_handle);
        handle_ = 0;
    }
}

Corona::API::Camera::~Camera() {
    if (handle_) {
        std::uintptr_t actor_pick_handle = 0;
        if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(handle_)) {
            actor_pick_handle = camera->actor_pick_handle;
        }
        SharedDataHub::instance().enqueue_camera_release({
            .camera_handle = handle_,
            .actor_pick_handle = actor_pick_handle,
        });
        handle_ = 0;
    }
}

void Corona::API::Camera::set(const std::array<float, 3>& position, const std::array<float, 3>& forward, const std::array<float, 3>& world_up, float fov) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set] Invalid camera handle");
        return;
    }

    ktm::fvec3 pos_vec;
    pos_vec.x = position[0];
    pos_vec.y = position[1];
    pos_vec.z = position[2];

    ktm::fvec3 fwd_vec;
    fwd_vec.x = forward[0];
    fwd_vec.y = forward[1];
    fwd_vec.z = forward[2];

    ktm::fvec3 up_vec;
    up_vec.x = world_up[0];
    up_vec.y = world_up[1];
    up_vec.z = world_up[2];

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_write(handle_)) {
        accessor->position = pos_vec;
        accessor->forward = fwd_vec;
        accessor->world_up = up_vec;
        accessor->fov = fov;
        accessor->aspect = static_cast<float>(width_) / static_cast<float>(height_);
    } else {
        CFW_LOG_ERROR("[Camera::set] Failed to acquire write access to camera storage");
    }
}

std::array<float, 3> Corona::API::Camera::get_position() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_position] Invalid camera handle");
        return {0.0f, 0.0f, 0.0f};
    }

    std::array result = {0.0f, 0.0f, 0.0f};
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        result[0] = accessor->position.x;
        result[1] = accessor->position.y;
        result[2] = accessor->position.z;
    } else {
        CFW_LOG_ERROR("[Camera::get_position] Failed to acquire read access to camera storage");
    }

    return result;
}

std::array<float, 3> Corona::API::Camera::get_forward() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_forward] Invalid camera handle");
        return {0.0f, 0.0f, 1.0f};
    }

    std::array result = {0.0f, 0.0f, 1.0f};
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        result[0] = accessor->forward.x;
        result[1] = accessor->forward.y;
        result[2] = accessor->forward.z;
    } else {
        CFW_LOG_ERROR("[Camera::get_forward] Failed to acquire read access to camera storage");
    }

    return result;
}

std::array<float, 3> Corona::API::Camera::get_world_up() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_world_up] Invalid camera handle");
        return {0.0f, 1.0f, 0.0f};
    }

    std::array result = {0.0f, 1.0f, 0.0f};
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        result[0] = accessor->world_up.x;
        result[1] = accessor->world_up.y;
        result[2] = accessor->world_up.z;
    } else {
        CFW_LOG_ERROR("[Camera::get_world_up] Failed to acquire read access to camera storage");
    }

    return result;
}

float Corona::API::Camera::get_fov() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_fov] Invalid camera handle");
        return 45.0f;
    }

    float result = 45.0f;
    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        result = accessor->fov;
    } else {
        CFW_LOG_ERROR("[Camera::get_fov] Failed to acquire read access to camera storage");
    }

    return result;
}

std::uintptr_t Corona::API::Camera::get_handle() const {
    return handle_;
}

void Corona::API::Camera::set_surface(void* surface) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_surface] Invalid camera handle");
        return;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::Surface;
    command.surface = surface;
    SharedDataHub::instance().enqueue_camera_state_update(command);

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::DisplaySurfaceChangedEvent>({surface});
    }
}

void* Corona::API::Camera::get_surface() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_surface] Invalid camera handle");
        return nullptr;
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        return accessor->surface;
    }

    CFW_LOG_ERROR("[Camera::get_surface] Failed to acquire read access to camera storage");
    return nullptr;
}

void Corona::API::Camera::set_offscreen_capture_mode(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_offscreen_capture_mode] Invalid camera handle");
        return;
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_write(handle_)) {
        if (enabled) {
            accessor->surface = nullptr;
            accessor->follows_default_surface = false;
            accessor->view_open = false;
        } else {
            accessor->follows_default_surface = true;
            accessor->surface = get_default_surface();
        }
        return;
    }

    CFW_LOG_ERROR("[Camera::set_offscreen_capture_mode] Failed to acquire write access to camera storage");
}

void Corona::API::Camera::save_screenshot(const std::string& path) const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::save_screenshot] Invalid camera handle");
        return;
    }

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::ScreenshotRequestEvent>({handle_, path, nullptr});
    }
}

bool Corona::API::Camera::save_screenshot_sync(const std::string& path) const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::save_screenshot_sync] Invalid camera handle");
        return false;
    }

    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::ScreenshotRequestEvent>({handle_, path, std::move(promise)});
    } else {
        return false;
    }

    // Block until OpticsSystem processes this screenshot
    auto status = future.wait_for(std::chrono::seconds(10));
    if (status == std::future_status::timeout) {
        CFW_LOG_WARNING("[Camera::save_screenshot_sync] Timeout waiting for screenshot: {}", path);
        return false;
    }
    return future.get();
}

void Corona::API::Camera::set_output_mode(const std::string& mode) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_output_mode] Invalid camera handle");
        return;
    }

    CameraOutputMode output_mode = CameraOutputMode::FinalColor;
    if (mode == "base_color") {
        output_mode = CameraOutputMode::BaseColor;
    } else if (mode == "normal") {
        output_mode = CameraOutputMode::Normal;
    } else if (mode == "position") {
        output_mode = CameraOutputMode::WorldPosition;
    } else if (mode == "object_id") {
        output_mode = CameraOutputMode::ObjectID;
    } else if (mode == "visibility_buffer") {
        output_mode = CameraOutputMode::VisibilityBuffer;
    } else if (mode == "ssao") {
        output_mode = CameraOutputMode::SSAO;
    } else if (mode == "ssao_raw") {
        output_mode = CameraOutputMode::SSAORaw;
    } else if (mode == "shadow_mask_raw") {
        output_mode = CameraOutputMode::ShadowMaskRaw;
    } else if (mode == "shadow_mask") {
        output_mode = CameraOutputMode::ShadowMask;
    } else if (mode != "final_color") {
        CFW_LOG_WARNING("[Camera::set_output_mode] Unknown mode '{}', defaulting to final_color", mode);
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::OutputMode;
    command.output_mode = output_mode;
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

std::string Corona::API::Camera::get_output_mode() const {
    if (handle_ == 0) {
        return "final_color";
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        switch (accessor->output_mode) {
            case CameraOutputMode::BaseColor:
                return "base_color";
            case CameraOutputMode::Normal:
                return "normal";
            case CameraOutputMode::WorldPosition:
                return "position";
            case CameraOutputMode::ObjectID:
                return "object_id";
            case CameraOutputMode::VisibilityBuffer:
                return "visibility_buffer";
            case CameraOutputMode::SSAO:
                return "ssao";
            case CameraOutputMode::SSAORaw:
                return "ssao_raw";
            case CameraOutputMode::ShadowMaskRaw:
                return "shadow_mask_raw";
            case CameraOutputMode::ShadowMask:
                return "shadow_mask";
            case CameraOutputMode::FinalColor:
                [[fallthrough]];
            default:
                return "final_color";
        }
    }
    return "final_color";
}

void Corona::API::Camera::set_render_backend(const std::string& mode) {
    Corona::API::set_render_backend(mode, handle_);
}

std::string Corona::API::Camera::get_render_backend() const {
    return Corona::API::get_render_backend(handle_);
}

void Corona::API::Camera::set_vision_render_mode(const std::string& mode) {
    Corona::API::set_vision_render_mode(mode, handle_);
}

std::string Corona::API::Camera::get_vision_render_mode() const {
    return Corona::API::get_vision_render_mode(handle_);
}

void Corona::API::Camera::set_ssat_view_viewer(const std::string& mode,
                                               std::uint32_t view_index) {
    Corona::API::set_ssat_view_viewer(mode, view_index, handle_);
}

Corona::API::SsatViewViewerStatus Corona::API::Camera::get_ssat_view_viewer() const {
    return Corona::API::get_ssat_view_viewer(handle_);
}

void Corona::API::Camera::set_shadow_cascade_debug(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_shadow_cascade_debug] Invalid camera handle");
        return;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::ShadowCascadeDebug;
    command.shadow_cascade_debug = enabled;
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

bool Corona::API::Camera::get_shadow_cascade_debug() const {
    if (handle_ == 0) {
        return false;
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        return accessor->shadow_cascade_debug;
    }
    return false;
}

void Corona::API::Camera::set_ssao_enabled(bool enabled) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_ssao_enabled] Invalid camera handle");
        return;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::SsaoEnabled;
    command.ssao_enabled = enabled;
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

bool Corona::API::Camera::get_ssao_enabled() const {
    if (handle_ == 0) {
        return true;
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        return accessor->ssao_enabled;
    }
    return true;
}

void Corona::API::Camera::set_view_state(bool open, int x, int y, int width, int height, float move_speed) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_view_state] Invalid camera handle");
        return;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::ViewState;
    command.view_open = open;
    command.view_x = x;
    command.view_y = y;
    command.view_width = std::max(width, 1);
    command.view_height = std::max(height, 1);
    command.move_speed = std::max(move_speed, 0.01f);
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

std::array<float, 6> Corona::API::Camera::get_view_state() const {
    if (handle_ == 0) {
        return {0.0f, 120.0f, 120.0f, 960.0f, 540.0f, 1.0f};
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        return {
            accessor->view_open ? 1.0f : 0.0f,
            static_cast<float>(accessor->view_x),
            static_cast<float>(accessor->view_y),
            static_cast<float>(accessor->view_width),
            static_cast<float>(accessor->view_height),
            accessor->move_speed,
        };
    }
    return {0.0f, 120.0f, 120.0f, 960.0f, 540.0f, 1.0f};
}

// ########################
//      ImageEffects
// ########################
Corona::API::ImageEffects::ImageEffects()
    : handle_(0) {
}

Corona::API::ImageEffects::~ImageEffects() {
    if (handle_ != 0) {
        // Corona::SharedDataHub::instance().image_effects_storage().deallocate(handle_);
        handle_ = 0;
    }
}

// ########################
//   Camera (原 Viewport 功能)
// ########################
void Corona::API::Camera::set_image_effects(ImageEffects* effects) {
    image_effects_ = effects;
    // TODO: 如果有 image_effects_storage，在此写入
}

Corona::API::ImageEffects* Corona::API::Camera::get_image_effects() {
    return image_effects_;
}

bool Corona::API::Camera::has_image_effects() const {
    return image_effects_ != nullptr;
}

void Corona::API::Camera::remove_image_effects() {
    image_effects_ = nullptr;
    // TODO: 如果有 image_effects_storage，在此清理
}

void Corona::API::Camera::set_size(int width, int height) {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::set_size] Invalid camera handle");
        return;
    }

    if (width <= 0 || height <= 0) {
        CFW_LOG_WARNING("[Camera::set_size] Invalid size: {}x{}", width, height);
        return;
    }

    width_ = width;
    height_ = height;

    CameraStateUpdateCommand command{};
    command.camera_handle = handle_;
    command.fields = CameraStateUpdateField::Size;
    command.width = static_cast<std::uint32_t>(width_);
    command.height = static_cast<std::uint32_t>(height_);
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

std::array<int, 2> Corona::API::Camera::get_size() const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::get_size] Invalid camera handle");
        return {width_, height_};
    }

    if (auto accessor = SharedDataHub::instance().camera_storage().acquire_read(handle_)) {
        return {
            static_cast<int>(accessor->width),
            static_cast<int>(accessor->height),
        };
    }

    CFW_LOG_ERROR("[Camera::get_size] Failed to acquire read access to camera storage");
    return {width_, height_};
}

void Corona::API::Camera::set_viewport_rect(int x, int y, int width, int height) {
    // TODO: Implement viewport rectangle settings
    CFW_LOG_WARNING("[Camera::set_viewport_rect] Not implemented yet");
}

std::uintptr_t Corona::API::Camera::pick_actor_at_pixel(int x, int y) const {
    if (handle_ == 0) {
        CFW_LOG_WARNING("[Camera::pick_actor_at_pixel] Invalid camera handle");
        return 0;
    }

    if (x < 0 || y < 0) {
        return 0;
    }

    std::uintptr_t actor_pick_handle = 0;
    if (auto camera = SharedDataHub::instance().camera_storage().try_acquire_read(handle_)) {
        actor_pick_handle = camera->actor_pick_handle;
    }
    if (actor_pick_handle == 0) {
        return 0;
    }

    auto pick = SharedDataHub::instance().actor_pick_storage().try_acquire_write(actor_pick_handle);
    if (!pick) {
        return 0;
    }

    const auto ux = static_cast<std::uint32_t>(x);
    const auto uy = static_cast<std::uint32_t>(y);
    const std::uintptr_t completed_actor =
        (pick->result_ready && pick->result_x == ux && pick->result_y == uy)
            ? pick->actor_handle
            : 0;

    pick->x = ux;
    pick->y = uy;
    pick->pending = true;
    pick->result_ready = false;

    return completed_actor;
}

namespace Corona::API {
void set_editor_camera_input_enabled(bool enabled) {
    Systems::CameraFollowController::instance().set_input_enabled(enabled);
}

bool is_editor_camera_input_enabled() {
    return Systems::CameraFollowController::instance().is_input_enabled();
}

void set_default_surface(void* surface) {
    g_default_surface.store(surface, std::memory_order_relaxed);

    // 句柄到达时，补写到已存在的相机，避免“先有 camera 后有 surface”的空窗。
    for (auto& camera : SharedDataHub::instance().camera_storage()) {
        if (camera.follows_default_surface) {
            camera.surface = surface;
        }
    }
}

void* get_default_surface() {
    return g_default_surface.load(std::memory_order_relaxed);
}

bool is_vision_available() {
#ifdef CORONA_ENABLE_VISION
    return true;
#else
    return false;
#endif
}

void set_render_backend(const std::string& mode, std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    if (resolved_handle == 0) {
        CFW_LOG_WARNING("[set_render_backend] No camera is available");
        return;
    }

    int backend = mode == "vision" ? 1 : 0;
    if (backend == 1 && !is_vision_available()) {
        CFW_LOG_WARNING("[set_render_backend] Vision not compiled in; falling back to Native");
        backend = 0;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = resolved_handle;
    command.fields = CameraStateUpdateField::RenderBackend;
    command.render_backend =
        backend == 1 ? CameraRenderBackend::Vision : CameraRenderBackend::Native;
    SharedDataHub::instance().enqueue_camera_state_update(command);

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::RenderBackendSwitchEvent>({backend, resolved_handle});
    }
}

std::string get_render_backend(std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    if (resolved_handle != 0) {
        if (auto camera = SharedDataHub::instance().camera_storage().acquire_read(resolved_handle)) {
            return camera->render_backend == CameraRenderBackend::Vision ? "vision" : "native";
        }
    }
    return "native";
}

void set_vision_render_mode(const std::string& mode, std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    if (resolved_handle == 0) {
        CFW_LOG_WARNING("[set_vision_render_mode] No camera is available");
        return;
    }

    CameraStateUpdateCommand command{};
    command.camera_handle = resolved_handle;
    command.fields = CameraStateUpdateField::VisionRenderMode;
    command.vision_render_mode = parse_vision_render_mode(mode);
    SharedDataHub::instance().enqueue_camera_state_update(command);
}

std::string get_vision_render_mode(std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    if (resolved_handle != 0) {
        if (auto camera = SharedDataHub::instance().camera_storage().acquire_read(resolved_handle)) {
            return vision_render_mode_to_string(camera->vision_render_mode);
        }
    }
    return "path_tracing";
}

void set_ssat_view_viewer(const std::string& mode,
                         std::uint32_t view_index,
                         std::uintptr_t camera_handle) {
    const auto resolved_handle = resolve_camera_handle(camera_handle);
    if (resolved_handle == 0) {
        CFW_LOG_WARNING("[set_ssat_view_viewer] No camera is available");
        return;
    }

    SsatViewViewerMode viewer_mode{};
    if (!parse_ssat_view_viewer_mode(mode, viewer_mode)) {
        CFW_LOG_WARNING(
            "[set_ssat_view_viewer] Unknown mode '{}'; expected 'interlaced' or 'final_view'",
            mode);
        return;
    }

    SharedDataHub::instance().set_ssat_view_viewer_request(
        resolved_handle, viewer_mode, view_index);
}

SsatViewViewerStatus get_ssat_view_viewer(std::uintptr_t camera_handle) {
    return make_ssat_view_viewer_status(camera_handle);
}

void load_vision_scene(const std::string& path) {
    if (!is_vision_available()) {
        CFW_LOG_WARNING("[load_vision_scene] Vision not compiled in; request ignored");
        return;
    }

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        Events::VisionSceneLoadEvent event;
        event.scene_path = path;
        event_bus->publish<Events::VisionSceneLoadEvent>(std::move(event));
    }
}

void load_vision_scene_from_json(const std::string& json_text,
                                 const std::string& base_dir,
                                 const std::string& scene_key,
                                 bool external_live) {
    if (!is_vision_available()) {
        CFW_LOG_WARNING("[load_vision_scene_from_json] Vision not compiled in; request ignored");
        return;
    }

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        Events::VisionSceneLoadEvent event;
        event.scene_json = json_text;
        event.base_dir = base_dir;
        event.scene_key = scene_key;
        event.external_live = external_live;
        event_bus->publish<Events::VisionSceneLoadEvent>(std::move(event));
    }
}

// ########################
//          Media
// ########################
MediaInfo import_media(const std::string& path) {
    MediaInfo info{};

    auto rid = Resource::ResourceManager::get_instance().import_sync(Utils::utf8_to_path(path));
    if (rid == Resource::IResource::INVALID_UID) {
        CFW_LOG_ERROR("[import_media] Failed to import media: {}", path);
        return info;
    }

    // acquire_read<T> does an unchecked static_cast, so acquire the base
    // resource and dynamic_cast to determine the concrete media type.
    auto handle = Resource::ResourceManager::get_instance().acquire_read<Resource::IResource>(rid);
    if (!handle) {
        CFW_LOG_ERROR("[import_media] Imported resource is not ready: {}", path);
        return info;
    }

    const Resource::IResource* res = &(*handle);

    if (const auto* video = dynamic_cast<const Resource::Video*>(res)) {
        const auto& m = video->metadata();
        info.resource_id = rid;
        info.media_type = "video";
        info.duration_seconds = m.duration_seconds;
        info.codec = m.codec_name;
        info.width = m.width;
        info.height = m.height;
        info.fps = m.fps;
        return info;
    }

    if (const auto* audio = dynamic_cast<const Resource::Audio*>(res)) {
        const auto& m = audio->metadata();
        info.resource_id = rid;
        info.media_type = "audio";
        info.duration_seconds = m.duration_seconds;
        info.codec = m.codec_name;
        info.sample_rate = m.sample_rate;
        info.channels = m.channels;
        return info;
    }

    CFW_LOG_ERROR("[import_media] Imported resource is neither video nor audio: {}", path);
    return info;
}

// ########################
//     Audio Playback
// ########################
void play_audio(std::uint64_t resource_id, bool loop) {
    if (resource_id == 0) {
        CFW_LOG_WARNING("[play_audio] Invalid resource_id (0)");
        return;
    }
    auto* event_bus = Kernel::KernelContext::instance().event_bus();
    if (!event_bus) {
        CFW_LOG_ERROR("[play_audio] event_bus not available");
        return;
    }
    event_bus->publish<Events::PlayAudioEvent>({resource_id, loop});
}

void stop_audio(std::uint64_t resource_id) {
    if (resource_id == 0) {
        CFW_LOG_WARNING("[stop_audio] Invalid resource_id (0)");
        return;
    }
    auto* event_bus = Kernel::KernelContext::instance().event_bus();
    if (!event_bus) {
        CFW_LOG_ERROR("[stop_audio] event_bus not available");
        return;
    }
    event_bus->publish<Events::StopAudioEvent>({resource_id});
}

}  // namespace Corona::API
