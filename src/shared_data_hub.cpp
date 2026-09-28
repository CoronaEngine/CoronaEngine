#include <corona/shared_data_hub.h>
#include <corona/utils/scene_path_key.h>

#include <algorithm>

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
    return result;
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

void SharedDataHub::set_external_vision_binding(std::uintptr_t actor_handle,
                                                ExternalVisionBindingDevice binding,
                                                bool refresh_source_path) {
    if (actor_handle == 0) {
        return;
    }
    binding.enabled = true;
    if (!refresh_source_path) {
        std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
        const auto it = external_vision_bindings_.find(actor_handle);
        if (it != external_vision_bindings_.end() &&
            it->second.source_path == binding.source_path &&
            it->second.source_base_dir == binding.source_base_dir &&
            (binding.source_path.empty() || !it->second.source_path_key.empty())) {
            // Trust only the key already published by this hub, never a copied
            // caller key that may no longer match its source fields.
            binding.source_path_key = it->second.source_path_key;
            it->second = std::move(binding);
            return;
        }
    }
    // Filesystem access must happen outside the shared metadata lock. Publish
    // the complete value atomically, with the same last-writer semantics as set.
    binding.source_path_key = Utils::normalize_scene_path_key(
        binding.source_path, binding.source_base_dir);
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    external_vision_bindings_[actor_handle] = std::move(binding);
}

void SharedDataHub::refresh_external_vision_binding_paths() {
    std::unordered_map<std::uintptr_t, ExternalVisionBindingDevice> snapshot;
    {
        std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
        snapshot = external_vision_bindings_;
    }
    for (const auto& [handle, binding] : snapshot) {
        auto key = Utils::normalize_scene_path_key(binding.source_path, binding.source_base_dir);
        std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
        const auto it = external_vision_bindings_.find(handle);
        if (it != external_vision_bindings_.end() &&
            it->second.source_path == binding.source_path &&
            it->second.source_base_dir == binding.source_base_dir &&
            it->second.source_path_key == binding.source_path_key) {
            // Preserve unrelated edits made while resolving the path.
            it->second.source_path_key = std::move(key);
        }
    }
}

void SharedDataHub::clear_external_vision_binding(std::uintptr_t actor_handle) {
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    external_vision_bindings_.erase(actor_handle);
}

std::optional<ExternalVisionBindingDevice> SharedDataHub::external_vision_binding(
    std::uintptr_t actor_handle) const {
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    const auto it = external_vision_bindings_.find(actor_handle);
    if (it == external_vision_bindings_.end() || !it->second.enabled) {
        return std::nullopt;
    }
    return it->second;
}

bool SharedDataHub::has_external_vision_binding(std::uintptr_t actor_handle) const {
    return external_vision_binding(actor_handle).has_value();
}

void SharedDataHub::clear_actor_metadata(std::uintptr_t actor_handle) {
    std::lock_guard<std::mutex> lock(actor_metadata_mutex_);
    actor_guids_.erase(actor_handle);
    external_vision_bindings_.erase(actor_handle);
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

void SharedDataHub::enqueue_camera_move(CameraMoveCommand command) {
    if (command.camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(camera_move_mutex_);
    command.sequence = ++camera_move_sequence_;
    pending_camera_moves_[command.camera_handle] = command;
}

std::vector<CameraMoveCommand> SharedDataHub::drain_camera_moves() {
    std::vector<CameraMoveCommand> moves;
    {
        std::lock_guard<std::mutex> lock(camera_move_mutex_);
        moves.reserve(pending_camera_moves_.size());
        for (auto& [_, command] : pending_camera_moves_) {
            moves.push_back(command);
        }
        pending_camera_moves_.clear();
    }

    std::sort(moves.begin(), moves.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    return moves;
}

void SharedDataHub::enqueue_camera_viewport_update(CameraViewportUpdateCommand command) {
    if (command.camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(camera_viewport_update_mutex_);
    command.sequence = ++camera_viewport_update_sequence_;
    pending_camera_viewport_updates_[command.camera_handle] = command;
}

std::vector<CameraViewportUpdateCommand> SharedDataHub::drain_camera_viewport_updates() {
    std::vector<CameraViewportUpdateCommand> updates;
    {
        std::lock_guard<std::mutex> lock(camera_viewport_update_mutex_);
        updates.reserve(pending_camera_viewport_updates_.size());
        for (auto& [_, command] : pending_camera_viewport_updates_) {
            updates.push_back(command);
        }
        pending_camera_viewport_updates_.clear();
    }

    std::sort(updates.begin(), updates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sequence < rhs.sequence;
    });
    return updates;
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
    if (has_camera_state_field(command.fields, CameraStateUpdateField::ShadowCascadeDebug)) {
        pending.shadow_cascade_debug = command.shadow_cascade_debug;
    }
    if (has_camera_state_field(command.fields, CameraStateUpdateField::SsaoEnabled)) {
        pending.ssao_enabled = command.ssao_enabled;
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

void SharedDataHub::enqueue_camera_release(CameraReleaseCommand command) {
    if (command.camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(camera_release_mutex_);
    pending_camera_releases_.push_back(command);
}

std::vector<CameraReleaseCommand> SharedDataHub::drain_camera_releases() {
    std::vector<CameraReleaseCommand> releases;
    {
        std::lock_guard<std::mutex> lock(camera_release_mutex_);
        releases.swap(pending_camera_releases_);
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

void SharedDataHub::enqueue_viewport_ui_pointer(ViewportUiPointerCommand command) {
    if (command.camera_handle == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
    command.sequence = ++viewport_ui_pointer_sequence_;
    pending_viewport_ui_pointer_commands_.push_back(std::move(command));
}

std::vector<ViewportUiPointerCommand> SharedDataHub::drain_viewport_ui_pointer_commands() {
    std::vector<ViewportUiPointerCommand> commands;
    {
        std::lock_guard<std::mutex> lock(viewport_ui_mutex_);
        commands.swap(pending_viewport_ui_pointer_commands_);
    }
    return commands;
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
