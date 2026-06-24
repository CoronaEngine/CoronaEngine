#include "corona/engine.h"

#include <corona/events/engine_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/audio.h>
#include <corona/resource/types/image.h>
#include <corona/resource/types/scene.h>
#include <corona/resource/types/text.h>
#include <corona/resource/types/video.h>
#include <corona/systems/acoustics/acoustics_system.h>
#include <corona/systems/display/display_system.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/systems/mechanics/mechanics_system.h>
#include <corona/systems/network/network_system.h>
#include <corona/systems/optics/optics_system.h>
#include <corona/systems/script/script_system.h>
#include <corona/systems/ui/imgui_system.h>

#include <chrono>
#include <memory>
#include <thread>

namespace Corona {

// 保存 ImguiSystem 指针，用于主循环中直接调用
static Systems::ImguiSystem* g_imgui_system_ = nullptr;

// ============================================================================
// 构造与析构
// ============================================================================

Engine::Engine()
    : kernel_(Kernel::KernelContext::instance()),
      initialized_(false),
      running_(false),
      exit_requested_(false),
      frame_number_(0),
      last_frame_time_(0.0f) {
}

Engine::~Engine() {
    if (running_.load()) {
        shutdown();
    }
}

// ============================================================================
// 生命周期管理
// ============================================================================

bool Engine::initialize() {
    if (initialized_.load()) {
        return true;
    }

    // 1. 初始化 KernelContext
    if (!kernel_.initialize()) {
        CFW_LOG_CRITICAL("Failed to initialize KernelContext");
        return false;
    }

    CFW_LOG_NOTICE("====================================");
    CFW_LOG_NOTICE("CoronaEngine Initializing...");
    CFW_LOG_NOTICE("====================================");

    // 2. 注册核心系统
    if (!register_systems()) {
        CFW_LOG_CRITICAL("Failed to register systems");
        return false;
    }

    auto& resource_manager = Resource::ResourceManager::get_instance();
    resource_manager.register_parser<Resource::TextParser>();
    resource_manager.register_parser<Resource::ImageParser>();
    resource_manager.register_parser<Resource::SceneParser>();
    resource_manager.register_parser<Resource::VideoParser>();
    resource_manager.register_parser<Resource::AudioParser>();

    // 3. 初始化所有系统
    auto* sys_mgr = kernel_.system_manager();
    if (!sys_mgr || !sys_mgr->initialize_all()) {
        CFW_LOG_CRITICAL("Failed to initialize systems");
        return false;
    }

    initialized_.store(true);

    CFW_LOG_NOTICE("====================================");
    CFW_LOG_NOTICE("CoronaEngine Initialized Successfully");
    CFW_LOG_NOTICE("====================================");

    return true;
}

void Engine::run() {
    if (!initialized_.load()) {
        CFW_LOG_CRITICAL("Cannot run engine: not initialized");
        return;
    }

    if (running_.load()) {
        CFW_LOG_WARNING("Engine is already running");
        return;
    }

    running_.store(true);
    exit_requested_.store(false);

    CFW_LOG_NOTICE("====================================");
    CFW_LOG_NOTICE("CoronaEngine Starting Main Loop");
    CFW_LOG_NOTICE("====================================");

    // 启动所有系统线程
    auto* sys_mgr = kernel_.system_manager();
    if (sys_mgr) {
        sys_mgr->start_all();
    }

    // 主循环
    auto last_time = std::chrono::high_resolution_clock::now();
    constexpr auto target_frame_duration = std::chrono::microseconds(16666);  // 60 FPS

    while (!exit_requested_.load()) {
        auto frame_start_time = std::chrono::high_resolution_clock::now();

        // 计算帧时间
        std::chrono::duration<float> delta_duration = frame_start_time - last_time;
        last_frame_time_ = delta_duration.count();
        last_time = frame_start_time;

        // 执行一帧
        tick();

        // 检查 UI 系统是否请求退出（用户关闭窗口）
        if (g_imgui_system_ && !g_imgui_system_->is_ui_running()) {
            CFW_LOG_INFO("UI window closed, requesting engine exit...");
            request_exit();
        }

        // 帧号递增
        frame_number_++;

        // 帧率控制（120 FPS）
        auto frame_end_time = std::chrono::high_resolution_clock::now();
        auto frame_elapsed = frame_end_time - frame_start_time;

        // 计算剩余时间并 sleep
        if (frame_elapsed < target_frame_duration) {
            auto sleep_duration = target_frame_duration - frame_elapsed;
            std::this_thread::sleep_for(sleep_duration);
        }
    }

    CFW_LOG_NOTICE("====================================");
    CFW_LOG_NOTICE("CoronaEngine Main Loop Exited");
    CFW_LOG_NOTICE("====================================");

    running_.store(false);
}

void Engine::request_exit() {
    exit_requested_.store(true);

    CFW_LOG_NOTICE("Engine exit requested");
}

void Engine::shutdown() {
    if (!initialized_.load()) {
        return;
    }

    auto* sys_mgr = kernel_.system_manager();
    if (sys_mgr) {
        sys_mgr->stop_all();
        sys_mgr->shutdown_all();
    }

    CFW_LOG_NOTICE("====================================");
    CFW_LOG_NOTICE("CoronaEngine Shutting Down...");
    CFW_LOG_NOTICE("====================================");

    // 关闭内核（SystemManager 的析构函数会自动调用 shutdown_all() 和 stop_all()）
    // 注意：kernel_.shutdown() 会重置 logger，所以之后不能再使用 logger 指针
    kernel_.shutdown();

    initialized_.store(false);

    // 不要在 kernel_.shutdown() 之后使用 logger，因为它已经被释放
}

// ============================================================================
// 状态查询
// ============================================================================

bool Engine::is_initialized() const {
    return initialized_.load();
}

bool Engine::is_running() const {
    return running_.load();
}

// ============================================================================
// 系统访问
// ============================================================================

Kernel::KernelContext& Engine::kernel() {
    return kernel_;
}

Kernel::ISystemManager* Engine::system_manager() {
    return kernel_.system_manager();
}

Kernel::IEventBus* Engine::event_bus() {
    return kernel_.event_bus();
}

Kernel::IEventBusStream* Engine::event_stream() {
    return kernel_.event_stream();
}

// ============================================================================
// 内部方法
// ============================================================================

bool Engine::register_systems() {
    auto* sys_mgr = kernel_.system_manager();
    if (!sys_mgr) {
        CFW_LOG_CRITICAL("SystemManager is null, cannot register systems");
        return false;
    }

    // 注册核心系统（按优先级自动排序）
    // Geometry(85) > Mechanics(75)：八叉树在同帧物理前重建，query_pairs() 时序正确

    // Display System - 最高优先级
    auto display_system = std::make_shared<Systems::DisplaySystem>();
    sys_mgr->register_system(display_system);

    // Optics System (光学系统)
    auto optics_system = std::make_shared<Systems::OpticsSystem>();
    sys_mgr->register_system(optics_system);

    // Geometry System (几何 + 八叉树宿主，原 SceneSystem 职责已并入)
    auto geometry_system = std::make_shared<Systems::GeometrySystem>();
    sys_mgr->register_system(geometry_system);

    // Mechanics System (力学系统)
    sys_mgr->register_system(std::make_shared<Systems::MechanicsSystem>());

    // Acoustics System (声学系统)
    sys_mgr->register_system(std::make_shared<Systems::AcousticsSystem>());

    sys_mgr->register_system(std::make_shared<Systems::ScriptSystem>());

    // Network System (ENet LAN collaborative editing)
    sys_mgr->register_system(std::make_shared<Systems::NetworkSystem>());

    // ImguiSystem - 运行在主线程
    auto imgui_system = std::make_shared<Systems::ImguiSystem>();
    g_imgui_system_ = imgui_system.get();  // 保存指针用于主循环
    sys_mgr->register_system(imgui_system);

    CFW_LOG_NOTICE("All core systems registered successfully");

    return true;
}

void Engine::tick() {
    // 在主线程中更新 ImguiSystem
    // SDL/ImGui 必须在主线程中运行
    if (g_imgui_system_) {
        g_imgui_system_->update();
    }

    // 系统通过 SystemBase 的 delta_time() 和 frame_number() 访问帧信息

    // 同步所有系统（可选）
    // 其他系统在各自的线程中运行，主循环可以在这里进行跨系统的同步

    // 收集性能统计
}

}  // namespace Corona
