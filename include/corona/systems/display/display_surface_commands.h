#pragma once

/// DisplaySurface 的入站命令通道（P5：退役跨线程同步回调）。
///
/// 背景：`DisplaySurfaceChangedEvent` / `DisplaySurfaceRemovedEvent` 由主线程 /
/// CEF / Vulkan 后端发布，而 EventBus 是**同步**的 —— 处理器会跑在发布线程上，
/// 于是 DisplaySystem 必须用 `DisplayCallbackGate` 防重入、用延迟队列兜住。
///
/// 改成入站命令后：
/// - 发布方只 push（一个 mutex，无回调）；
/// - Display 在**自己的** update() 开头 drain 并处理；
/// - 调用方仍然等自己带的票据/promise（语义不变，兑现点晚 ≤1 tick）。
///
/// Fifo：注册与移除的顺序有意义，不可合并。

#include <corona/kernel/command/command_buffer.h>
#include <corona/systems/ui/ui_surface_lifecycle.h>

#include <cstdint>
#include <future>
#include <memory>
#include <optional>

namespace Corona {

struct DisplaySurfaceCommand {
    enum class Kind : std::uint8_t { Changed, Removed };

    Kind kind{Kind::Changed};
    void* surface{nullptr};
    /// 注册回执（仅 Changed 用；调用方 wait_until 它）
    std::optional<Systems::UI::SurfaceCompletionTicket> registration_ticket;
    /// 拆除回执（仅 Removed 用）
    std::optional<Systems::UI::SurfaceCompletionTicket> removal_ticket;
    /// 拆除完成信号（仅 Removed 用；Display 拆完 swapchain 后兑现）
    std::shared_ptr<std::promise<void>> done;
};

struct DisplaySurfaceCommandKey {
    static std::uintptr_t get(const DisplaySurfaceCommand& command) noexcept {
        return reinterpret_cast<std::uintptr_t>(command.surface);
    }
};

class DisplaySurfaceCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<DisplaySurfaceCommand, DisplaySurfaceCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    static DisplaySurfaceCommandChannel& instance() {
        static DisplaySurfaceCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

}  // namespace Corona
