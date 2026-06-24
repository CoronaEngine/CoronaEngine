#pragma once
#include <corona/kernel/utils/storage.h>
#include <ktm/ktm.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

#include "horizon.h"

// Forward declarations

namespace Corona {

class Model;

struct MeshDevice {
    Horizon::HardwareBuffer indexBuffer;
    Horizon::HardwareBuffer vertexBuffer;

    // StorageBuffer mirrors for compute shader (VBuffer resolve) access.
    // Horizon BufferUsage is non-combinable, so we keep separate copies.
    Horizon::HardwareBuffer indexStorageBuffer;
    Horizon::HardwareBuffer vertexStorageBuffer;

    uint32_t materialIndex;
    Horizon::HardwareImage textureBuffer;

    // 材质颜色 (RGBA)
    std::array<float, 4> materialColor{1.0f, 1.0f, 1.0f, 1.0f};
};

struct ModelTransform {
    ktm::fvec3 position;
    ktm::fvec3 euler_rotation;
    ktm::fvec3 scale;

    ModelTransform() {
        position.x = 0.0f;
        position.y = 0.0f;
        position.z = 0.0f;

        euler_rotation.x = 0.0f;
        euler_rotation.y = 0.0f;
        euler_rotation.z = 0.0f;

        scale.x = 1.0f;
        scale.y = 1.0f;
        scale.z = 1.0f;
    }

    // Definition lives in shared_data_hub.cpp to avoid instantiating
    // ktm::affine3d::rotate in translation units that also pull in
    // ocarina/vision headers (which define a global operator* on iterable
    // types and would cause ambiguous-overload errors against ktm).
    [[nodiscard]] ktm::fmat4x4 compute_matrix() const;
};

struct ModelResource {
    std::uint64_t model_id;
};

struct GeometryDevice {
    std::uintptr_t transform_handle{};
    std::uintptr_t model_resource_handle{};
    std::vector<MeshDevice> mesh_handles;
    ktm::fvec3 native_local_correction_offset{0.0f, 0.0f, 0.0f};
    float native_local_correction_scale{1.0f};
};

struct MechanicsDevice {
    std::uintptr_t geometry_handle{};
    ktm::fvec3 max_xyz;
    ktm::fvec3 min_xyz;

    // 物体级物理参数
    float mass{1.0f};
    float restitution{0.8f};
    float damping{0.99f};

    // 物理开关：false 时物理系统跳过该对象（不参与模拟，但仍保留数据）
    bool physics_enabled{false};

    // 力学碰撞检测开关：false 时完全禁用该物体的碰撞检测（物体不与其他物体或地面碰撞）
    bool bEnableCollision{false};

    // 轴锁定位掩码：bit0=锁定X轴, bit1=锁定Y轴, bit2=锁定Z轴
    uint8_t linear_lock_mask{0};   // 锁定线性运动（平移）的轴
    uint8_t angular_lock_mask{0};  // 锁定角度运动（旋转）的轴

    // 碰撞回调函数
    std::function<void(std::uintptr_t, bool, const std::array<float, 3>&, const std::array<float, 3>&)> collision_callback;

    // 移动回调函数
    std::function<void()> on_move_callback;
};

struct AcousticsDevice {
    std::uintptr_t geometry_handle{};
    float volume{1.0f};
    bool audio_enabled{true};
};

struct OpticsDevice {
    std::uintptr_t geometry_handle{};

    bool visible{true};  // 控制模型是否参与渲染

    // 光照影响开关：false 时物体不受灯光照射影响（仍参与渲染但不接收光照计算）
    bool bEnableLighting{true};

    // Disney Principled BRDF parameters
    float metallic{0.0f};
    float roughness{0.5f};
    float subsurface{0.0f};
    float specular{0.5f};
    float specularTint{0.0f};
    float anisotropic{0.0f};
    float sheen{0.0f};
    float sheenTint{0.5f};
    float clearcoat{0.0f};
    float clearcoatGloss{1.0f};

    // Legacy parameters (kept for backward compatibility)
    ktm::fvec3 ambient{0.2f, 0.2f, 0.2f};
    ktm::fvec3 diffuse{0.8f, 0.8f, 0.8f};
    ktm::fvec3 specular_color{1.0f, 1.0f, 1.0f};
    float shininess{32.0f};
};

struct ProfileDevice {
    std::uintptr_t optics_handle{};
    std::uintptr_t acoustics_handle{};
    std::uintptr_t mechanics_handle{};
    std::uintptr_t geometry_handle{};
};

struct ExternalVisionBindingDevice {
    bool enabled{false};
    std::string source_path;
    std::string shape_guid;
    int shape_index{-1};
    std::string json_path;
    std::string shape_type;
    std::string shape_identity_key;
    std::string model_path;
};

struct ActorDevice {
    std::vector<std::uintptr_t> profile_handles;
    std::filesystem::path model_path;  //Actor文件路径，同时作为Actor的唯一标识
    bool follow_camera{false};         // true: render in Optics pass 2 using camera-local orthographic space
};

enum class CameraOutputMode : uint8_t {
    FinalColor,
    BaseColor,
    Normal,
    WorldPosition,
    ObjectID,
    VisibilityBuffer,
};

enum class CameraRenderBackend : uint8_t {
    Native,
    Vision,
};

enum class CameraVisionRenderMode : uint8_t {
    PathTracing,
    SVGF,
    SSAT,
};

struct CameraDevice {
    void* surface{};
    bool follows_default_surface{true};

    ktm::fvec3 position;
    ktm::fvec3 forward;
    ktm::fvec3 world_up;
    float fov{60.0f};
    float aspect{16.0f / 9.0f};
    float near_plane{0.1f};
    float far_plane{100.0f};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    CameraOutputMode output_mode{CameraOutputMode::FinalColor};
    CameraRenderBackend render_backend{CameraRenderBackend::Native};
    CameraVisionRenderMode vision_render_mode{CameraVisionRenderMode::PathTracing};
    bool view_open{false};
    bool viewport_rect_active{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    std::uintptr_t actor_pick_handle{};

    CameraDevice() {
        position.x = 0.0f;
        position.y = 0.0f;
        position.z = -5.0f;

        forward.x = 0.0f;
        forward.y = 0.0f;
        forward.z = 1.0f;

        world_up.x = 0.0f;
        world_up.y = 1.0f;
        world_up.z = 0.0f;
    }

    [[nodiscard]] ktm::fmat4x4 compute_view_matrix() const {
        ktm::fvec3 normalized_forward = ktm::normalize(forward);
        return ktm::look_to_lh(position, normalized_forward, world_up);
    }

    [[nodiscard]] ktm::fmat4x4 compute_projection_matrix() const {
        ktm::fmat4x4 proj = ktm::perspective_lh(ktm::radians(fov), aspect, near_plane, far_plane);
        // Vulkan NDC Y轴向下（与OpenGL相反），需要翻转Y分量
        proj[1][1] *= -1.0f;
        return proj;
    }

    [[nodiscard]] ktm::fmat4x4 compute_view_proj_matrix() const {
        return compute_projection_matrix() * compute_view_matrix();
    }
};

struct ActorPickDevice {
    std::string request_id;
    std::string result_request_id;
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t result_x{0};
    std::uint32_t result_y{0};
    std::uintptr_t actor_handle{0};
    bool pending{false};
    bool result_ready{false};
};

struct CameraMoveCommand {
    std::uintptr_t camera_handle{};
    ktm::fvec3 position{};
    ktm::fvec3 forward{};
    ktm::fvec3 world_up{};
    float fov{45.0f};
    std::uint64_t sequence{};
};

struct CameraViewportUpdateCommand {
    std::uintptr_t camera_handle{};
    void* surface{};
    bool view_open{false};
    int x{120};
    int y{120};
    int width{960};
    int height{540};
    int render_width{960};
    int render_height{540};
    std::uint64_t sequence{};
};

enum class CameraStateUpdateField : std::uint32_t {
    None = 0,
    Surface = 1u << 0,
    Size = 1u << 1,
    OutputMode = 1u << 2,
    RenderBackend = 1u << 3,
    ViewState = 1u << 4,
    VisionRenderMode = 1u << 5,
};

constexpr CameraStateUpdateField operator|(CameraStateUpdateField lhs,
                                           CameraStateUpdateField rhs) {
    return static_cast<CameraStateUpdateField>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr bool has_camera_state_field(CameraStateUpdateField fields,
                                      CameraStateUpdateField field) {
    return (static_cast<std::uint32_t>(fields) &
            static_cast<std::uint32_t>(field)) != 0;
}

struct CameraStateUpdateCommand {
    std::uintptr_t camera_handle{};
    CameraStateUpdateField fields{CameraStateUpdateField::None};
    void* surface{};
    std::uint32_t width{1};
    std::uint32_t height{1};
    CameraOutputMode output_mode{CameraOutputMode::FinalColor};
    CameraRenderBackend render_backend{CameraRenderBackend::Native};
    CameraVisionRenderMode vision_render_mode{CameraVisionRenderMode::PathTracing};
    bool view_open{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    std::uint64_t sequence{};
};

struct CameraReleaseCommand {
    std::uintptr_t camera_handle{};
    std::uintptr_t actor_pick_handle{};
};

enum class ViewportUiMode : std::uint8_t {
    Flat2D,
    Stereo3D,
};

enum class ViewportUiCursorShape : std::uint8_t {
    Arrow,
    Hand,
    Crosshair,
    Grab,
    Grabbing,
    Hidden,
};

struct ViewportUiCalibration {
    float lenticular_pitch{19.1849f};
    float slant_angle_radians{0.2333f};
    float phase_offset{10.0f};
    std::array<float, 3> rgb_subpixel_offsets{0.0f, 1.0f / 3.0f, 2.0f / 3.0f};
    std::uint32_t display_width{1920};
    std::uint32_t display_height{1080};
    float parallax_scale{19.1849f};
};

struct ViewportUiState {
    std::uintptr_t camera_handle{};
    ViewportUiMode mode{ViewportUiMode::Flat2D};
    ViewportUiCursorShape cursor_shape{ViewportUiCursorShape::Arrow};
    ViewportUiCalibration calibration{};
};

struct ViewportUiPointerCommand {
    std::uintptr_t camera_handle{};
    std::string event_type;
    float x{0.0f};
    float y{0.0f};
    std::uint32_t buttons{0};
    std::uint32_t modifiers{0};
    ViewportUiCursorShape cursor_shape{ViewportUiCursorShape::Arrow};
    std::uint64_t sequence{};
};

struct EnvironmentDevice {
    ktm::fvec3 sun_position;
    std::uint32_t floor_grid_enabled{1};

    // 统一光照参数（供 OpticsSystem lighting/sky/tonemap 共用）
    ktm::fvec3 sun_color{1.0f, 0.949f, 0.853f};  // ~5500K 日光色温
    float sun_intensity{10.0f};                  // 太阳直射辐照度
    float sky_intensity{20.0f};                  // 大气散射功率
    float exposure{1.0f};                        // 全局曝光

    // 物理场景参数
    ktm::fvec3 gravity{0.0f, -9.8f, 0.0f};
    float floor_y{0.0f};
    float floor_restitution{0.6f};
    float fixed_dt{1.0f / 60.0f};
};

struct SceneDevice {
    bool enabled{true};
    bool simulation_enabled{false};
    std::uintptr_t environment{};
    std::vector<std::uintptr_t> actor_handles;
    std::vector<std::uintptr_t> camera_handles;
    std::uintptr_t active_camera_handle{};
    ktm::fvec3 min_world;
    ktm::fvec3 max_world;
    ktm::fvec3 center_world;
};

struct ImageDevice {
    Horizon::HardwareImage image;
    Horizon::SubmitReceipt submit_receipt;

    /// Written by DisplaySystem after compositing finishes reading the image.
    /// Producers wait on this before overwriting with new content to prevent GPU read/write races.
    Horizon::SubmitReceipt consumed_receipt;
};

class SharedDataHub {
   public:
    static SharedDataHub& instance();

    ~SharedDataHub() = default;

    SharedDataHub(const SharedDataHub&) = delete;
    SharedDataHub& operator=(const SharedDataHub&) = delete;
    SharedDataHub(SharedDataHub&&) = delete;
    SharedDataHub& operator=(SharedDataHub&&) = delete;

   private:
    SharedDataHub() = default;

   public:
    // 新的 Storage 类型定义，包含默认的容量和内存池参数
    using ModelResourceStorage = Kernel::Utils::Storage<ModelResource, 128, 2>;
    using ModelTransformStorage = Kernel::Utils::Storage<ModelTransform, 128, 2>;
    using GeometryStorage = Kernel::Utils::Storage<GeometryDevice, 128, 2>;
    using MechanicsStorage = Kernel::Utils::Storage<MechanicsDevice, 128, 2>;
    using AcousticsStorage = Kernel::Utils::Storage<AcousticsDevice, 128, 2>;
    using OpticsStorage = Kernel::Utils::Storage<OpticsDevice, 128, 2>;
    using ProfileStorage = Kernel::Utils::Storage<ProfileDevice, 128, 2>;
    using ActorStorage = Kernel::Utils::Storage<ActorDevice, 128, 2>;
    using CameraStorage = Kernel::Utils::Storage<CameraDevice, 128, 2>;
    using ActorPickStorage = Kernel::Utils::Storage<ActorPickDevice, 128, 2>;
    using EnvironmentStorage = Kernel::Utils::Storage<EnvironmentDevice, 128, 2>;
    using SceneStorage = Kernel::Utils::Storage<SceneDevice, 128, 2>;
    using ImageStorage = Kernel::Utils::Storage<ImageDevice, 128, 2>;

    ModelResourceStorage& model_resource_storage();
    const ModelResourceStorage& model_resource_storage() const;

    ModelTransformStorage& model_transform_storage();
    const ModelTransformStorage& model_transform_storage() const;

    GeometryStorage& geometry_storage();
    const GeometryStorage& geometry_storage() const;

    MechanicsStorage& mechanics_storage();
    const MechanicsStorage& mechanics_storage() const;

    AcousticsStorage& acoustics_storage();
    const AcousticsStorage& acoustics_storage() const;

    OpticsStorage& optics_storage();
    const OpticsStorage& optics_storage() const;

    ProfileStorage& profile_storage();
    const ProfileStorage& profile_storage() const;

    ActorStorage& actor_storage();
    const ActorStorage& actor_storage() const;

    // Runtime-only editor metadata keyed by native actor handle. Persisted state
    // remains in .scene; this cache only bridges Python proxy actors to native
    // systems such as OpticsSystem/ExternalVisionSceneAdapter.
    void set_actor_guid(std::uintptr_t actor_handle, std::string actor_guid);
    [[nodiscard]] std::string actor_guid(std::uintptr_t actor_handle) const;
    void set_external_vision_binding(std::uintptr_t actor_handle, ExternalVisionBindingDevice binding);
    void clear_external_vision_binding(std::uintptr_t actor_handle);
    [[nodiscard]] std::optional<ExternalVisionBindingDevice> external_vision_binding(
        std::uintptr_t actor_handle) const;
    [[nodiscard]] bool has_external_vision_binding(std::uintptr_t actor_handle) const;
    void clear_actor_metadata(std::uintptr_t actor_handle);

    CameraStorage& camera_storage();
    const CameraStorage& camera_storage() const;

    ActorPickStorage& actor_pick_storage();
    const ActorPickStorage& actor_pick_storage() const;

    EnvironmentStorage& environment_storage();
    const EnvironmentStorage& environment_storage() const;

    SceneStorage& scene_storage();
    const SceneStorage& scene_storage() const;

    ImageStorage& image_storage();
    const ImageStorage& image_storage() const;

    void enqueue_camera_move(CameraMoveCommand command);
    std::vector<CameraMoveCommand> drain_camera_moves();
    void enqueue_camera_viewport_update(CameraViewportUpdateCommand command);
    std::vector<CameraViewportUpdateCommand> drain_camera_viewport_updates();
    void enqueue_camera_state_update(CameraStateUpdateCommand command);
    std::vector<CameraStateUpdateCommand> drain_camera_state_updates();
    void enqueue_camera_release(CameraReleaseCommand command);
    std::vector<CameraReleaseCommand> drain_camera_releases();
    void set_viewport_ui_mode(std::uintptr_t camera_handle, ViewportUiMode mode);
    void set_viewport_ui_calibration(std::uintptr_t camera_handle,
                                     const ViewportUiCalibration& calibration);
    [[nodiscard]] ViewportUiState viewport_ui_state(std::uintptr_t camera_handle) const;
    void enqueue_viewport_ui_pointer(ViewportUiPointerCommand command);
    std::vector<ViewportUiPointerCommand> drain_viewport_ui_pointer_commands();

   private:
    ModelResourceStorage model_resource_storage_;
    GeometryStorage geometry_storage_;
    ModelTransformStorage model_transform_storage_;
    MechanicsStorage mechanics_storage_;
    OpticsStorage optics_storage_;
    AcousticsStorage acoustics_storage_;
    ProfileStorage profile_storage_;
    ActorStorage actor_storage_;
    mutable std::mutex actor_metadata_mutex_;
    std::unordered_map<std::uintptr_t, std::string> actor_guids_;
    std::unordered_map<std::uintptr_t, ExternalVisionBindingDevice> external_vision_bindings_;
    EnvironmentStorage environment_storage_;
    CameraStorage camera_storage_;
    ActorPickStorage actor_pick_storage_;
    SceneStorage scene_storage_;
    ImageStorage image_storage_;
    std::mutex camera_move_mutex_;
    std::unordered_map<std::uintptr_t, CameraMoveCommand> pending_camera_moves_;
    std::uint64_t camera_move_sequence_{0};
    std::mutex camera_viewport_update_mutex_;
    std::unordered_map<std::uintptr_t, CameraViewportUpdateCommand>
        pending_camera_viewport_updates_;
    std::uint64_t camera_viewport_update_sequence_{0};
    std::mutex camera_state_update_mutex_;
    std::unordered_map<std::uintptr_t, CameraStateUpdateCommand>
        pending_camera_state_updates_;
    std::uint64_t camera_state_update_sequence_{0};
    std::mutex camera_release_mutex_;
    std::vector<CameraReleaseCommand> pending_camera_releases_;
    mutable std::mutex viewport_ui_mutex_;
    std::unordered_map<std::uintptr_t, ViewportUiState> viewport_ui_states_;
    std::vector<ViewportUiPointerCommand> pending_viewport_ui_pointer_commands_;
    std::uint64_t viewport_ui_pointer_sequence_{0};
};

}  // namespace Corona
