#include <corona/shared_data_hub.h>
#include <corona/utils/scene_path_key.h>

#include <algorithm>
#include <utility>

namespace Corona {

ktm::fmat4x4 ModelTransform::compute_matrix() const {
    ktm::fquat qx = ktm::fquat::from_angle_x(euler_rotation.x);
    ktm::fquat qy = ktm::fquat::from_angle_y(euler_rotation.y);
    ktm::fquat qz = ktm::fquat::from_angle_z(euler_rotation.z);
    ktm::fquat rot_quat = qz * qy * qx;

    ktm::faffine3d affine;
    affine.translate(position).rotate(rot_quat).scale(scale);

    ktm::fmat4x4 result;
    affine >> result;
    return result * affine_basis;
}

SharedDataHub& SharedDataHub::instance() {
    static SharedDataHub instance;
    return instance;
}

// Storage accessors definitions
SharedDataHub::ModelResourceStorage& SharedDataHub::model_resource_storage() { return model_resource_storage_; }
const SharedDataHub::ModelResourceStorage& SharedDataHub::model_resource_storage() const { return model_resource_storage_; }

SharedDataHub::ModelTransformStorage& SharedDataHub::model_transform_storage() { return model_transform_storage_; }
const SharedDataHub::ModelTransformStorage& SharedDataHub::model_transform_storage() const { return model_transform_storage_; }

SharedDataHub::GeometryStorage& SharedDataHub::geometry_storage() { return geometry_storage_; }
const SharedDataHub::GeometryStorage& SharedDataHub::geometry_storage() const { return geometry_storage_; }

SharedDataHub::MechanicsStorage& SharedDataHub::mechanics_storage() { return mechanics_storage_; }
const SharedDataHub::MechanicsStorage& SharedDataHub::mechanics_storage() const { return mechanics_storage_; }

SharedDataHub::AcousticsStorage& SharedDataHub::acoustics_storage() { return acoustics_storage_; }
const SharedDataHub::AcousticsStorage& SharedDataHub::acoustics_storage() const { return acoustics_storage_; }

SharedDataHub::OpticsStorage& SharedDataHub::optics_storage() { return optics_storage_; }
const SharedDataHub::OpticsStorage& SharedDataHub::optics_storage() const { return optics_storage_; }

SharedDataHub::ProfileStorage& SharedDataHub::profile_storage() { return profile_storage_; }
const SharedDataHub::ProfileStorage& SharedDataHub::profile_storage() const { return profile_storage_; }

SharedDataHub::ActorStorage& SharedDataHub::actor_storage() { return actor_storage_; }
const SharedDataHub::ActorStorage& SharedDataHub::actor_storage() const { return actor_storage_; }

std::vector<std::uintptr_t> SharedDataHub::resolve_actor_geometry_handles(
    std::uintptr_t actor_handle) {
    std::vector<std::uintptr_t> geometry_handles;
    const auto append_geometry = [&geometry_handles](std::uintptr_t geometry_handle) {
        if (geometry_handle != 0 &&
            std::find(geometry_handles.begin(), geometry_handles.end(),
                      geometry_handle) == geometry_handles.end()) {
            geometry_handles.push_back(geometry_handle);
        }
    };

    const auto actor = actor_storage_.try_acquire_read(actor_handle);
    if (!actor) {
        return geometry_handles;
    }
    for (const auto profile_handle : actor->profile_handles) {
        const auto profile = profile_storage_.try_acquire_read(profile_handle);
        if (!profile) {
            continue;
        }
        append_geometry(profile->geometry_handle);
        if (const auto mechanics =
                mechanics_storage_.try_acquire_read(profile->mechanics_handle)) {
            append_geometry(mechanics->geometry_handle);
        }
        if (const auto optics =
                optics_storage_.try_acquire_read(profile->optics_handle)) {
            append_geometry(optics->geometry_handle);
        }
        if (const auto acoustics =
                acoustics_storage_.try_acquire_read(profile->acoustics_handle)) {
            append_geometry(acoustics->geometry_handle);
        }
    }
    return geometry_handles;
}

std::optional<std::uintptr_t>
SharedDataHub::resolve_actor_primary_transform_handle(
    std::uintptr_t actor_handle) {
    for (const auto geometry_handle :
         resolve_actor_geometry_handles(actor_handle)) {
        const auto geometry = geometry_storage_.try_acquire_read(geometry_handle);
        if (geometry && geometry->transform_handle != 0) {
            if (model_transform_storage_.try_acquire_read(
                    geometry->transform_handle)) {
                return geometry->transform_handle;
            }
        }
    }
    return std::nullopt;
}

void SharedDataHub::set_actor_guid(std::uintptr_t actor_handle, std::string actor_guid) {
    if (actor_handle == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    if (actor_guid.empty()) {
        actor_guids_.erase(actor_handle);
    } else {
        actor_guids_[actor_handle] = std::move(actor_guid);
    }
}

std::string SharedDataHub::actor_guid(std::uintptr_t actor_handle) const {
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    const auto it = actor_guids_.find(actor_handle);
    return it != actor_guids_.end() ? it->second : std::string{};
}






void SharedDataHub::clear_actor_metadata(std::uintptr_t actor_handle) {
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    actor_guids_.erase(actor_handle);
}

SharedDataHub::CameraStorage& SharedDataHub::camera_storage() { return camera_storage_; }
const SharedDataHub::CameraStorage& SharedDataHub::camera_storage() const { return camera_storage_; }

SharedDataHub::ActorPickStorage& SharedDataHub::actor_pick_storage() { return actor_pick_storage_; }
const SharedDataHub::ActorPickStorage& SharedDataHub::actor_pick_storage() const { return actor_pick_storage_; }

void SharedDataHub::enqueue_actor_pick_request(ActorPickRequestCommand command) {
    if (command.camera_handle == 0 || command.request_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(actor_pick_queue_mutex_);
    pending_actor_pick_requests_.push_back(std::move(command));
}

std::optional<ActorPickRequestCommand> SharedDataHub::take_actor_pick_request(
    std::uintptr_t camera_handle) {
    std::lock_guard<std::mutex> lock(actor_pick_queue_mutex_);
    const auto it = std::find_if(pending_actor_pick_requests_.begin(),
                                 pending_actor_pick_requests_.end(),
                                 [camera_handle](const auto& request) {
                                     return request.camera_handle == camera_handle;
                                 });
    if (it == pending_actor_pick_requests_.end()) {
        return std::nullopt;
    }
    auto request = std::move(*it);
    pending_actor_pick_requests_.erase(it);
    return request;
}

void SharedDataHub::enqueue_actor_pick_completion(ActorPickCompletion completion) {
    if (completion.camera_handle == 0 || completion.request_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(actor_pick_queue_mutex_);
    actor_pick_completions_.push_back(std::move(completion));
}

std::vector<ActorPickCompletion> SharedDataHub::drain_actor_pick_completions() {
    std::lock_guard<std::mutex> lock(actor_pick_queue_mutex_);
    std::vector<ActorPickCompletion> completions;
    completions.swap(actor_pick_completions_);
    return completions;
}

SharedDataHub::EnvironmentStorage& SharedDataHub::environment_storage() { return environment_storage_; }
const SharedDataHub::EnvironmentStorage& SharedDataHub::environment_storage() const { return environment_storage_; }

SharedDataHub::SceneStorage& SharedDataHub::scene_storage() { return scene_storage_; }
const SharedDataHub::SceneStorage& SharedDataHub::scene_storage() const { return scene_storage_; }

SharedDataHub::ImageStorage& SharedDataHub::image_storage() { return image_storage_; }
const SharedDataHub::ImageStorage& SharedDataHub::image_storage() const { return image_storage_; }

void SharedDataHub::publish_camera_frame_timing(std::uintptr_t camera_handle, double render_ms, double denoise_ms,
                                                CameraVisionRenderMode mode,
                                                std::chrono::steady_clock::time_point completed_at) {
    std::lock_guard<std::mutex> lock(camera_timing_mutex_);
    double frame_ms = 0.0;
    const auto previous = camera_timings_.find(camera_handle);
    if (previous != camera_timings_.end() && previous->second.mode == mode) {
        frame_ms = std::chrono::duration<double, std::milli>(
            completed_at - previous->second.recorded_at).count();
    }
    camera_timings_[camera_handle] = {render_ms, denoise_ms, frame_ms, mode, completed_at};
}

std::optional<CameraFrameTiming> SharedDataHub::camera_frame_timing(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_timing_mutex_);
    const auto it = camera_timings_.find(camera_handle);
    return it == camera_timings_.end() ? std::nullopt : std::optional{it->second};
}

namespace {

// CameraDevice -> 外部可读投影。只搬运"读者需要的字段"，内部状态（字符串设置、
// actor_pick_handle 等）留在 DAG 内部。
CameraSnapshotRow make_camera_row(std::uintptr_t handle, const CameraDevice& camera) {
    CameraSnapshotRow row;
    row.handle = handle;
    row.position = camera.position;
    row.forward = camera.forward;
    row.world_up = camera.world_up;
    row.fov = camera.fov;
    row.aspect = camera.aspect;
    row.near_plane = camera.near_plane;
    row.far_plane = camera.far_plane;
    row.projection_x_sign = camera.projection_x_sign;
    row.width = camera.width;
    row.height = camera.height;
    row.surface = camera.surface;
    row.follows_default_surface = camera.follows_default_surface;
    row.view_open = camera.view_open;
    row.view_x = camera.view_x;
    row.view_y = camera.view_y;
    row.view_width = camera.view_width;
    row.view_height = camera.view_height;
    row.move_speed = camera.move_speed;
    row.output_mode = camera.output_mode;
    row.render_backend = camera.render_backend;
    row.vision_render_mode = camera.vision_render_mode;
    row.vision_denoise = camera.vision_denoise;
    row.vision_denoiser = camera.vision_denoiser;
    row.vision_accumulation = camera.vision_accumulation;
    row.vision_stable_planes = camera.vision_stable_planes;
    return row;
}

}  // namespace

void SharedDataHub::publish_camera_snapshot(CameraSnapshot snapshot) {
    camera_snapshot_.publish(std::move(snapshot));
}

void SharedDataHub::publish_camera_snapshot_from_storage() {
    CameraSnapshot snapshot;
    snapshot.tick = camera_snapshot_.revision() + 1;
    const auto& storage = camera_storage_;
    for (const auto& camera : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&camera));
        if (handle == 0) {
            continue;
        }
        snapshot.rows.push_back(make_camera_row(handle, camera));
    }
    camera_snapshot_.publish(std::move(snapshot));
}

std::shared_ptr<const CameraSnapshot> SharedDataHub::camera_snapshot() const {
    return camera_snapshot_.read();
}

void SharedDataHub::seed_camera_snapshot_from_storage(std::uintptr_t camera_handle) {
    if (camera_handle == 0) {
        return;
    }

    // 建相机时槽位刚分配，除创建者外无人持有引用；这里读一次把首行播种下去，
    // 避免"创建后到首个 Optics tick 之间"的 get_* 读到默认值。
    std::optional<CameraSnapshotRow> seeded;
    if (auto camera = camera_storage_.try_acquire_read(camera_handle)) {
        seeded = make_camera_row(camera_handle, *camera);
    }
    if (!seeded.has_value()) {
        return;
    }

    CameraSnapshot next;
    if (auto current = camera_snapshot_.read()) {
        next = *current;
    }
    auto it = std::find_if(next.rows.begin(), next.rows.end(),
                           [camera_handle](const CameraSnapshotRow& candidate) {
                               return candidate.handle == camera_handle;
                           });
    if (it == next.rows.end()) {
        next.rows.push_back(*seeded);
    } else {
        *it = *seeded;
    }
    camera_snapshot_.publish(std::move(next));
}

void SharedDataHub::remove_camera_snapshot_row(std::uintptr_t camera_handle) {
    if (camera_handle == 0) {
        return;
    }

    auto current = camera_snapshot_.read();
    if (!current) {
        return;
    }
    CameraSnapshot next = *current;
    const auto before = next.rows.size();
    next.rows.erase(std::remove_if(next.rows.begin(), next.rows.end(),
                                   [camera_handle](const CameraSnapshotRow& row) {
                                       return row.handle == camera_handle;
                                   }),
                    next.rows.end());
    if (next.rows.size() != before) {
        camera_snapshot_.publish(std::move(next));
    }
}

std::optional<CameraSnapshotRow> SharedDataHub::camera_row(std::uintptr_t camera_handle) const {
    if (camera_handle == 0) {
        return std::nullopt;
    }
    auto snapshot = camera_snapshot_.read();
    if (!snapshot) {
        return std::nullopt;
    }
    for (const auto& row : snapshot->rows) {
        if (row.handle == camera_handle) {
            return row;
        }
    }
    return std::nullopt;
}

CameraSnapshotRow SharedDataHub::camera_view(std::uintptr_t camera_handle) const {
    CameraSnapshotRow view;
    view.handle = camera_handle;
    if (auto committed = camera_row(camera_handle)) {
        view = *committed;
    }

    // 叠加待处理的姿态命令（读己之写）：peek 内部自带锁，不再需要单独的互斥量
    if (const auto pending = camera_moves_.peek(camera_handle)) {
        view.position = pending->position;
        view.forward = pending->forward;
        view.world_up = pending->world_up;
        view.fov = pending->fov;
    }

    // 叠加待处理的状态命令（按 fields 位掩码部分更新）
    {
        std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
        if (auto it = pending_camera_state_updates_.find(camera_handle);
            it != pending_camera_state_updates_.end()) {
            const auto& update = it->second;
            if (has_camera_state_field(update.fields, CameraStateUpdateField::Projection)) {
                view.projection_x_sign = update.projection_x_sign;
                view.near_plane = update.near_plane;
                view.far_plane = update.far_plane;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::Surface)) {
                view.surface = update.surface;
                view.follows_default_surface = false;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::Size)) {
                view.width = update.width;
                view.height = update.height;
                view.aspect = static_cast<float>(update.width) /
                              static_cast<float>(std::max(update.height, 1u));
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::OutputMode)) {
                view.output_mode = update.output_mode;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::RenderBackend)) {
                view.render_backend = update.render_backend;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::VisionRenderMode)) {
                view.vision_render_mode = update.vision_render_mode;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::VisionDenoise)) {
                view.vision_denoise = update.vision_denoise;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::VisionDenoiser)) {
                view.vision_denoiser = update.vision_denoiser;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::VisionAccumulation)) {
                view.vision_accumulation = update.vision_accumulation;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::VisionStablePlanes)) {
                view.vision_stable_planes = update.vision_stable_planes;
            }
            if (has_camera_state_field(update.fields,
                                       CameraStateUpdateField::FollowsDefaultSurface)) {
                view.follows_default_surface = update.follows_default_surface;
            }
            if (has_camera_state_field(update.fields, CameraStateUpdateField::ViewState)) {
                view.view_open = update.view_open;
                view.view_x = update.view_x;
                view.view_y = update.view_y;
                view.view_width = update.view_width;
                view.view_height = update.view_height;
                view.move_speed = update.move_speed;
            }
        }
    }

    return view;
}

void SharedDataHub::enqueue_transform_update(TransformUpdateCommand command) {
    if (command.transform_handle == 0 || command.fields == TransformField::None) {
        return;
    }
    // LatestWins：同一个 transform 只保留最后一条，出队时按 CommandBuffer 内部
    // 单调序列排序，因此应用顺序与生产者线程的调度顺序无关。
    transform_updates_.push(command);
}

std::vector<TransformUpdateCommand> SharedDataHub::drain_transform_updates() {
    return transform_updates_.drain();
}

void SharedDataHub::clear_transform_updates(std::uintptr_t transform_handle) {
    transform_updates_.erase(transform_handle);
}

void SharedDataHub::publish_transform_snapshot_from_storage() {
    TransformSnapshot snapshot;
    snapshot.tick = transform_snapshot_.revision() + 1;
    const auto& storage = model_transform_storage_;
    for (const auto& transform : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&transform));
        if (handle == 0) {
            continue;
        }
        TransformSnapshotRow row;
        row.handle = handle;
        row.position = transform.position;
        row.euler_rotation = transform.euler_rotation;
        row.scale = transform.scale;
        row.affine_basis = transform.affine_basis;
        snapshot.rows.push_back(row);
    }
    transform_snapshot_.publish(std::move(snapshot));
}

std::shared_ptr<const TransformSnapshot> SharedDataHub::transform_snapshot() const {
    return transform_snapshot_.read();
}

std::optional<TransformSnapshotRow> SharedDataHub::transform_row(
    std::uintptr_t transform_handle) const {
    if (transform_handle == 0) {
        return std::nullopt;
    }
    auto snapshot = transform_snapshot_.read();
    if (!snapshot) {
        return std::nullopt;
    }
    for (const auto& row : snapshot->rows) {
        if (row.handle == transform_handle) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<TransformSnapshotRow> SharedDataHub::transform_view(
    std::uintptr_t transform_handle) const {
    if (transform_handle == 0) {
        return std::nullopt;
    }

    std::optional<TransformSnapshotRow> view = transform_row(transform_handle);
    if (auto pending = transform_updates_.peek(transform_handle)) {
        if (!view.has_value()) {
            view.emplace();
            view->handle = transform_handle;
        }
        if (has_transform_field(pending->fields, TransformField::Position)) {
            view->position = pending->position;
        }
        if (has_transform_field(pending->fields, TransformField::Rotation)) {
            view->euler_rotation = pending->euler_rotation;
        }
        if (has_transform_field(pending->fields, TransformField::Scale)) {
            view->scale = pending->scale;
        }
        if (has_transform_field(pending->fields, TransformField::Matrix)) {
            // 与 set_matrix() 的应用语义一致：矩阵已含平移，TRS 被归一化。
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) {
                    view->affine_basis[c][r] = pending->matrix[c * 4 + r];
                }
            }
            view->affine_basis[3] = ktm::fvec4{0.0f, 0.0f, 0.0f, 1.0f};
            view->position = pending->position;
            view->euler_rotation = ktm::fvec3{0.0f, 0.0f, 0.0f};
            view->scale = ktm::fvec3{1.0f, 1.0f, 1.0f};
        }
    }
    return view;
}

void SharedDataHub::enqueue_camera_move(CameraMoveCommand command) {
    if (command.camera_handle == 0) {
        return;
    }
    // LatestWins：同一台相机只保留最后一条；出队顺序由 CommandBuffer 内部
    // 单调序列决定，与生产者线程的调度顺序无关。
    camera_moves_.push(std::move(command));
}

std::vector<CameraMoveCommand> SharedDataHub::drain_camera_moves() {
    return camera_moves_.drain();
}

void SharedDataHub::enqueue_camera_viewport_update(CameraViewportUpdateCommand command) {
    if (command.camera_handle == 0) {
        return;
    }
    // LatestWins：同一台相机只保留最后一次视口状态；出队顺序由 buffer 内部序列决定。
    camera_viewport_updates_.push(std::move(command));
}

std::vector<CameraViewportUpdateCommand> SharedDataHub::drain_camera_viewport_updates() {
    return camera_viewport_updates_.drain();
}

void SharedDataHub::enqueue_default_surface_camera_update(DefaultSurfaceCameraUpdateCommand command) {
    if (!command.surface || command.width == 0 || command.height == 0) return;
    // 容量 1 + LatestWins = 单槽：新命令覆盖旧命令。
    default_surface_camera_update_.push(command);
}

std::optional<DefaultSurfaceCameraUpdateCommand> SharedDataHub::take_default_surface_camera_update() {
    auto updates = default_surface_camera_update_.drain();
    if (updates.empty()) {
        return std::nullopt;
    }
    return updates.front();
}

void SharedDataHub::enqueue_camera_state_update(CameraStateUpdateCommand command) {
    if (command.camera_handle == 0 ||
        command.fields == CameraStateUpdateField::None) {
        return;
    }

    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    auto& pending = pending_camera_state_updates_[command.camera_handle];
    if (pending.camera_handle == 0) {
        pending.camera_handle = command.camera_handle;
    }
    pending.sequence = ++camera_state_update_sequence_;
    pending.fields = pending.fields | command.fields;

    if (has_camera_state_field(command.fields, CameraStateUpdateField::Projection)) {
        pending.projection_x_sign = command.projection_x_sign;
        pending.near_plane = command.near_plane;
        pending.far_plane = command.far_plane;
    }

    if (has_camera_state_field(command.fields, CameraStateUpdateField::Surface)) {
        pending.surface = command.surface;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::Size)) {
        pending.width = command.width;
        pending.height = command.height;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::OutputMode)) {
        pending.output_mode = command.output_mode;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::RenderBackend)) {
        pending.render_backend = command.render_backend;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionRenderMode)) {
        pending.vision_render_mode = command.vision_render_mode;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionDenoise)) {
        pending.vision_denoise = command.vision_denoise;
        requested_camera_vision_denoise_[command.camera_handle] = {
            command.vision_denoise, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionDenoiser)) {
        pending.vision_denoiser = command.vision_denoiser;
        requested_camera_vision_denoiser_[command.camera_handle] = {
            command.vision_denoiser, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionReblurSettings)) {
        pending.vision_reblur_settings = command.vision_reblur_settings;
        requested_camera_vision_reblur_settings_[command.camera_handle] = {
            command.vision_reblur_settings, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionPtSettings)) {
        pending.vision_pt_settings = command.vision_pt_settings;
        requested_camera_vision_pt_settings_[command.camera_handle] = {
            command.vision_pt_settings, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionRestirSettings)) {
        pending.vision_restir_settings = command.vision_restir_settings;
        requested_camera_vision_restir_settings_[command.camera_handle] = {
            command.vision_restir_settings, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionRelaxSettings)) {
        pending.vision_relax_settings = command.vision_relax_settings;
        requested_camera_vision_relax_settings_[command.camera_handle] = {
            command.vision_relax_settings, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionAccumulation)) {
        pending.vision_accumulation = command.vision_accumulation;
        requested_camera_vision_accumulation_[command.camera_handle] = {
            command.vision_accumulation, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::VisionStablePlanes)) {
        pending.vision_stable_planes = command.vision_stable_planes;
        requested_camera_vision_stable_planes_[command.camera_handle] = {
            command.vision_stable_planes, pending.sequence};
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::ViewState)) {
        pending.view_open = command.view_open;
        pending.view_x = command.view_x;
        pending.view_y = command.view_y;
        pending.view_width = command.view_width;
        pending.view_height = command.view_height;
        pending.move_speed = command.move_speed;
    }
}

std::vector<CameraStateUpdateCommand> SharedDataHub::drain_camera_state_updates() {
    std::vector<CameraStateUpdateCommand> updates;
    {
        std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
        updates.reserve(pending_camera_state_updates_.size());
        for (auto& [_, command] : pending_camera_state_updates_) {
            updates.push_back(command);
        }
        pending_camera_state_updates_.clear();
    }

    std::sort(updates.begin(), updates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    return updates;
}

std::optional<bool> SharedDataHub::requested_camera_vision_denoise(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_denoise_.find(camera_handle);
    if (it != requested_camera_vision_denoise_.end()) {
        return it->second.enabled;
    }
    return std::nullopt;
}

std::optional<CameraVisionDenoiser> SharedDataHub::requested_camera_vision_denoiser(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_denoiser_.find(camera_handle);
    if (it != requested_camera_vision_denoiser_.end()) return it->second.type;
    return std::nullopt;
}

void SharedDataHub::acknowledge_camera_vision_denoiser(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_denoiser_.find(camera_handle);
    if (it != requested_camera_vision_denoiser_.end() && it->second.sequence <= applied_sequence) {
        requested_camera_vision_denoiser_.erase(it);
    }
}

std::optional<std::string> SharedDataHub::requested_camera_vision_reblur_settings(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_reblur_settings_.find(camera_handle);
    if (it != requested_camera_vision_reblur_settings_.end()) return it->second.json;
    return std::nullopt;
}

std::optional<std::string> SharedDataHub::requested_camera_vision_pt_settings(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_pt_settings_.find(camera_handle);
    if (it != requested_camera_vision_pt_settings_.end()) return it->second.json;
    return std::nullopt;
}

std::optional<std::string> SharedDataHub::requested_camera_vision_restir_settings(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_restir_settings_.find(camera_handle);
    if (it != requested_camera_vision_restir_settings_.end()) return it->second.json;
    return std::nullopt;
}

std::optional<std::string> SharedDataHub::requested_camera_vision_relax_settings(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_relax_settings_.find(camera_handle);
    if (it != requested_camera_vision_relax_settings_.end()) return it->second.json;
    return std::nullopt;
}

void SharedDataHub::acknowledge_camera_vision_reblur_settings(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_reblur_settings_.find(camera_handle);
    if (it != requested_camera_vision_reblur_settings_.end() && it->second.sequence <= applied_sequence) {
        requested_camera_vision_reblur_settings_.erase(it);
    }
}

void SharedDataHub::acknowledge_camera_vision_pt_settings(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_pt_settings_.find(camera_handle);
    if (it != requested_camera_vision_pt_settings_.end() && it->second.sequence <= applied_sequence) {
        requested_camera_vision_pt_settings_.erase(it);
    }
}

void SharedDataHub::acknowledge_camera_vision_restir_settings(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_restir_settings_.find(camera_handle);
    if (it != requested_camera_vision_restir_settings_.end() && it->second.sequence <= applied_sequence) {
        requested_camera_vision_restir_settings_.erase(it);
    }
}

void SharedDataHub::acknowledge_camera_vision_relax_settings(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_relax_settings_.find(camera_handle);
    if (it != requested_camera_vision_relax_settings_.end() && it->second.sequence <= applied_sequence) {
        requested_camera_vision_relax_settings_.erase(it);
    }
}

void SharedDataHub::acknowledge_camera_vision_denoise(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_denoise_.find(camera_handle);
    if (it != requested_camera_vision_denoise_.end() &&
        it->second.sequence <= applied_sequence) {
        requested_camera_vision_denoise_.erase(it);
    }
}

std::optional<bool> SharedDataHub::requested_camera_vision_accumulation(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_accumulation_.find(camera_handle);
    if (it != requested_camera_vision_accumulation_.end()) {
        return it->second.enabled;
    }
    return std::nullopt;
}

void SharedDataHub::acknowledge_camera_vision_accumulation(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_accumulation_.find(camera_handle);
    if (it != requested_camera_vision_accumulation_.end() &&
        it->second.sequence <= applied_sequence) {
        requested_camera_vision_accumulation_.erase(it);
    }
}

std::optional<bool> SharedDataHub::requested_camera_vision_stable_planes(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_stable_planes_.find(camera_handle);
    if (it != requested_camera_vision_stable_planes_.end()) {
        return it->second.enabled;
    }
    return std::nullopt;
}

void SharedDataHub::acknowledge_camera_vision_stable_planes(
    std::uintptr_t camera_handle, std::uint64_t applied_sequence) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    const auto it = requested_camera_vision_stable_planes_.find(camera_handle);
    if (it != requested_camera_vision_stable_planes_.end() &&
        it->second.sequence <= applied_sequence) {
        requested_camera_vision_stable_planes_.erase(it);
    }
}

void SharedDataHub::clear_camera_state_updates(std::uintptr_t camera_handle) {
    std::lock_guard<std::mutex> lock(camera_state_update_mutex_);
    pending_camera_state_updates_.erase(camera_handle);
    requested_camera_vision_denoise_.erase(camera_handle);
    requested_camera_vision_denoiser_.erase(camera_handle);
    requested_camera_vision_reblur_settings_.erase(camera_handle);
    requested_camera_vision_pt_settings_.erase(camera_handle);
    requested_camera_vision_restir_settings_.erase(camera_handle);
    requested_camera_vision_relax_settings_.erase(camera_handle);
    requested_camera_vision_accumulation_.erase(camera_handle);
    requested_camera_vision_stable_planes_.erase(camera_handle);
}

void SharedDataHub::enqueue_camera_release(CameraReleaseCommand command) {
    if (command.camera_handle == 0) {
        return;
    }

    clear_camera_state_updates(command.camera_handle);
    // Fifo：同一相机多次释放不会被合并，顺序保持不变。
    camera_releases_.push(command);
}

std::vector<CameraReleaseCommand> SharedDataHub::drain_camera_releases() {
    auto releases = camera_releases_.drain();
    if (!releases.empty()) {
        std::lock_guard<std::mutex> lock(camera_timing_mutex_);
        for (const auto& release : releases) camera_timings_.erase(release.camera_handle);
    }
    return releases;
}

void SharedDataHub::set_viewport_ui_mode(std::uintptr_t camera_handle, ViewportUiMode mode) {
    if (camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    auto& state = viewport_ui_states_[camera_handle];
    state.camera_handle = camera_handle;
    state.mode = mode;
}

void SharedDataHub::set_viewport_ui_calibration(std::uintptr_t camera_handle,
                                                const ViewportUiCalibration& calibration) {
    if (camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    auto& state = viewport_ui_states_[camera_handle];
    state.camera_handle = camera_handle;
    state.calibration = calibration;
}

ViewportUiState SharedDataHub::viewport_ui_state(std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    const auto it = viewport_ui_states_.find(camera_handle);
    if (it != viewport_ui_states_.end()) {
        return it->second;
    }
    ViewportUiState state;
    state.camera_handle = camera_handle;
    return state;
}

void SharedDataHub::set_viewport_ui_pointer(std::uintptr_t camera_handle,
                                              ViewportUiPointerState pointer) {
    if (camera_handle == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    auto& state = viewport_ui_states_[camera_handle];
    state.camera_handle = camera_handle;
    state.pointer = pointer;
}

void SharedDataHub::clear_viewport_ui_pointer(std::uintptr_t camera_handle,
                                                std::uint64_t owner_context) {
    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    const auto it = viewport_ui_states_.find(camera_handle);
    if (it == viewport_ui_states_.end()) {
        return;
    }
    if (owner_context != 0 && it->second.pointer.owner_context != owner_context) {
        return;
    }
    viewport_ui_states_.erase(it);
}

void SharedDataHub::set_viewport_gizmo_target(ViewportGizmoTarget target) {
    if (target.camera_handle == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(viewport_gizmo_mutex_);
    auto& state = viewport_gizmo_states_[target.camera_handle];
    const bool changed = state.target.actor_handle != target.actor_handle ||
                         state.target.scene_id != target.scene_id ||
                         state.target.actor_name != target.actor_name;
    state.target = std::move(target);
    state.sequence = ++viewport_gizmo_sequence_;
    if (changed) {
        state.hover_axis = ViewportGizmoAxis::None;
        state.active_axis = ViewportGizmoAxis::None;
        state.dragging = false;
    }
}

void SharedDataHub::clear_viewport_gizmo_target(std::uintptr_t camera_handle) {
    if (camera_handle == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(viewport_gizmo_mutex_);
    viewport_gizmo_states_.erase(camera_handle);
    ++viewport_gizmo_sequence_;
}

ViewportGizmoState SharedDataHub::viewport_gizmo_state(
    std::uintptr_t camera_handle) const {
    std::lock_guard<std::mutex> lock(viewport_gizmo_mutex_);
    const auto it = viewport_gizmo_states_.find(camera_handle);
    if (it != viewport_gizmo_states_.end()) {
        return it->second;
    }
    ViewportGizmoState state;
    state.target.camera_handle = camera_handle;
    return state;
}

void SharedDataHub::update_viewport_gizmo_interaction(
    std::uintptr_t camera_handle,
    ViewportGizmoAxis axis,
    bool dragging,
    ViewportGizmoAxis hover_axis) {
    if (camera_handle == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(viewport_gizmo_mutex_);
    auto& state = viewport_gizmo_states_[camera_handle];
    state.target.camera_handle = camera_handle;
    state.active_axis = dragging ? axis : ViewportGizmoAxis::None;
    state.hover_axis = dragging ? axis : hover_axis;
    state.dragging = dragging;
    state.sequence = ++viewport_gizmo_sequence_;
}
}  // namespace Corona
