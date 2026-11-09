#pragma once
#include <horizon/core/storage.h>
#include <nlohmann/json.hpp>
#include <corona/camera_command_authority.h>
#include <corona/kernel/entity_id.h>
#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/memory/gpu_mem_ledger.h>
#include <corona/resource/types/scene.h>  // Resource::IkChain（GeometryDevice::ik_chains）
#include <corona/resource/model_import_options.h>
#include <corona/resource/types/animation_player.h>
#include <corona/systems/display/image_frame_metadata.h>
#include <corona/systems/optics/viewport_gizmo_math.h>
#include <corona/systems/optics/material_source.h>

#include <ktm/ktm.h>
#include <corona/systems/geometry/model_node_state.h>
#include <corona/systems/geometry/mesh_draw_bounds.h>

#include <array>
#include <atomic>
#include <chrono>
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

struct MeshMaterialDevice {
    enum Slot : std::size_t { BaseColor, Normal, Metallic, Roughness, Occlusion,
                             Emissive, Specular, SpecularColor, Count };
    Resource::MaterialData source;
    std::array<Horizon::HardwareImage, Count> textures;
    std::array<std::shared_ptr<Memory::GpuMemToken>, Count> memory;
};

struct MeshDevice {
    Geometry::MeshDrawClusters draw_clusters;
    Horizon::HardwareBuffer shadowIndexBuffer;
    Horizon::HardwareBuffer indexBuffer;
    Horizon::HardwareBuffer vertexBuffer;

    // Rasterization and VBuffer resolve share the same backing allocation.
    Horizon::HardwareBuffer indexStorageBuffer;
    Horizon::HardwareBuffer vertexStorageBuffer;

    uint32_t materialIndex;
    Horizon::HardwareImage textureBuffer;
    std::shared_ptr<const MeshMaterialDevice> material;

    // 材质颜色 (RGBA)
    std::array<float, 4> materialColor{1.0f, 1.0f, 1.0f, 1.0f};

    // CPU/import-side bounds for GPU visibility/deferred decode diagnostics.
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    uint32_t max_index = 0;

    // ---- GPU 显存记账令牌（P0：mesh/texture 计量）----
    // mesh_mem 覆盖本 MeshDevice 的 4 个 GPU 缓冲（vertex/index/storage）；
    // tex_mem 覆盖 textureBuffer（仅真实纹理时非空；占位纹理为空）。
    // 纹理句柄可按材质/资源 ID 被多个 MeshDevice 共享，故纹理令牌用 shared_ptr：
    // 同一张 GPU 纹理只记账一次，最后一个引用释放时才扣减。
    Corona::Memory::GpuMemToken mesh_mem;
    std::shared_ptr<Corona::Memory::GpuMemToken> tex_mem;
};

/// 一个共享 MeshDevice 的场景实例。transform 为网格本地空间到 actor 本地空间
/// 的列主序矩阵；同一个 MeshDevice 可被多个实例引用。
struct MeshInstanceDevice {
    std::uint32_t mesh_index = 0;
    std::uint32_t object_id = 0;
    std::uint32_t node_index = UINT32_MAX;
    bool visible = true;
    std::array<float,3> local_bounds_min{}, local_bounds_max{};
    ktm::fmat4x4 original_transform{ktm::fmat4x4::from_eye()};
    ktm::fmat4x4 transform{ktm::fmat4x4::from_eye()};
    // Authored node world space -> the normalized space of baked/skinned vertices.
    ModelNodes::Matrix node_delta_prefix{ModelNodes::identity()};
};

struct ModelTransform {
    // Authored linear transform, composed after editable TRS. Translation lives
    // in position so streaming/picking retain the correct actor origin.
    ktm::fmat4x4 affine_basis{ktm::fmat4x4::from_eye()};
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
    Resource::ModelImportOptions import_options;
    // GPU 资源（mesh_handles）的生命周期状态。
    // - Ready：mesh_handles 已就绪（默认）。from_image、网络重建、距离重载等
    //   同步路径产出的都是 Ready，行为不变。
    // - PendingImport：仅记录了 model_path，磁盘资源尚未导入（model_id=0）。
    //   GeometrySystem 在 update() 中异步 import，完成后转 PendingBuild。
    // - PendingBuild：model_id 已就绪、mesh_handles 待 GeometrySystem 异步构建。
    // - Failed：import 失败（路径无效等），不再重试。
    // 注意：LRU evict / 距离卸载只清空 mesh_handles、不改此字段——其恢复走
    //   actor_load_states 状态机 + rebuild_actor_gpu_resources，与本字段无关。
    enum class GpuBuildState : std::uint8_t { Ready, PendingImport, PendingBuild, Failed };

    EntityId transform_handle{};
    EntityId model_resource_handle{};
    std::vector<MeshDevice> mesh_handles;
    std::vector<MeshInstanceDevice> mesh_instances;
    std::vector<ModelNodes::Node> model_nodes;
    std::vector<API::ModelNodeOverride> model_node_overrides;
    std::uint64_t model_node_revision = 0;
    ModelNodes::Matrix node_instance_prefix=ModelNodes::identity();
    bool has_node_instance_prefix=false;
    bool model_node_instances_mapped=false;
    std::optional<std::array<float,6>> model_node_bounds;
    ktm::fvec3 native_local_correction_offset{0.0f, 0.0f, 0.0f};
    float native_local_correction_scale{1.0f};
    GpuBuildState gpu_build_state{GpuBuildState::Ready};
    // 待异步导入的模型路径（UTF-8）。仅 PendingImport 阶段有意义；
    // GeometrySystem import 完成后将解析出的 model_id 写入 model_resource 槽。
    std::string model_path_utf8;
    // 异步 import 任务的 epoch（防 slot 复用的 ABA）。GeometrySystem 发起 import 时
    // 写入一个进程级单调递增值，并在 import 完成回写前比对：若不符说明本槽已被
    // deallocate→复用为新对象（allocate 时 T{} 会把本字段重置为 0），丢弃本次结果。
    // 0 = 无在途 import 任务。
    std::uint64_t import_epoch{0};

    // ---- 骨骼动画（per-instance，P2）----
    // is_skinned：对应 Scene 带骨架（model_id 解析的 Scene::data.skeleton 有值）。
    //   GeometrySystem 在 mesh 构建/上传时探测并置位；非蒙皮几何此字段恒 false，零开销。
    // Player owns per-instance clip selection, asynchronous sources and local-TRS transitions.
    // skinned_cpu_vertices：仅活跃 Vision 相机请求 GPU 蒙皮 LOD0 回读，每个 mesh 一份原始字节，
    //   布局严格等同 Resource::Vertex 数组（pos[3]+normal[3]+uv[2]，32B）。
    //   Vision 读取该副本；碰撞和光栅顶点缓冲直接使用 GPU 结果，
    //   不依赖该回读副本重新上传。光栅模式及非蒙皮几何此容器为空。
    bool is_skinned{false};
    std::uint64_t skin_pose_revision = 0;
    std::shared_ptr<Resource::AnimationPlayer> animation_player{std::make_shared<Resource::AnimationPlayer>()};
    std::vector<std::vector<std::byte>> skinned_cpu_vertices;

    // 蒙皮动态 local AABB：每帧合并 GPU 蒙皮包围盒；混合静态子网格时根据完整
    // 碰撞网格补全包围盒（模型空间，已含首帧归一化）。
    // 物理系统每帧读取它覆盖静态 min_xyz/max_xyz，使碰撞 AABB 跟随当前动画姿态。
    // skinned_aabb_valid=false 表示尚未蒙皮过（首帧前），物理回退静态 AABB。
    bool skinned_aabb_valid{false};
    // Published by Mechanics only after the exact triangle data has been
    // prepared. Consumers compare against the currently loaded model identity.
    std::uint64_t collision_model_id{};
    ktm::fvec3 skinned_aabb_min{0.0f, 0.0f, 0.0f};
    ktm::fvec3 skinned_aabb_max{0.0f, 0.0f, 0.0f};

    // ---- IK（CCD）----
    // 每帧采样动画 local，更新自动目标，然后按顺序求解启用的链。
    // 最终 local 只做一次 FK 和蒙皮；配置与自动驱动的 runtime 独立保存。
    std::vector<Resource::IkChain> ik_chains;
    // Increment on configuration replacement; mechanics discards stale snapshot results.
    std::uint64_t ik_chains_revision{0};

    // 骨骼名 → SkeletonData::nodes 下标缓存。
    // 首次求值及模型替换时重建，用于解析持久化链的骨骼名称。
    std::unordered_map<std::string, int> bone_name_to_node_idx;
    std::uint64_t skeleton_cache_model_id{0};

    // 叶子骨骼名列表（BoneNode::children 为空的节点）。构建 bone_name_to_node_idx
    // 时同步填充。编辑器也可直接通过资源骨架查询，不依赖首帧动画。
    std::vector<std::string> leaf_bone_names;
};

// 物体参与物理模拟的方式。
// Dynamic  ：受重力和冲量影响，与其他物体碰撞并弹开（原 physics_enabled=true）。
// Kinematic：外部驱动（脚本/动画），不受重力和碰撞冲量影响，但推开 Dynamic 物体；触发碰撞回调。
// Static   ：完全静止（地形/场景固定件），不受任何力影响；触发碰撞回调。
// Phantom  ：完全不参与碰撞检测，不受任何力；适用于纯视觉/音频物体（原 collision_enabled=false）。
enum class BodyType : std::uint8_t {
    Dynamic,
    Kinematic,
    Static,
    Phantom,
};

inline std::string_view body_type_to_string(BodyType bt) {
    switch (bt) {
        case BodyType::Dynamic:   return "dynamic";
        case BodyType::Kinematic: return "kinematic";
        case BodyType::Static:    return "static";
        case BodyType::Phantom:   return "phantom";
    }
    return "dynamic";
}

inline BodyType body_type_from_string(std::string_view s) {
    if (s == "kinematic") return BodyType::Kinematic;
    if (s == "static")    return BodyType::Static;
    if (s == "phantom")   return BodyType::Phantom;
    return BodyType::Dynamic;
}

struct MechanicsDevice {
    EntityId geometry_handle{};
    ktm::fvec3 max_xyz;
    ktm::fvec3 min_xyz;

    // 物体级物理参数
    float mass{1.0f};
    float restitution{0.8f};
    float damping{0.99f};

    // 物体参与物理的方式（取代旧 physics_enabled + collision_shape 双字段）
    BodyType body_type{BodyType::Dynamic};

    // 轴锁定位掩码：bit0=锁定X轴, bit1=锁定Y轴, bit2=锁定Z轴
    uint8_t linear_lock_mask{0};   // 锁定线性运动（平移）的轴
    uint8_t angular_lock_mask{0};  // 锁定角度运动（旋转）的轴

    // 碰撞回调函数
    std::function<void(EntityId, bool, const std::array<float, 3>&, const std::array<float, 3>&)> collision_callback;

    // 移动回调函数
    std::function<void()> on_move_callback;
};

struct AcousticsDevice {
    EntityId geometry_handle{};
    std::uint64_t resource_id{0};  // 绑定的音频资源（空间音频物体）；0 表示未绑定
    float volume{1.0f};
    bool audio_enabled{true};
};

struct OpticsDevice {
    EntityId geometry_handle{};
    std::shared_ptr<const Materials::Overrides> material_overrides;
    std::shared_ptr<const Materials::Principled> principled;

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
    EntityId optics_handle{};
    EntityId acoustics_handle{};
    EntityId mechanics_handle{};
    EntityId geometry_handle{};
};



struct ActorDevice {
    std::vector<EntityId> profile_handles;
    std::filesystem::path model_path;
    Resource::ModelImportOptions model_import_options;
    bool follow_camera{false};
    bool pinned{false};  // 标记后不会被距离剔除卸载，也不会被不可见帧淘汰
};

enum class CameraOutputMode : uint8_t {
    FinalColor,
    BaseColor,
    Normal,
    WorldPosition,
    ObjectID,
    VisibilityBuffer,
    SSAO,
    SSAORaw,
    ShadowMaskRaw,
    ShadowMask,
};

enum class CameraRenderBackend : uint8_t {
    Native,
    Vision,
};

enum class CameraVisionRenderMode : uint8_t {
    PathTracing,
    SVGF,
    SSAT,
    ProgressivePathTracing,
    ReSTIR,
};

enum class CameraVisionDenoiser : uint8_t { Scene, SVGF, ReBLUR, ReLAX };

// Runtime telemetry only; never serialized into scene settings.
struct CameraFrameTiming {
    double render_ms{};
    double denoise_ms{};
    // Wall-clock cadence between completed frames of this camera, including waits.
    double frame_ms{};
    CameraVisionRenderMode mode{CameraVisionRenderMode::PathTracing};
    std::chrono::steady_clock::time_point recorded_at{};
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
    // Preserve the image-right axis of imported cameras, including reflected bases.
    float projection_x_sign{1.0f};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    CameraOutputMode output_mode{CameraOutputMode::FinalColor};
    CameraRenderBackend render_backend{CameraRenderBackend::Native};
    CameraVisionRenderMode vision_render_mode{CameraVisionRenderMode::PathTracing};
    // PT produces one sample per pixel per frame; REBLUR owns temporal history.
    bool vision_denoise{true};
    CameraVisionDenoiser vision_denoiser{CameraVisionDenoiser::ReBLUR};
    nlohmann::json vision_reblur_settings = nlohmann::json::object();
    nlohmann::json vision_pt_settings = nlohmann::json::object();
    nlohmann::json vision_restir_settings = nlohmann::json::object();
    nlohmann::json vision_relax_settings = nlohmann::json::object();
    nlohmann::json vision_reblur_effective_settings = nlohmann::json::object(); // Runtime feedback, not serialized.
    nlohmann::json vision_pt_effective_settings = nlohmann::json::object(); // Runtime feedback, not serialized.
    nlohmann::json vision_restir_effective_settings = nlohmann::json::object(); // Runtime feedback, not serialized.
    nlohmann::json vision_restir_applied_settings = nlohmann::json::object(); // Overrides matching runtime feedback, not serialized.
    nlohmann::json vision_relax_effective_settings = nlohmann::json::object(); // Runtime feedback, not serialized.
    bool vision_accumulation{false};
    bool vision_stable_planes{true};
    bool view_open{false};
    bool viewport_rect_active{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    EntityId actor_pick_handle{};

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
        proj[0][0] *= projection_x_sign;
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
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t result_x{0};
    std::uint32_t result_y{0};
    EntityId actor_handle{0};
    std::uint32_t node_index{UINT32_MAX};
    bool pending{false};
    bool result_ready{false};
};

// ---------------------------------------------------------------------------
// 相机快照（P1：跨线程只读视图）
//
// CameraDevice 是 DAG 的内部状态；外部线程（CEF / 脚本 / 主线程）过去直接
// try_acquire_read 槽位来读它，于是 UI 的读要么被 DAG 的临界区阻塞，要么
// 在 _nowait 版本下丢一整帧。这里改为每 tick 发布一份只读投影：
//   - 发布者：Optics（DAG 线程），tick 内应用完相机命令之后重建一次
//   - 读者：任意线程，读到的永远是某个完整 tick 的状态
// 字段只做投影：蒙皮/字符串设置等内部状态不进来。
// ---------------------------------------------------------------------------
struct CameraSnapshotRow {
    EntityId handle{0};
    ktm::fvec3 position{};
    ktm::fvec3 forward{};
    ktm::fvec3 world_up{};
    float fov{60.0f};
    float aspect{16.0f / 9.0f};
    float near_plane{0.1f};
    float far_plane{100.0f};
    float projection_x_sign{1.0f};
    std::uint32_t width{1920};
    std::uint32_t height{1080};
    void* surface{nullptr};
    bool follows_default_surface{true};
    bool view_open{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    CameraOutputMode output_mode{CameraOutputMode::FinalColor};
    CameraRenderBackend render_backend{CameraRenderBackend::Native};
    CameraVisionRenderMode vision_render_mode{CameraVisionRenderMode::PathTracing};
    bool vision_denoise{true};
    CameraVisionDenoiser vision_denoiser{CameraVisionDenoiser::ReBLUR};
    bool vision_accumulation{false};
    bool vision_stable_planes{true};
};

struct CameraSnapshot {
    std::uint64_t tick{0};
    std::vector<CameraSnapshotRow> rows;
};

// ---------------------------------------------------------------------------
// 变换（P2：跨线程写命令 + 读快照）
//
// ModelTransform 有 3 个非 DAG 写者（CEF gizmo、脚本、网络）和 4 个 DAG 写者。
// 过去外部写者直接 try_acquire_write 槽位，与 DAG 写者之间没有顺序保证；
// 现在外部写只走 TransformUpdateCommand，由 Optics 在 tick 内应用。
// ---------------------------------------------------------------------------
enum class TransformField : std::uint32_t {
    None = 0,
    Position = 1u << 0,
    Rotation = 1u << 1,
    Scale = 1u << 2,
    /// 整列主序仿射矩阵。写入时连同 position 一起给出，并重置 rotation/scale，
    /// 与 set_matrix() 的语义保持一致（矩阵已包含 TRS）。
    Matrix = 1u << 3,
    All = Position | Rotation | Scale,
};

constexpr TransformField operator|(TransformField lhs, TransformField rhs) {
    return static_cast<TransformField>(static_cast<std::uint32_t>(lhs) |
                                       static_cast<std::uint32_t>(rhs));
}

constexpr bool has_transform_field(TransformField fields, TransformField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct TransformUpdateCommand {
    EntityId transform_handle{};
    TransformField fields{TransformField::None};
    ktm::fvec3 position{};
    ktm::fvec3 euler_rotation{};
    ktm::fvec3 scale{1.0f, 1.0f, 1.0f};
    /// 仅 TransformField::Matrix 时有效：列主序仿射矩阵（含平移）。
    std::array<float, 16> matrix{1.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f};
};

struct TransformUpdateKey {
    static EntityId get(const TransformUpdateCommand& command) noexcept {
        return command.transform_handle;
    }
};

/// 一份变换快照行：外部线程读这份投影，不再对槽位加锁。
struct TransformSnapshotRow {
    EntityId handle{0};
    ktm::fvec3 position{};
    ktm::fvec3 euler_rotation{};
    ktm::fvec3 scale{1.0f, 1.0f, 1.0f};
    /// 作者给定的线性基（set_matrix 写入的那一份）。快照带上它是为了让
    /// get_matrix() 也能走"快照 + pending"而不再读槽位。
    ktm::fmat4x4 affine_basis{ktm::fmat4x4::from_eye()};
};

struct TransformSnapshot {
    std::uint64_t tick{0};
    std::vector<TransformSnapshotRow> rows;
};

// Runtime-only actor pick request/completion messages. These are deliberately
// kept outside ActorPickDevice so multiple clicks can be in flight without a
// newer request overwriting an older one.
struct ActorPickRequestCommand {
    EntityId camera_handle{};
    std::string scene_id;
    std::string request_id;
    std::uint32_t x{0};
    std::uint32_t y{0};
};

struct ActorPickCompletion {
    EntityId camera_handle{};
    std::string scene_id;
    std::string request_id;
    std::string status; // success, miss, error
    EntityId actor_handle{};
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t node_index{UINT32_MAX};
};

struct CameraMoveCommand {
    EntityId camera_handle{};
    ktm::fvec3 position{};
    ktm::fvec3 forward{};
    ktm::fvec3 world_up{};
    float fov{45.0f};
};

/// 相机姿态命令的合并键（LatestWins：同一台相机只保留最后一条）
struct CameraMoveKey {
    static EntityId get(const CameraMoveCommand& command) noexcept {
        return command.camera_handle;
    }
};

struct CameraViewportUpdateCommand {
    EntityId camera_handle{};
    void* surface{};
    bool view_open{false};
    int x{120};
    int y{120};
    int width{960};
    int height{540};
    int render_width{960};
    int render_height{540};
    /// 远端 CEF 重构引入：命令序号（诊断用）
    std::uint64_t sequence{};
    /// 远端 CEF 重构引入：相机命令租约。owner 退役时等待在手命令，
    /// apply_camera_viewport_update() 校验 active 后才写入。
    std::shared_ptr<CameraCommandAuthority> authority;
};

/// 视口更新命令的合并键（LatestWins：同一台相机只保留最后一次视口状态）
struct CameraViewportUpdateKey {
    static EntityId get(const CameraViewportUpdateCommand& command) noexcept {
        return command.camera_handle;
    }
};

struct DefaultSurfaceCameraUpdateCommand {
    void* surface{};
    std::uint32_t width{}, height{};
};

enum class CameraStateUpdateField : std::uint32_t {
    None = 0,
    Surface = 1u << 0,
    Size = 1u << 1,
    OutputMode = 1u << 2,
    RenderBackend = 1u << 3,
    ViewState = 1u << 4,
    VisionRenderMode = 1u << 5,
    VisionDenoise = 1u << 8,
    VisionAccumulation = 1u << 9,
    VisionStablePlanes = 1u << 10,
    VisionDenoiser = 1u << 11,
    VisionReblurSettings = 1u << 12,
    VisionRelaxSettings = 1u << 13,
    VisionPtSettings = 1u << 14,
    VisionRestirSettings = 1u << 15,
    Projection = 1u << 16,
    FollowsDefaultSurface = 1u << 17,
    /// 远端 CEF 重构引入（原本也占用 1u<<17，与本字段撞车，改到 1u<<18）：
    /// 只影响几何/速度，不改变窗口生命周期。
    ViewSettings = 1u << 18,
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
    EntityId camera_handle{};
    CameraStateUpdateField fields{CameraStateUpdateField::None};
    void* surface{};
    std::uint32_t width{1};
    std::uint32_t height{1};
    float projection_x_sign{1.0f};
    float near_plane{0.1f};
    float far_plane{100.0f};
    CameraOutputMode output_mode{CameraOutputMode::FinalColor};
    CameraRenderBackend render_backend{CameraRenderBackend::Native};
    CameraVisionRenderMode vision_render_mode{CameraVisionRenderMode::PathTracing};
    bool vision_denoise{true};
    CameraVisionDenoiser vision_denoiser{CameraVisionDenoiser::ReBLUR};
    nlohmann::json vision_reblur_settings = nlohmann::json::object();
    nlohmann::json vision_pt_settings = nlohmann::json::object();
    nlohmann::json vision_restir_settings = nlohmann::json::object();
    nlohmann::json vision_relax_settings = nlohmann::json::object();
    bool vision_accumulation{false};
    bool vision_stable_planes{true};
    bool view_open{false};
    bool follows_default_surface{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    std::uint64_t sequence{};
};

struct CameraReleaseCommand {
    EntityId camera_handle{};
    EntityId actor_pick_handle{};
};

/// 相机释放命令的合并键（Fifo 队列只用它做 peek / erase）
struct CameraReleaseKey {
    static EntityId get(const CameraReleaseCommand& command) noexcept {
        return command.camera_handle;
    }
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

struct ViewportUiPointerState {
    float x{0.0f};
    float y{0.0f};
    ViewportUiCursorShape cursor_shape{ViewportUiCursorShape::Hidden};
    std::uint64_t owner_context{};
};

struct ViewportUiState {
    EntityId camera_handle{};
    ViewportUiMode mode{ViewportUiMode::Flat2D};
    ViewportUiCursorShape cursor_shape{ViewportUiCursorShape::Arrow};
    ViewportUiCalibration calibration{};
    /// 远端 CEF 重构引入：指针状态并入 session store
    ViewportUiPointerState pointer{};
};

struct ViewportGizmoTarget {
    EntityId camera_handle{};
    std::string scene_id;
    std::string actor_name;
    EntityId actor_handle{};
};

struct ViewportGizmoPointerCommand {
    EntityId camera_handle{};
    std::string request_id;
    std::string event_type;
    float x{0.0f};
    float y{0.0f};
    float viewport_width{0.0f};
    float viewport_height{0.0f};
    std::uint32_t button{0};
    std::uint32_t buttons{0};
    std::uint32_t modifiers{0};
    std::uint64_t sequence{};
};

struct ViewportGizmoPointerResult {
    EntityId camera_handle{};
    std::string request_id;
    ViewportGizmoAxis axis{ViewportGizmoAxis::None};
    bool consumed{false};
    bool dragging{false};
    bool ended{false};
    bool cancelled{false};
    ktm::fvec3 position{};
};

struct ViewportGizmoState {
    ViewportGizmoTarget target{};
    ViewportGizmoAxis hover_axis{ViewportGizmoAxis::None};
    ViewportGizmoAxis active_axis{ViewportGizmoAxis::None};
    bool dragging{false};
    std::uint64_t sequence{};
};

struct EnvironmentDevice {
    std::shared_ptr<const nlohmann::json> authored_lighting;
    std::size_t authored_lighting_revision{};
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
    EntityId environment{};
    std::vector<EntityId> actor_handles;
    std::vector<EntityId> visible_actor_handles;
    std::vector<EntityId> camera_handles;
    EntityId active_camera_handle{};
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
    Systems::Detail::ImageFrameMetadata metadata;
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
    [[nodiscard]] std::vector<EntityId> resolve_actor_geometry_handles(
        EntityId actor_handle);
    [[nodiscard]] std::optional<EntityId>
    resolve_actor_primary_transform_handle(EntityId actor_handle);

    // Runtime-only editor metadata keyed by native actor handle. Persisted state
    // remains in scene.ini; this cache only bridges Python proxy actors to native
    // systems such as OpticsSystem.
    void set_actor_guid(EntityId actor_handle, std::string actor_guid);
    [[nodiscard]] std::string actor_guid(EntityId actor_handle) const;
    // Explicit scene reload also refreshes retained actors (e.g. directory links).
    void clear_actor_metadata(EntityId actor_handle);

    CameraStorage& camera_storage();
    const CameraStorage& camera_storage() const;

    ActorPickStorage& actor_pick_storage();
    const ActorPickStorage& actor_pick_storage() const;
    void enqueue_actor_pick_request(ActorPickRequestCommand command);
    std::optional<ActorPickRequestCommand> take_actor_pick_request(EntityId camera_handle);
    void enqueue_actor_pick_completion(ActorPickCompletion completion);
    std::vector<ActorPickCompletion> drain_actor_pick_completions();

    EnvironmentStorage& environment_storage();
    const EnvironmentStorage& environment_storage() const;

    SceneStorage& scene_storage();
    const SceneStorage& scene_storage() const;

    ImageStorage& image_storage();
    const ImageStorage& image_storage() const;

    // ---- 变换（P2）：外部写走命令，外部读走快照 ----
    void enqueue_transform_update(TransformUpdateCommand command);
    std::vector<TransformUpdateCommand> drain_transform_updates();
    void publish_transform_snapshot_from_storage();
    [[nodiscard]] std::shared_ptr<const TransformSnapshot> transform_snapshot() const;
    [[nodiscard]] std::optional<TransformSnapshotRow> transform_row(
        EntityId transform_handle) const;
    /// 有效值 = 快照（已提交）+ 待处理命令（pending）
    [[nodiscard]] std::optional<TransformSnapshotRow> transform_view(
        EntityId transform_handle) const;
    void clear_transform_updates(EntityId transform_handle);

    // ---- 相机快照（P1）：DAG 每 tick 发布，外部线程只读 ----
    /// 用一整份新快照替换当前快照（Optics，DAG 线程）
    void publish_camera_snapshot(CameraSnapshot snapshot);
    /// 从相机槽位重建整份快照（Optics，DAG 线程；tick 内应用完命令之后调用）
    void publish_camera_snapshot_from_storage();
    /// 当前快照（任意线程，可为 nullptr）
    [[nodiscard]] std::shared_ptr<const CameraSnapshot> camera_snapshot() const;
    /// 创建相机时从槽位播种一行（主线程，仅创建路径），避免首帧读到空快照
    void seed_camera_snapshot_from_storage(EntityId camera_handle);
    /// 销毁相机时移除该行（避免把已释放的槽位暴露给读者）
    void remove_camera_snapshot_row(EntityId camera_handle);
    /// 读一行的已提交值（快照）
    [[nodiscard]] std::optional<CameraSnapshotRow> camera_row(EntityId camera_handle) const;
    /// 有效值 = 快照（已提交）+ 待处理命令（pending），用于 get_* 的读己之写
    [[nodiscard]] CameraSnapshotRow camera_view(EntityId camera_handle) const;

    void publish_camera_frame_timing(EntityId camera_handle, double render_ms, double denoise_ms,
                                     CameraVisionRenderMode mode,
                                     std::chrono::steady_clock::time_point completed_at =
                                         std::chrono::steady_clock::now());
    [[nodiscard]] std::optional<CameraFrameTiming> camera_frame_timing(
        EntityId camera_handle) const;
    void enqueue_camera_move(CameraMoveCommand command);
    std::vector<CameraMoveCommand> drain_camera_moves();
    void enqueue_camera_viewport_update(CameraViewportUpdateCommand command);
    std::vector<CameraViewportUpdateCommand> drain_camera_viewport_updates();
    void enqueue_default_surface_camera_update(DefaultSurfaceCameraUpdateCommand command);
    std::optional<DefaultSurfaceCameraUpdateCommand> take_default_surface_camera_update();
    void enqueue_camera_state_update(CameraStateUpdateCommand command);
    std::vector<CameraStateUpdateCommand> drain_camera_state_updates();
    [[nodiscard]] std::optional<bool> requested_camera_vision_denoise(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_denoise(EntityId camera_handle,
                                          std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<CameraVisionDenoiser> requested_camera_vision_denoiser(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_denoiser(EntityId camera_handle,
                                           std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<nlohmann::json> requested_camera_vision_reblur_settings(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_reblur_settings(EntityId camera_handle,
                                                   std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<nlohmann::json> requested_camera_vision_pt_settings(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_pt_settings(EntityId camera_handle,
                                                   std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<nlohmann::json> requested_camera_vision_restir_settings(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_restir_settings(EntityId camera_handle,
                                                   std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<nlohmann::json> requested_camera_vision_relax_settings(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_relax_settings(EntityId camera_handle,
                                                   std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<bool> requested_camera_vision_accumulation(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_accumulation(EntityId camera_handle,
                                               std::uint64_t applied_sequence);
    [[nodiscard]] std::optional<bool> requested_camera_vision_stable_planes(
        EntityId camera_handle) const;
    void acknowledge_camera_vision_stable_planes(EntityId camera_handle,
                                               std::uint64_t applied_sequence);
    void clear_camera_state_updates(EntityId camera_handle);
    void enqueue_camera_release(CameraReleaseCommand command);
    std::vector<CameraReleaseCommand> drain_camera_releases();
    void set_viewport_ui_mode(EntityId camera_handle, ViewportUiMode mode);
    void set_viewport_ui_calibration(EntityId camera_handle,
                                     const ViewportUiCalibration& calibration);
    [[nodiscard]] ViewportUiState viewport_ui_state(EntityId camera_handle) const;
    /// 远端 CEF 重构引入：直接写/清 session store 里的指针状态
    void set_viewport_ui_pointer(EntityId camera_handle, ViewportUiPointerState pointer);
    void clear_viewport_ui_pointer(EntityId camera_handle, std::uint64_t owner_context);
    void set_viewport_gizmo_target(ViewportGizmoTarget target);
    void clear_viewport_gizmo_target(EntityId camera_handle);
    [[nodiscard]] ViewportGizmoState viewport_gizmo_state(
        EntityId camera_handle) const;
    void update_viewport_gizmo_interaction(EntityId camera_handle,
                                           ViewportGizmoAxis axis,
                                           bool dragging,
                                           ViewportGizmoAxis hover_axis =
                                               ViewportGizmoAxis::None);

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
    std::unordered_map<EntityId, std::string> actor_guids_;
    EnvironmentStorage environment_storage_;
    CameraStorage camera_storage_;
    Kernel::SnapshotChannel<CameraSnapshot> camera_snapshot_;
    Kernel::CommandBuffer<TransformUpdateCommand, TransformUpdateKey,
                          Kernel::CoalescePolicy::LatestWins>
        transform_updates_;
    Kernel::SnapshotChannel<TransformSnapshot> transform_snapshot_;
    mutable std::mutex camera_timing_mutex_;
    std::unordered_map<EntityId, CameraFrameTiming> camera_timings_;
    ActorPickStorage actor_pick_storage_;
    mutable std::mutex actor_pick_queue_mutex_;
    std::vector<ActorPickRequestCommand> pending_actor_pick_requests_;
    std::vector<ActorPickCompletion> actor_pick_completions_;
    SceneStorage scene_storage_;
    ImageStorage image_storage_;
    // 相机姿态：LatestWins（拖拽在 120Hz 下只需最后一次），P5 起统一走 CommandBuffer
    Kernel::CommandBuffer<CameraMoveCommand, CameraMoveKey,
                          Kernel::CoalescePolicy::LatestWins>
        camera_moves_;
    // 视口更新：LatestWins（同一台相机只保留最后一次），P5 起统一走 CommandBuffer
    Kernel::CommandBuffer<CameraViewportUpdateCommand, CameraViewportUpdateKey,
                          Kernel::CoalescePolicy::LatestWins>
        camera_viewport_updates_;
    // 默认 surface 更新是"单槽"语义：容量 1 的 LatestWins —— 新命令覆盖旧命令，
    // 取走即清空。key 恒为 0（结构体里没有句柄字段），因此共用默认 CommandKey。
    Kernel::CommandBuffer<DefaultSurfaceCameraUpdateCommand,
                          Kernel::CommandKey<DefaultSurfaceCameraUpdateCommand>,
                          Kernel::CoalescePolicy::LatestWins>
        default_surface_camera_update_{1};
    mutable std::mutex camera_state_update_mutex_;
    std::unordered_map<EntityId, CameraStateUpdateCommand>
        pending_camera_state_updates_;
    struct RequestedCameraBool {
        bool enabled{false};
        std::uint64_t sequence{};
    };
    struct RequestedCameraDenoiser {
        CameraVisionDenoiser type{CameraVisionDenoiser::Scene};
        std::uint64_t sequence{};
    };
    std::unordered_map<EntityId, RequestedCameraDenoiser> requested_camera_vision_denoiser_;
    struct RequestedReblurSettings {
        nlohmann::json value;
        std::uint64_t sequence{};
    };
    std::unordered_map<EntityId, RequestedReblurSettings> requested_camera_vision_reblur_settings_;
    struct RequestedPtSettings {
        nlohmann::json value;
        std::uint64_t sequence{};
    };
    struct RequestedRestirSettings {
        nlohmann::json value;
        std::uint64_t sequence{};
    };
    std::unordered_map<EntityId, RequestedPtSettings> requested_camera_vision_pt_settings_;
    std::unordered_map<EntityId, RequestedRestirSettings> requested_camera_vision_restir_settings_;
    struct RequestedRelaxSettings {
        nlohmann::json value;
        std::uint64_t sequence{};
    };
    std::unordered_map<EntityId, RequestedRelaxSettings> requested_camera_vision_relax_settings_;
    // Retained after drain until the render thread commits or discards the request.
    std::unordered_map<EntityId, RequestedCameraBool>
        requested_camera_vision_denoise_;
    std::unordered_map<EntityId, RequestedCameraBool>
        requested_camera_vision_accumulation_;
    std::unordered_map<EntityId, RequestedCameraBool>
        requested_camera_vision_stable_planes_;
    std::uint64_t camera_state_update_sequence_{0};
    // 相机释放：Fifo（不可合并 —— 创建/销毁顺序有意义），P5 起统一走 CommandBuffer
    Kernel::CommandBuffer<CameraReleaseCommand, CameraReleaseKey,
                          Kernel::CoalescePolicy::Fifo>
        camera_releases_;
    mutable std::mutex viewport_ui_mutex_;
    std::unordered_map<EntityId, ViewportUiState> viewport_ui_states_;
    mutable std::mutex viewport_gizmo_mutex_;
    std::unordered_map<EntityId, ViewportGizmoState> viewport_gizmo_states_;
    std::uint64_t viewport_gizmo_sequence_{0};
};

}  // namespace Corona
