#pragma once

/// 帧就绪的入站命令通道（P5 第二步：彻底移除跨线程同步回调）。
///
/// `OpticsFrameReadyEvent` 由 Optics 在 DAG 线程发布；`UIFrameReadyEvent` 由
/// VulkanBackend 在**主线程**发布 —— 后者是唯一剩下的跨线程同步回调，
/// 也是 DisplayCallbackGate 存在的最后理由。
///
/// 改成命令后：发布方只 push；Display 在自己的 update() 里 drain 并处理。
/// 帧命令是"最新覆盖"语义（同 surface 只保留最后一帧），被覆盖帧的
/// PublishedImage 令牌随命令析构自然释放。

#include <corona/events/display_system_events.h>
#include <corona/kernel/command/command_buffer.h>

#include <cstdint>

namespace Corona {

struct OpticsFrameCommand {
    Events::OpticsFrameReadyEvent event;
};

struct UIFrameCommand {
    Events::UIFrameReadyEvent event;
};

struct OpticsFrameCommandKey {
    static std::uintptr_t get(const OpticsFrameCommand& command) noexcept {
        return reinterpret_cast<std::uintptr_t>(command.event.surface);
    }
};

struct UIFrameCommandKey {
    static std::uintptr_t get(const UIFrameCommand& command) noexcept {
        return reinterpret_cast<std::uintptr_t>(command.event.surface);
    }
};

class OpticsFrameCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<OpticsFrameCommand, OpticsFrameCommandKey,
                                         Kernel::CoalescePolicy::LatestWins>;
    static OpticsFrameCommandChannel& instance() {
        static OpticsFrameCommandChannel channel;
        return channel;
    }
    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

class UIFrameCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<UIFrameCommand, UIFrameCommandKey,
                                         Kernel::CoalescePolicy::LatestWins>;
    static UIFrameCommandChannel& instance() {
        static UIFrameCommandChannel channel;
        return channel;
    }
    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

/// 允许发布点写成 `push_optics_frame({...});`（花括号实参无需显式类型）
struct OpticsFramePusher {
    void operator()(Events::OpticsFrameReadyEvent event) const {
        OpticsFrameCommandChannel::instance().commands().push(
            OpticsFrameCommand{std::move(event)});
    }
};

struct UIFramePusher {
    void operator()(Events::UIFrameReadyEvent event) const {
        UIFrameCommandChannel::instance().commands().push(
            UIFrameCommand{std::move(event)});
    }
};

inline const OpticsFramePusher push_optics_frame{};
inline const UIFramePusher push_ui_frame{};

}  // namespace Corona
