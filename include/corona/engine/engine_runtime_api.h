#pragma once

#include <corona/kernel/entity_id.h>

#include <corona/systems/geometry/model_node_state.h>
#include <corona/resource/model_import_options.h>
#include <array>
#include <nlohmann/json_fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Corona {
class Model;

namespace API {

struct GeometryRenderStatus {
    bool observed{false};
    bool ready{false};
    bool failed{false};
    bool collision_ready{false};
    std::string gpu_build_state{"Unavailable"};
    std::size_t mesh_count{0};
    std::size_t renderable_mesh_count{0};
    std::size_t invalid_mesh_count{0};
};

// ============================================================================
// UI 侧可设置当前默认显示 surface（例如 SDL 原生窗口句柄）。
// ============================================================================
void set_default_surface(void* surface);
[[nodiscard]] void* get_default_surface();

// Editor camera input ownership. Blockly/game runtimes disable only editor
// camera controls; keyboard and mouse events continue to reach script input.
void set_editor_camera_input_enabled(bool enabled);
[[nodiscard]] bool is_editor_camera_input_enabled();

// ============================================================================
// Geometry: 作为所有组件的锚点，存储位置/旋转/缩放和模型数据
// ============================================================================
class Geometry {
   public:
    explicit Geometry(const std::string& model_path, Resource::ModelImportOptions options = {});
    ~Geometry();

    Geometry(const Geometry&) = delete;
    Geometry& operator=(const Geometry&) = delete;
    Geometry(Geometry&& other) noexcept;
    Geometry& operator=(Geometry&& other) noexcept;

    /// 从图片文件创建一个带贴图的 quad（两个三角形）几何，用作 UI 平面。
    /// 与普通模型几何不同：不经 Resource::Scene，程序化生成顶点并直接把图片作为
    /// albedo texture 上传。失败时返回一个所有 handle 为 0 的空 Geometry。
    [[nodiscard]] static Geometry from_image(const std::string& image_path);

    [[nodiscard]] std::vector<ModelNodeState> get_model_nodes() const;
    /// Node totals of the same model without materializing the node list.  The
    /// editor scene tree refreshes on the CEF UI thread, where copying thousands
    /// of node states per actor is not affordable.
    [[nodiscard]] ModelNodes::Counts get_model_node_counts() const;
    [[nodiscard]] std::optional<std::array<float,6>> get_model_node_aabb(uint32_t node_index) const;
    bool set_model_node_state(uint32_t node_index, const ModelNodeState& state);
    [[nodiscard]] std::vector<ModelNodeOverride> get_model_node_overrides() const;
    bool set_model_node_overrides(const std::vector<ModelNodeOverride>& overrides);

    /// The single inbound channel for a geometry transform: one complete TRS, one command.
    ///
    /// The decomposed set_position/set_rotation/set_scale pair-up was removed with it. Each of
    /// those enqueued its own single-field TransformUpdateCommand, and SharedDataHub coalesces per
    /// transform handle, so a caller that set position then rotation then scale only ever had the
    /// LAST field applied - actor creation, transform patches, property patches and resource
    /// rebinding all silently lost position and rotation. Callers that hold a partial patch must
    /// complete it from the current values first, exactly as Camera::set() requires a full pose.
    void set_transform(const std::array<float, 3>& position,
                       const std::array<float, 3>& rotation,
                       const std::array<float, 3>& scale);
    // Column-major affine matrix. Preserve shear and reflection without TRS decomposition.
    void set_matrix(const std::array<float, 16>& matrix);
    [[nodiscard]] std::array<float, 16> get_matrix() const;
    void set_native_local_correction(const std::array<float, 3>& offset, float scale);

    [[nodiscard]] std::array<float, 3> get_position() const;
    [[nodiscard]] std::array<float, 3> get_rotation() const;
    [[nodiscard]] std::array<float, 3> get_scale() const;

    /// 获取模型 AABB，返回 {min_x, min_y, min_z, max_x, max_y, max_z}
    [[nodiscard]] std::array<float, 6> get_aabb() const;

    /// 尽力而为的 AABB：不等待被占用的锁，拿不到就返回 nullopt。
    /// 绘制编辑器、派发输入的线程必须用它——那里的无界等待会冻住整个 UI。
    [[nodiscard]] std::optional<std::array<float, 6>> try_get_aabb() const;

    /// 获取构造时传入的模型文件路径，用于 Actor 身份标识和资源加载/卸载
    [[nodiscard]] const std::filesystem::path& get_model_path() const;
    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] std::string get_gpu_build_state() const;
    [[nodiscard]] std::size_t get_mesh_count() const;
    [[nodiscard]] std::uint64_t get_model_id() const;
    [[nodiscard]] GeometryRenderStatus get_render_status() const;
    /// Instance geometry storage handle, also used by editor configuration APIs.
    [[nodiscard]] EntityId get_handle() const;

   private:
    friend class Mechanics;
    friend class Optics;
    friend class Acoustics;

    /// 空几何（所有 handle 为 0）。仅供 from_image 等静态工厂内部使用。
    Geometry() = default;

   protected:
    [[nodiscard]] EntityId get_transform_handle() const;
    [[nodiscard]] EntityId get_model_resource_handle() const;

   private:
    EntityId handle_{};
    EntityId transform_handle_{};
    EntityId model_resource_handle_{};
    std::filesystem::path model_path_;
};

// ============================================================================
// Mechanics: 物理/力学组件，依赖 Geometry
// ============================================================================

/// One mechanics edit. Omitted fields are left untouched, so a single-field change reads as
/// set_properties({.body_type = "phantom"}) and a batch as several designated initializers.
/// This replaces the six set_mass / set_restitution / set_damping / set_body_type /
/// set_linear_lock / set_angular_lock calls that had to be issued one field at a time.
struct MechanicsProperties {
    std::optional<float> mass;
    std::optional<float> restitution;
    std::optional<float> damping;
    std::optional<std::string> body_type;
    std::optional<std::array<bool, 3>> linear_lock;
    std::optional<std::array<bool, 3>> angular_lock;
};

/// One mechanics read: every field from a single snapshot, so a caller reading several fields
/// cannot observe a combination that never existed. Each point getter used to rebuild the whole
/// snapshot+pending view, and the mechanics tick may apply a pending command between two of them.
struct MechanicsState {
    float mass{1.0f};
    float restitution{0.8f};
    float damping{0.99f};
    std::string body_type{"dynamic"};
    std::array<bool, 3> linear_lock{};
    std::array<bool, 3> angular_lock{};
};

class Mechanics {
   public:
    explicit Mechanics(Geometry& geo);
    ~Mechanics();

    /// The single mechanics write entry point: one command carrying every field given.
    void set_properties(const MechanicsProperties& properties);
    /// The single mechanics read entry point: one consistent snapshot of every field.
    [[nodiscard]] MechanicsState get_state() const;

    // 设置碰撞回调（参数为对方 actor 句柄、began(true=enter,false=exit)、法线、碰撞点）
    void set_collision_callback(std::function<void(EntityId, bool, const std::array<float, 3>&, const std::array<float, 3>&)> callback);

    // 设置移动回调
    void set_on_move_callback(std::function<void()> callback);

   private:
    friend class Actor;

    [[nodiscard]] EntityId get_handle() const;
    [[nodiscard]] Geometry* get_geometry() const;

    Geometry* geometry_;
    EntityId handle_{};
};

// ============================================================================
// Optics: 光学/渲染组件，依赖 Geometry
// ============================================================================
/// One optics edit. Omitted fields are left untouched, so a single-field change reads as
/// set_properties({.metallic = 1.0f}) and a material batch as several designated initializers.
/// Replaces the sixteen per-field setters (metallic/roughness/subsurface/specular/specular_tint/
/// anisotropic/sheen/sheen_tint/clearcoat/clearcoat_gloss/visible/lighting_enabled/ambient/
/// diffuse/specular_color/shininess) that had to be issued one field at a time.
struct OpticsProperties {
    std::optional<float> metallic;
    std::optional<float> roughness;
    std::optional<float> subsurface;
    std::optional<float> specular;
    std::optional<float> specular_tint;
    std::optional<float> anisotropic;
    std::optional<float> sheen;
    std::optional<float> sheen_tint;
    std::optional<float> clearcoat;
    std::optional<float> clearcoat_gloss;
    std::optional<bool> visible;
    std::optional<bool> lighting_enabled;
    std::optional<std::array<float, 3>> ambient;
    std::optional<std::array<float, 3>> diffuse;
    std::optional<std::array<float, 3>> specular_color;
    std::optional<float> shininess;
};

/// One optics read: every field from a single snapshot. actor_to_json read five of these fields
/// in one expression, and each point getter rebuilt the snapshot+pending view, so the result could
/// straddle a pending optics command being applied.
struct OpticsState {
    float metallic{0.0f};
    float roughness{0.5f};
    float subsurface{0.0f};
    float specular{0.5f};
    float specular_tint{0.0f};
    float anisotropic{0.0f};
    float sheen{0.0f};
    float sheen_tint{0.5f};
    float clearcoat{0.0f};
    float clearcoat_gloss{1.0f};
    bool visible{true};
    bool lighting_enabled{true};
    std::array<float, 3> ambient{0.2f, 0.2f, 0.2f};
    std::array<float, 3> diffuse{0.8f, 0.8f, 0.8f};
    std::array<float, 3> specular_color{1.0f, 1.0f, 1.0f};
    float shininess{32.0f};
};

class Optics {
   public:
    explicit Optics(Geometry& geo);
    ~Optics();

    /// The single optics write entry point: one command carrying every field given.
    void set_properties(const OpticsProperties& properties);
    /// The single optics read entry point: one consistent snapshot of every field.
    [[nodiscard]] OpticsState get_state() const;

   private:
    friend class Actor;

    [[nodiscard]] EntityId get_handle() const;
    [[nodiscard]] Geometry* get_geometry() const;

    Geometry* geometry_;
    EntityId handle_{};
};

// ============================================================================
// Acoustics: 声学组件，依赖 Geometry
// ============================================================================
/// One acoustics edit. Omitted fields are left untouched. Replaces the three per-field setters
/// (set_volume / set_audio_enabled / set_audio_resource) that had to be issued one at a time.
struct AcousticsProperties {
    std::optional<float> volume;
    std::optional<bool> audio_enabled;
    std::optional<std::uint64_t> audio_resource;
};

/// One acoustics read: every field from a single snapshot.
struct AcousticsState {
    float volume{1.0f};
    bool audio_enabled{true};
    std::uint64_t audio_resource{0};
};

class Acoustics {
   public:
    explicit Acoustics(Geometry& geo);
    ~Acoustics();

    /// The single acoustics write entry point: one command carrying every field given.
    void set_properties(const AcousticsProperties& properties);
    /// The single acoustics read entry point: one consistent snapshot of every field.
    [[nodiscard]] AcousticsState get_state() const;
    /// 在本组件（actor）的世界位置处播放绑定的音频
    void play(bool loop);
    /// 停止本组件的播放
    void stop();

   private:
    friend class Actor;

    [[nodiscard]] EntityId get_handle() const;
    [[nodiscard]] Geometry* get_geometry() const;

    Geometry* geometry_;
    EntityId handle_{};
};

// ============================================================================
// Actor: OOP 风格的实体类，支持多套组件和多个 Geometry
// ============================================================================
class Actor {
   public:
    Actor();
    ~Actor();

    struct Profile {
        Optics* optics{nullptr};
        Acoustics* acoustics{nullptr};
        Mechanics* mechanics{nullptr};
        Geometry* geometry{nullptr};
    };

    Profile* add_profile(const Profile& profile);
    void remove_profile(const Profile* profile);
    void set_active_profile(const Profile* profile);
    [[nodiscard]] Profile* get_active_profile();
    [[nodiscard]] std::size_t profile_count() const;
    void set_follow_camera(bool enabled);
    [[nodiscard]] bool get_follow_camera() const;
    void set_actor_guid(const std::string& actor_guid);
    [[nodiscard]] std::string get_actor_guid() const;
    // Relative sources require an absolute scene/project source_base_dir.
    // Clear and rebind (or reload the scene) to refresh changed directory links.

    [[nodiscard]] EntityId get_handle() const;

   private:
    friend class Scene;

    EntityId handle_{};
    std::unordered_map<EntityId, Profile> profiles_;
    std::unordered_map<EntityId, EntityId> profile_storage_handles_;
    EntityId active_profile_handle_{0};
    EntityId next_profile_handle_{1};
};

// ============================================================================
// Camera: 相机类（合并了原 Viewport 的功能）
// ============================================================================

/// Everything one viewport edit can change. `open` is optional on purpose: the two callers differ
/// only in whether the edit also drives the window lifecycle. Omitting it changes the rectangle and
/// speed and leaves the open state alone (this was set_view_settings); providing it opens or closes
/// the view and carries the rectangle and speed with it (set_view_state).
///
/// set_view_state / set_view_settings / set_viewport_rect were three names for those two
/// behaviours, and set_viewport_rect was an unimplemented stub that silently did nothing.
struct CameraViewState {
    std::optional<bool> open;
    int x{120};
    int y{120};
    int width{960};
    int height{540};
    float move_speed{1.0f};
};

/// One camera pose read: every pose field from a single snapshot. The four point getters each
/// rebuilt the snapshot+pending view, and several callers read all of them in one expression.
struct CameraPose {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
    float fov{45.0f};
};

class Camera {
   public:
    Camera();
    Camera(const std::array<float, 3>& position, const std::array<float, 3>& forward,
           const std::array<float, 3>& world_up, float fov);
    ~Camera();

    void set(const std::array<float, 3>& position, const std::array<float, 3>& forward,
             const std::array<float, 3>& world_up, float fov);
    [[nodiscard]] EntityId get_handle() const;
    void set_surface(void* surface);
    [[nodiscard]] void* get_surface() const;
    void set_offscreen_capture_mode(bool enabled);
    void save_screenshot(const std::string& path) const;
    bool save_screenshot_sync(const std::string& path) const;

    void set_output_mode(const std::string& mode);
    [[nodiscard]] std::string get_output_mode() const;
    void set_render_backend(const std::string& mode);
    [[nodiscard]] std::string get_render_backend() const;
    void set_vision_render_mode(const std::string& mode);
    [[nodiscard]] std::string get_vision_render_mode() const;
    void set_vision_denoise(bool enabled);
    [[nodiscard]] bool get_vision_denoise() const;
    [[nodiscard]] bool get_requested_vision_denoise() const;
    void set_vision_denoiser(const std::string& type);
    [[nodiscard]] std::string get_vision_denoiser() const;
    [[nodiscard]] std::string get_requested_vision_denoiser() const;
    void set_vision_reblur_settings(const std::string& json);
    void set_vision_pt_settings(const std::string& json);
    void set_vision_restir_settings(const std::string& json);
    void set_vision_relax_settings(const std::string& json);
    [[nodiscard]] std::string get_requested_vision_reblur_settings() const;
    [[nodiscard]] std::string get_requested_vision_pt_settings() const;
    [[nodiscard]] std::string get_requested_vision_restir_settings() const;
    [[nodiscard]] std::string get_requested_vision_relax_settings() const;
    [[nodiscard]] std::string get_effective_vision_reblur_settings() const;
    [[nodiscard]] std::string get_effective_vision_pt_settings() const;
    [[nodiscard]] std::string get_effective_vision_restir_settings() const;
    [[nodiscard]] std::string get_effective_vision_relax_settings() const;
    void set_vision_accumulation(bool enabled);
    [[nodiscard]] bool get_vision_accumulation() const;
    [[nodiscard]] bool get_requested_vision_accumulation() const;
    void set_vision_stable_planes(bool enabled);
    [[nodiscard]] bool get_vision_stable_planes() const;
    [[nodiscard]] bool get_requested_vision_stable_planes() const;
    /// The single viewport entry point: one complete state, one command. See CameraViewState.
    void set_view(const CameraViewState& state);
    /// The single viewport read entry point. `open` is always present on the way out.
    [[nodiscard]] CameraViewState get_view() const;

    /// The single pose read entry point: one consistent snapshot of the pose.
    [[nodiscard]] CameraPose get_pose() const;
    void set_projection(float x_sign, float near_plane, float far_plane);
    // x sign, near clip, far clip.
    // Return the accepted settings even before the render thread consumes them.
    [[nodiscard]] std::array<float, 3> get_projection() const;

    // ========== 原 Viewport 功能 ==========
    void set_size(int width, int height);
    [[nodiscard]] std::array<int, 2> get_size() const;

   private:
    friend class Scene;

    EntityId handle_{};
    int width_{1920};
    int height_{1080};
    std::array<float, 3> projection_{1.0f, 0.1f, 100.0f};
};

// ============================================================================
// Environment: 环境类
// ============================================================================

/// One environment edit. Omitted fields are left untouched, so a single-field change reads as
/// set_properties({.sky_intensity = 20.0f}) and a batch as several designated initializers.
/// Replaces the eight set_sun_direction / set_sun_intensity / set_sky_intensity / set_floor_grid /
/// set_gravity / set_floor_y / set_floor_restitution / set_fixed_dt calls that had to be issued
/// one field at a time.
struct EnvironmentProperties {
    std::optional<std::array<float, 3>> sun_direction;
    std::optional<float> sun_intensity;
    std::optional<float> sky_intensity;
    std::optional<bool> floor_grid;
    std::optional<std::array<float, 3>> gravity;
    std::optional<float> floor_y;
    std::optional<float> floor_restitution;
    std::optional<float> fixed_dt;
};

/// One environment read: every field from a single snapshot. The eight point getters each rebuilt
/// the snapshot+pending view, so a caller reading several fields could straddle a pending command.
struct EnvironmentState {
    std::array<float, 3> sun_direction{0.0f, 1.0f, 0.0f};
    float sun_intensity{10.0f};
    float sky_intensity{20.0f};
    bool floor_grid{true};
    std::array<float, 3> gravity{0.0f, -9.8f, 0.0f};
    float floor_y{0.0f};
    float floor_restitution{0.6f};
    float fixed_dt{1.0f / 60.0f};
};

class Environment {
   public:
    Environment();
    ~Environment();
    void set_authored_lighting(const std::string& description);

    /// The single environment write entry point: one command carrying every field given.
    void set_properties(const EnvironmentProperties& properties);
    /// The single environment read entry point: one consistent snapshot of every field.
    [[nodiscard]] EnvironmentState get_state() const;

   private:
    friend class Scene;

    [[nodiscard]] EntityId get_handle() const;

    EntityId handle_{};
};

// ============================================================================
// Scene: 场景类（OOP 设计）
// ============================================================================
class Scene {
   public:
    Scene();
    ~Scene();

    // ========== Environment 管理 ==========
    void set_environment(Environment* env);
    [[nodiscard]] Environment* get_environment();
    [[nodiscard]] bool has_environment() const;
    void remove_environment();

    // ========== Actor 管理 ==========
    void add_actor(Actor* actor);
    void remove_actor(Actor* actor);
    void clear_actors();

    [[nodiscard]] std::size_t actor_count() const;
    [[nodiscard]] bool has_actor(const Actor* actor) const;

    // ========== Camera 管理 ==========
    void add_camera(Camera* camera);
    void remove_camera(Camera* camera);
    void clear_cameras();
    void set_active_camera(Camera* camera);
    [[nodiscard]] EntityId get_active_camera_handle() const;

    [[nodiscard]] std::size_t camera_count() const;
    [[nodiscard]] bool has_camera(const Camera* camera) const;

    /// 获取场景世界 AABB，返回 {min_x, min_y, min_z, max_x, max_y, max_z}
    [[nodiscard]] std::array<float, 6> get_aabb() const;

    // ========== 场景启用/禁用 ==========
    /// 启用或禁用场景（禁用后跳过渲染与物理模拟）
    void set_enabled(bool enabled);
    [[nodiscard]] bool is_enabled() const;

    // ========== 物理模拟开关 ==========
    /// 启用或禁用该场景的物理模拟（不影响渲染）
    void set_simulation_enabled(bool enabled);
    [[nodiscard]] bool is_simulation_enabled() const;

   private:
    EntityId handle_{};

    Environment* environment_{nullptr};
    std::vector<Actor*> actors_;
    std::vector<Camera*> cameras_;
    std::unordered_set<const Actor*> actors_index_;
    std::unordered_set<const Camera*> cameras_index_;
    // 本对象的"读己之写"镜像：enabled/simulation_enabled 现在以命令形式异步
    // 落地到 SceneDevice，getter 若继续读槽位会看到上一 tick 的旧值。
    // 这两个字段只由本对象的 setter 写，DAG 侧不写它们，因此镜像即权威。
    bool enabled_{true};
    bool simulation_enabled_{false};
};

// ============================================================================
// Render backend control (Native vs Vision)
// ============================================================================
/// 兼容接口：引擎始终包含 Vision，返回 true；不代表 Vulkan 设备已经初始化成功。
[[nodiscard]] bool is_vision_available();

/// 请求切换光学渲染后端。mode: "native" 或 "vision"。
/// Vision 设备在需要渲染时延迟初始化。
void set_render_backend(const std::string& mode, EntityId camera_handle = 0);

/// 获取当前请求的渲染后端，返回 "native" 或 "vision"。
[[nodiscard]] std::string get_render_backend(EntityId camera_handle = 0);

/// Set the Vision algorithm without changing independent denoise/accumulation preferences.
/// Legacy "svgf" selects "path_tracing" and enables denoise; "ssat" remains supported.
/// Legacy "progressive_path_tracing" selects "path_tracing" and enables accumulation.
void set_vision_render_mode(const std::string& mode, EntityId camera_handle = 0);

/// 获取当前 camera 请求的 Vision 渲染技术。
[[nodiscard]] std::string get_vision_render_mode(EntityId camera_handle = 0);

/// Queue an independent SVGF preference for this camera (disabled by default).
void set_vision_denoise(bool enabled, EntityId camera_handle = 0);

/// Return the camera's committed SVGF preference.
[[nodiscard]] bool get_vision_denoise(EntityId camera_handle = 0);

/// Return the latest uncommitted preference, or committed state when none is pending.
/// Persistence uses this to preserve setter requests even while a render update is in flight.
[[nodiscard]] bool get_requested_vision_denoise(EntityId camera_handle = 0);

/// Select "scene", "svgf", "reblur" or "relax", independently of the enable switch.
void set_vision_denoiser(const std::string& type, EntityId camera_handle = 0);
[[nodiscard]] std::string get_vision_denoiser(EntityId camera_handle = 0);
[[nodiscard]] std::string get_requested_vision_denoiser(EntityId camera_handle = 0);
enum class VisionSettingsFamily { Pt, Restir, Reblur, Relax };
void set_vision_settings(VisionSettingsFamily family, const nlohmann::json& settings, EntityId camera_handle);
[[nodiscard]] nlohmann::json get_requested_vision_settings(VisionSettingsFamily family, EntityId camera_handle);
[[nodiscard]] nlohmann::json get_effective_vision_settings(VisionSettingsFamily family, EntityId camera_handle);
// String adapters for independent legacy callers. Native communication uses values.
void set_vision_reblur_settings(const std::string& json, EntityId camera_handle = 0);
void set_vision_pt_settings(const std::string& json, EntityId camera_handle = 0);
void set_vision_restir_settings(const std::string& json, EntityId camera_handle = 0);
void set_vision_relax_settings(const std::string& json, EntityId camera_handle = 0);
[[nodiscard]] std::string get_requested_vision_reblur_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_requested_vision_pt_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_requested_vision_restir_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_requested_vision_relax_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_effective_vision_reblur_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_effective_vision_pt_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_effective_vision_restir_settings(EntityId camera_handle = 0);
[[nodiscard]] std::string get_effective_vision_relax_settings(EntityId camera_handle = 0);

/// Queue an independent accumulation preference for this camera (disabled by default).
void set_vision_accumulation(bool enabled, EntityId camera_handle = 0);

/// Return the camera's committed accumulation preference.
[[nodiscard]] bool get_vision_accumulation(EntityId camera_handle = 0);

/// Return the latest uncommitted preference, or committed state when none is pending.
/// Persistence uses this while a render update is in flight.
[[nodiscard]] bool get_requested_vision_accumulation(EntityId camera_handle = 0);

/// Queue an independent stable-plane preference for this camera (enabled by default).
void set_vision_stable_planes(bool enabled, EntityId camera_handle = 0);

/// Return the camera's committed stable-plane preference.
[[nodiscard]] bool get_vision_stable_planes(EntityId camera_handle = 0);

/// Return the latest uncommitted preference, or committed state when none is pending.
/// Persistence uses this while a render update is in flight.
[[nodiscard]] bool get_requested_vision_stable_planes(EntityId camera_handle = 0);

// ============================================================================
// Media (video/audio) import
//
// Audio/video files are standalone resources, NOT 3D actors. import_media
// loads the file through the resource manager and returns its id + metadata
// without going through Geometry/Scene.
// ============================================================================
struct MediaInfo {
    std::uint64_t resource_id{0};  ///< 资源 ID（0 表示导入失败）
    std::string media_type;        ///< "video" / "audio" / ""（失败）
    double duration_seconds{0.0};  ///< 时长（秒）
    std::string codec;             ///< 编码名

    // 视频字段
    int width{0};
    int height{0};
    double fps{0.0};

    // 音频字段
    int sample_rate{0};
    int channels{0};
};

/// 导入音频或视频文件，返回资源信息。失败时 media_type 为空、resource_id 为 0。
[[nodiscard]] MediaInfo import_media(const std::string& path);

// ============================================================================
// Audio playback (global, not spatialized)
// ============================================================================

/// 播放已导入的音频资源。resource_id 来自 import_media 返回的 MediaInfo。
/// 通过 EventBus 通知 AcousticsSystem，在独立线程上播放。
/// @param resource_id 音频资源 ID（由 import_media 返回）
/// @param loop 是否循环播放（默认 false）
void play_audio(std::uint64_t resource_id, bool loop = false);

/// 停止播放指定资源。
/// @param resource_id 音频资源 ID
void stop_audio(std::uint64_t resource_id);

}  // namespace API
}  // namespace Corona
