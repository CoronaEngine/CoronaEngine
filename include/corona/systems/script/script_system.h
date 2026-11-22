#pragma once

#include <corona/events/display_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>

#include <memory>

namespace Corona::Script::Python {
struct PythonAPI;
}

namespace Corona::Systems {

/**
 * @brief 脚本系统
 *
 * 负责管理窗口、输入事件和显示设备。
 * 运行在独立线程，以 60 FPS 响应输入事件。
 *
 * 这个系统的线程会阻塞在 `PythonAPI::enter_runtime()` 里直到进程结束，因此
 * `pause()`/`resume()` 不会影响它，`get_actual_fps()` 与帧统计也不描述 Python
 * 循环自己的节奏（循环在 Python 侧按约 4 ms 轮询、60 Hz `update_runtime()`）。
 */
class ScriptSystem : public Kernel::SystemBase {
   public:
    ScriptSystem();

    ~ScriptSystem() override;

    // ========================================
    // ISystem 接口实现
    // ========================================

    std::string_view get_name() const override {
        return "Script";
    }

    int get_priority() const override {
        return 60;  // 最高优先级，最先初始化
    }

    /**
     * @brief 初始化显示系统
     * @param ctx 系统上下文
     * @return 初始化成功返回 true
     */
    bool initialize(Kernel::ISystemContext* ctx) override;

    void stop() override;

    /**
     * @brief 每帧更新显示
     *
     * 在独立线程中调用，处理窗口事件和输入
     */
    void update() override;

    /**
     * @brief 关闭显示系统
     *
     * 销毁窗口并清理显示资源
     */
    void shutdown() override;

   private:
    std::unique_ptr<Script::Python::PythonAPI> python_api_;
};

}  // namespace Corona::Systems
