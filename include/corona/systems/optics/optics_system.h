#pragma once

#include "horizon.h"
#include <corona/events/optics_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/display/published_image.h>
#include <corona/systems/optics/vision_pipeline_key.h>
#include <corona/systems/optics/vision_scene_resource.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 前向声明 Hardware 结构体
struct Hardware;

namespace ocarina {
class Device;
}

namespace Corona::Systems {

class GeometrySystem;  // 前向声明，用于 LOD 查询

namespace Vision {
struct VisionBuildResult;  // 定义见 vision/vision_geometry_adapter.h
class VisionZeroCopyBridge;  // 定义见 vision/vision_zero_copy_bridge.h
}  // namespace Vision

/**
 * @brief 光学系统 (Optics System)
 *
 * 负责场景光学渲染、光线追踪、GPU 资源管理和渲染管线控制。
 * 运行在独立线程，以 120 FPS 渲染场景。
 */

class OpticsSystem : public Kernel::SystemBase {
   public:
    OpticsSystem();
    ~OpticsSystem() override;

    // ========================================
    // ISystem 接口实现
    // ========================================

    std::string_view get_name() const override {
        return "Optics";
    }

    int get_priority() const override {
        return 90;  // 高优先级，在显示系统之后初始化
    }

    /**
     * @brief 初始化光学系统
     * @param ctx 系统上下文
     * @return 初始化成功返回 true
     */
    bool initialize(Kernel::ISystemContext* ctx) override;

    /**
     * @brief 每帧渲染
     *
     * 在独立线程中调用，执行场景光学渲染
     */
    void update() override;

    [[nodiscard]] Kernel::ComponentAccess component_access() const override;

    /**
     * @brief 关闭光学系统
     *
     * 清理所有 GPU 资源和渲染管线
     */
    void shutdown() override;

   private:
    friend struct VisionEngineSceneTest;
    friend struct VisionDeviceLifetimeTest;
    /// Apply a frame's camera/transform commands and publish committed camera state.
    void apply_camera_and_transform_updates();
    bool initialize_hardware_resources();
    bool initialize_render_pipelines();
    /// Allocate visibility and depth targets for camera-follow UI meshes.
    void ensure_ui_view_resources(EntityId camera_handle,
                                  uint32_t width,
                                  uint32_t height,
                                  uint64_t frame_index);
    void evict_idle_ui_view_resources(uint64_t frame_index);
    struct RenderView {
        EntityId scene_handle{};
        EntityId camera_handle{};
        CameraVisionRenderMode vision_mode{CameraVisionRenderMode::PathTracing};
        bool participating_media{false};
    };
    struct LockedRenderView {
        const SceneDevice* scene{nullptr};
        // Camera settings are small value data. Never hold their storage lock
        // across shader compilation, GPU execution or presentation waits.
        CameraDevice camera;
    };
    struct RenderFramePlan {
        std::vector<RenderView> views;
        bool has_cameras{false};
    };
    [[nodiscard]] RenderFramePlan build_render_frame_plan();
    [[nodiscard]] std::optional<LockedRenderView> lock_render_view(
        const RenderView& view);
    void optics_pipeline(uint64_t frame_index);
    void process_pending_screenshots(EntityId camera_handle, Horizon::HardwareImage& render_target,
                                     uint32_t width, uint32_t height);



    // Vision 相关私有方法
    bool init_vision_lazy();  ///< 首次渲染时初始化 Vision
    void run_vision_frame(uint64_t frame_index,
                         const std::vector<RenderView>& views);

    using VisionPipelineKey = Vision::VisionPipelineKey;
    using VisionPipelineKeyHash = Vision::VisionPipelineKeyHash;
    using VisionSceneResource = Vision::VisionSceneResource;
    struct VisionPipelineRuntime;

    VisionPipelineRuntime& get_or_create_runtime(const VisionPipelineKey& key);
    VisionPipelineRuntime& active_vision_runtime();
    struct VisionCameraFeedback {
        std::optional<nlohmann::json> restir_effective, restir_applied;
        std::optional<nlohmann::json> pt_effective, reblur_effective, relax_effective;
        std::optional<nlohmann::json> reblur_contexts;
    };
    void publish_vision_camera_feedback(EntityId camera_handle,
                                       const VisionCameraFeedback& feedback);
    bool prepare_vision_camera_view(VisionPipelineRuntime& runtime,
                                   EntityId camera_handle,
                                   uint32_t width, uint32_t height,
                                   bool denoise, bool accumulation, bool stable_planes = true,
                                   CameraVisionDenoiser denoiser = CameraVisionDenoiser::Scene,
                                   const nlohmann::json& reblur_settings = nlohmann::json::object(),
                                   const nlohmann::json& relax_settings = nlohmann::json::object(),
                                   const nlohmann::json& pt_settings = nlohmann::json::object(),
                                   const nlohmann::json& restir_settings = nlohmann::json::object(),
                                   VisionCameraFeedback* deferred_feedback = nullptr,
                                   std::size_t imported_media_count = 0);
    VisionPipelineKey make_vision_pipeline_key(Corona::CameraVisionRenderMode mode) const;
    std::shared_ptr<VisionSceneResource> get_or_create_vision_scene_resource();
    void release_unused_vision_scene_resources();
    void evict_idle_vision_runtimes(uint64_t frame_index);
    void activate_single_vision_runtime_key(const VisionPipelineKey& key);
    void clear_vision_runtimes();
    void shutdown_vision_backend();

    void apply_vision_render_mode(Corona::CameraVisionRenderMode mode);
    bool load_engine_built_vision_scene(Corona::CameraVisionRenderMode mode);
    bool initialize_engine_built_vision_runtime(VisionPipelineRuntime& runtime,
                                                const VisionPipelineKey& key);

    /// 计算当前 SharedDataHub 场景的轻量签名，用于检测动态变化
    /// （几何拓扑 / transform / 材质参数 / materialColor / visible）。
    // A caller holding a scene lock must pass that scene to avoid locking it again.
    std::size_t compute_vision_scene_signature(const SceneDevice* locked_scene = nullptr) const;

    /// 以"全量重建"方式把当前场景数据重新同步到 Vision：
    /// build_vision_geometry → scene.prepare → prepare_geometry → invalidate。
    /// 复用现有 pipeline（材质类型固定，无需重编译着色器），并通过
    /// scene.prepare() 内部的 remove_unused_elements() 回收旧材质，避免累积泄漏。
    /// 返回本次重建的统计结果，供去抖/重试逻辑区分"空场景"与"数据未就绪"。
    Vision::VisionBuildResult rebuild_vision_scene(VisionPipelineRuntime& runtime);

    /// 去抖检测：若签名变化则触发（延迟）重建，覆盖导入/导出/参数调整等动态操作。
    void sync_vision_scene_topology(VisionPipelineRuntime& runtime);
    void sync_vision_dynamic_scene(VisionPipelineRuntime& runtime);
    bool sync_engine_vision_lighting(VisionPipelineRuntime& runtime, const EnvironmentDevice& environment);

    Corona::CameraVisionRenderMode current_vision_render_mode_{
        Corona::CameraVisionRenderMode::PathTracing};
    std::size_t last_vision_mode_conflict_signature_{0};
    std::optional<VisionPipelineKey> active_vision_runtime_key_;
    // Declared before the dependents so they are also destroyed first during
    // ordinary C++ destruction. Normal engine shutdown releases this explicitly.
    std::unique_ptr<ocarina::Device> vision_device_;
    std::unordered_map<VisionPipelineKey,
                       std::unique_ptr<VisionPipelineRuntime>,
                       VisionPipelineKeyHash>
        vision_runtimes_;
    std::shared_ptr<VisionSceneResource> vision_scene_resource_;


    // ========================================================================
    // Per-surface optics render output
    // ========================================================================
    // Each surface has its own final image and shared-storage handle so rendering
    // cameras to different surfaces does not overwrite their outputs. DisplaySystem
    // composites each surface independently.
    struct SurfaceRenderTarget {
        Detail::PublishedImage published_image;
        Horizon::HardwareImage final_output;        ///< RGBA16F final output owned by this surface.
        Horizon::HardwareImage ui_overlay;          ///< Pass 2 camera-follow actor overlay
        Horizon::HardwareImage ui_warped_overlay;   ///< LFD-warped UI overlay for Stereo3D mode
        EntityId image_handle = 0;   ///< ImageStorage handle owned by this surface.
        uint32_t width = 0;                ///< Current output image width in pixels.
        uint32_t height = 0;
        uint64_t last_used_frame = 0;      ///< Last Optics update that acquired this target; used for idle eviction.
    };

    /// Acquire, create, or resize a surface target and refresh last_used_frame.
    /// width/height specify the current camera resolution; surface must not be null.
    SurfaceRenderTarget& acquire_surface_target(void* surface, uint32_t width,
                                                uint32_t height, uint64_t frame_index);
    SurfaceRenderTarget& acquire_offscreen_screenshot_target(EntityId camera_handle,
                                                             uint32_t width,
                                                             uint32_t height,
                                                             uint64_t frame_index);

    /// Release GPU images and storage handles for targets unused by any camera
    /// beyond the idle threshold, reclaiming resources as viewports come and go.
    void evict_idle_surface_targets(uint64_t frame_index);
    void release_surface_target(SurfaceRenderTarget& target);
    void evict_idle_offscreen_screenshot_targets(uint64_t frame_index);

    /// 在 background 上渲染 follow-camera UI actor + 可选柱镜 warp + composite。
    /// 仅在 hardware_->executor 上“记录”pass，不 commit。有 follow-camera 实例时
    /// 就地合成至 background；调用方负责 commit + publish。
    /// Record after the background pass on the same stream.
    Horizon::HardwareImage* compose_surface_ui_overlay(Horizon::HardwareStream& stream,
                                                       EntityId camera_handle,
                                                       const CameraDevice& camera,
                                                       const SceneDevice& scene,
                                                       SurfaceRenderTarget& target,
                                                       Horizon::HardwareImage& background,
                                                       const ViewportUiState& ui_state,
                                                       uint64_t frame_index);

    std::unordered_map<void*, SurfaceRenderTarget> surface_targets_;
    std::unordered_map<std::uintptr_t, SurfaceRenderTarget> offscreen_screenshot_targets_;
    /// 空闲多少帧后回收一个 surface 目标（约 2s @120fps）。
    static constexpr uint64_t kSurfaceTargetIdleEvictFrames = 240;

    struct UiPassLogState {
        bool has_state = false;
        bool has_follow_camera_instances = false;
        bool stereo_ui = false;
        std::uint32_t instance_count = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    std::unordered_map<std::uintptr_t, UiPassLogState> ui_pass_log_states_;

    struct UiViewResources;
    std::unordered_map<std::uintptr_t, std::unique_ptr<UiViewResources>>
        ui_view_resources_;
    static constexpr uint64_t kUiViewIdleEvictFrames = 240;

    std::unique_ptr<Hardware> hardware_;
    std::chrono::steady_clock::duration explicit_render_interval_{};
    std::chrono::steady_clock::time_point next_render_update_{};

    // LOD 系统引用（渲染时查询 LOD 缓冲）
    GeometrySystem* geometry_system_ = nullptr;
    bool geometry_system_queried_ = false;  // 避免每帧重试失败的获取

    // Vision 后端状态
    bool vision_initialized_{false};
    std::chrono::steady_clock::time_point next_vision_init_attempt_{};

    // ---- Vision 动态场景同步（脏标记 + 去抖全量重建）----
    static constexpr uint32_t kVisionRebuildDebounceFrames = 3;  ///< 稳定该帧数后才重建

    // Incomplete builds never advance the applied signature. Repeated failures
    // back off to a five-second retry while preserving the last complete scene.
    static constexpr uint32_t kVisionRebuildMaxRetries = 30;  ///< 重试上限（约 0.5s @60fps）

    // ---- Vision 外部场景加载（跨线程：事件写请求，渲染线程消费）----

    struct PendingScreenshot {
        EntityId camera_handle = 0;
        std::string file_path;
        std::shared_ptr<std::promise<bool>> completion_promise;
        std::chrono::steady_clock::time_point expires_at{};
        unsigned render_frames{};
        unsigned warmup_frames{};
        std::string timings_path;
    };
    // Only requests promoted before draining camera commands may use this frame.
    std::vector<PendingScreenshot> incoming_screenshots_;
    std::vector<PendingScreenshot> pending_screenshots_;
    std::mutex screenshot_mutex_;
    void begin_screenshot_frame();
    bool has_pending_screenshot(EntityId camera_handle);
    // A zero camera handle cancels every request during shutdown.
    void fail_pending_screenshots(EntityId camera_handle);
    void fail_expired_pending_screenshots();
    void fail_unrenderable_pending_screenshots();
    Kernel::EventId screenshot_request_sub_id_ = 0;
};

}  // namespace Corona::Systems
