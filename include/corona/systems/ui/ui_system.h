#pragma once

#include <SDL3/SDL.h>
#include <corona/events/script_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/i_system.h>
#include <corona/systems/ui/vulkan_backend.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace Corona::Systems {

class VulkanBackend;

/**
 * @brief UI系统
 *
 * 负责启动和管理基于 SDL3 + CEF + Vulkan quad compositor 的 UI 界面。
 * （ImGui 已移除；类名沿用历史，实现已无 ImGui 依赖。）
 * 运行在主线程（不使用独立线程），因为 SDL/CEF 都要求在主线程中运行。
 *
 * 注意：此系统直接实现 ISystem 接口，而不是继承 SystemBase，
 * 因为它不需要独立线程和 SystemBase 提供的线程管理功能。
 */
class UiSystem : public Kernel::ISystem {
   public:
    UiSystem() = default;
    ~UiSystem() override = default;

    // ========================================
    // ISystem 接口实现
    // ========================================

    [[nodiscard]] std::string_view get_name() const override { return "UI"; }
    [[nodiscard]] int get_priority() const override { return 40; }

    bool initialize(Kernel::ISystemContext* ctx) override;
    void update() override;
    void shutdown() override;

    // 主线程系统不需要线程控制，提供空实现
    void start() override;
    void stop() override;
    void pause() override {}
    void resume() override {}

    // 状态和帧率
    [[nodiscard]] Kernel::SystemState get_state() const override { return state_; }
    [[nodiscard]] int get_target_fps() const override { return 60; }

    // 性能统计：UiSystem 运行在主线程，没有 SystemBase 的线程循环为它计时，所以这里
    // 如实测量 update() 自身的耗时（与 SystemBase::thread_loop 的口径一致：帧时间 =
    // 一次 update() 的墙钟耗时，fps = 帧数 / 帧耗时之和）。
    // 原先这三个 getter 硬编码返回 0，会让 [UI/Frame] 诊断里的 UI 行读起来像"UI 不耗时"，
    // 而它其实是"未测量"——两者必须区分开。
    [[nodiscard]] float get_actual_fps() const override;
    [[nodiscard]] float get_average_frame_time() const override;
    [[nodiscard]] float get_max_frame_time() const override;
    [[nodiscard]] std::uint64_t get_total_frames() const override;
    void reset_stats() override;

    /**
     * @brief 检查 UI 系统是否仍在运行
     * @return 如果用户关闭了窗口返回 false
     */
    [[nodiscard]] bool is_ui_running() const { return running_; }

   private:
    Kernel::SystemState state_ = Kernel::SystemState::idle;

    SDL_Event event_{};
    bool show_demo_window_ = false;
    bool running_ = false;
    SDL_Window* window_ = nullptr;

    bool window_size_changed_ = false;
    bool sdl_initialized_ = false;

    std::unique_ptr<VulkanBackend> vulkan_backend_;

    int active_tab_id_ = -1;

    Kernel::EventId sdl_start_id_ = 0;

    // 帧统计（由 update() 在 UI 主线程写入，可能被其它线程读取，故用原子量）。
    std::atomic<std::uint64_t> frame_count_{0};
    std::atomic<double> total_frame_ms_{0.0};  ///< update() 耗时累加（毫秒）
    std::atomic<float> max_frame_ms_{0.0f};    ///< 单次 update() 最大耗时（毫秒）
};

}  // namespace Corona::Systems