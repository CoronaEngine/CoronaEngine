#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "horizon/core/logging.h"
#include "corona/kernel/core/kernel_context.h"
#include "corona/kernel/event/i_event_bus.h"
#include "corona/kernel/event/i_event_stream.h"
#include "corona/kernel/system/i_system_context.h"
#include "corona/kernel/system/i_system_manager.h"
#include "corona/kernel/system/system_base.h"
#include "corona/kernel/system/systems_thread.h"

namespace Corona::Kernel {

class SystemContext : public ISystemContext {
   public:
    explicit SystemContext(ISystemManager* system_manager)
        : system_manager_(system_manager),
          main_thread_delta_time_(0.0f),
          main_thread_frame_number_(0) {}

    IEventBus* event_bus() override {
        return KernelContext::instance().event_bus();
    }

    IEventBusStream* event_stream() override {
        return KernelContext::instance().event_stream();
    }

    ISystem* get_system(std::string_view name) override {
        auto sys = system_manager_->get_system(name);
        return sys ? sys.get() : nullptr;
    }

    float get_delta_time() const override {
        return main_thread_delta_time_;
    }

    uint64_t get_frame_number() const override {
        return main_thread_frame_number_;
    }

    void update_frame_info(float delta_time, uint64_t frame_number) {
        main_thread_delta_time_ = delta_time;
        main_thread_frame_number_ = frame_number;
    }

   private:
    ISystemManager* system_manager_;
    float main_thread_delta_time_;
    uint64_t main_thread_frame_number_;
};

class SystemManager : public ISystemManager {
   public:
    SystemManager() : context_(std::make_unique<SystemContext>(this)) {}

    void register_system(std::shared_ptr<ISystem> system) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto* base = dynamic_cast<SystemBase*>(system.get());
        if (base != nullptr) {
            base->set_context(context_.get());
        }

        systems_.push_back(system);
        systems_by_name_[std::string(system->get_name())] = system;
        systems_sorted_ = false;
    }

    std::shared_ptr<ISystem> get_system(std::string_view name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = systems_by_name_.find(std::string(name));
        if (it != systems_by_name_.end()) {
            return it->second;
        }
        return nullptr;
    }

    bool initialize_all() override {
        auto systems = snapshot_systems(true);
        for (std::size_t index = 0; index < systems.size(); ++index) {
            auto& system = systems[index];
            if (!system->initialize(context_.get())) {
                // 记录失败的系统：其 initialize() 可能只完成了一部分，shutdown() 不能假设
                // 它已完整初始化（例如 UI 系统在 CEF 初始化失败后仍持有半初始化的状态）。
                // 尚未轮到初始化的系统一并标记，避免对从未初始化的对象调用 shutdown()。
                for (std::size_t rest = index; rest < systems.size(); ++rest) {
                    failed_systems_.insert(systems[rest].get());
                }
                CFW_LOG_ERROR("Failed to initialize system: {}", system->get_name());
                return false;
            }

            CFW_LOG_INFO("Initialized system: {}", system->get_name());
        }

        return true;
    }

    void start_all() override {
        auto systems = snapshot_systems();

        if (dag_) {
            // DAG 里的系统不再各自建线程：状态照旧切到 running，update() 交给 DAG 线程
            for (auto& system : systems) {
                if (dag_system_names_.count(std::string(system->get_name())) != 0) {
                    system->start_inline();
                    CFW_LOG_INFO("Started system (inline, DAG): {}", system->get_name());
                }
            }
            if (dag_->start()) {
                CFW_LOG_INFO("Started systems thread: {} system(s) at {}Hz", dag_->graph().node_count(),
                             dag_->tick_rate());
            } else {
                CFW_LOG_ERROR("Failed to start systems thread");
            }
        }

        for (auto& system : systems) {
            if (dag_ && dag_system_names_.count(std::string(system->get_name())) != 0) {
                continue;
            }
            system->start();
            CFW_LOG_INFO("Started system: {}", system->get_name());
        }
    }

    void pause_all() override {
        auto systems = snapshot_systems();
        if (dag_) {
            dag_->pause();
        }
        for (auto& system : systems) {
            if (dag_ && dag_system_names_.count(std::string(system->get_name())) != 0) {
                continue;
            }
            system->pause();
        }
    }

    void resume_all() override {
        auto systems = snapshot_systems();
        if (dag_) {
            dag_->resume();
        }
        for (auto& system : systems) {
            if (dag_ && dag_system_names_.count(std::string(system->get_name())) != 0) {
                continue;
            }
            system->resume();
        }
    }

    void stop_all() override {
        auto systems = snapshot_systems();

        // DAG 线程必须最先停：它是这批系统 update() 的唯一调用者，先停它才能保证
        // 后面 Display/UI 的关停序列不会被一次 tick 打断。
        if (dag_) {
            dag_->stop();
            CFW_LOG_INFO("Stopped systems thread");
        }

        // UI produces frames consumed by Display. Stop that producer first, then
        // stop Display immediately so it cannot continue presenting while the
        // remaining systems are draining.
        auto stop_named = [&](std::string_view name) {
            for (const auto& system : systems) {
                if (system->get_name() == name) {
                    system->stop();
                    CFW_LOG_INFO("Stopped system: {}", system->get_name());
                    return;
                }
            }
        };
        stop_named("UI");
        stop_named("Display");

        for (auto it = systems.rbegin(); it != systems.rend(); ++it) {
            const auto name = (*it)->get_name();
            if (name == "UI" || name == "Display") {
                continue;
            }
            (*it)->stop();
            CFW_LOG_INFO("Stopped system: {}", name);
        }
    }

    void shutdown_all() override {
        auto systems = snapshot_systems();
        for (auto it = systems.rbegin(); it != systems.rend(); ++it) {
            if (failed_systems_.count(it->get()) != 0) {
                // initialize() 失败的系统不执行 shutdown()：半初始化的对象上调用
                // shutdown 属于未定义行为（CEF 未初始化时 CefShutdown 会挂死/崩溃）。
                CFW_LOG_WARNING("Shutdown skipped for system with failed initialization: {}",
                                (*it)->get_name());
                continue;
            }
            (*it)->shutdown();
            CFW_LOG_INFO("Shutdown system: {}", (*it)->get_name());
        }
    }

    void sync_all() override {
        auto systems = snapshot_systems();
        for (auto& system : systems) {
            system->sync();
        }
    }

    std::vector<std::shared_ptr<ISystem>> get_all_systems() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return systems_;
    }

    SystemStats get_system_stats(std::string_view name) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto sys = get_system_unlocked(name);
        if (!sys) {
            return SystemStats{};
        }

        SystemStats stats;
        fill_stats(stats, *sys);
        return stats;
    }

    std::vector<SystemStats> get_all_stats() override {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<SystemStats> all_stats;
        all_stats.reserve(systems_.size());

        for (const auto& sys : systems_) {
            SystemStats stats;
            fill_stats(stats, *sys);
            all_stats.push_back(stats);
        }

        return all_stats;
    }

    SystemContext* get_context() {
        return context_.get();
    }

    // ========================================
    // DAG 模式
    // ========================================

    bool configure_dag(const SystemDagConfig& config, std::string* error) override {
        std::lock_guard<std::mutex> lock(mutex_);

        dag_.reset();
        dag_config_ = config;
        dag_system_names_.clear();

        if (config.nodes.empty()) {
            if (error) *error = "system dag: empty node list";
            return false;
        }

        SystemsThreadConfig thread_config;
        thread_config.tick_rate = config.tick_rate;
        thread_config.nodes.reserve(config.nodes.size());

        for (const auto& node : config.nodes) {
            auto it = systems_by_name_.find(std::string(node.system));
            if (it == systems_by_name_.end()) {
                if (error) {
                    *error = "system dag: unknown system '" + std::string(node.system) + "'";
                }
                return false;
            }
            SystemsThreadNode spec;
            spec.system = it->second.get();
            spec.after = node.after;
            spec.period_ticks = node.period_ticks;
            spec.phase = node.phase;
            thread_config.nodes.push_back(spec);
            dag_system_names_.insert(std::string(node.system));
        }

        auto dag = std::make_unique<SystemsThread>(std::move(thread_config));
        if (!dag->prepare(error)) {
            return false;
        }
        dag_ = std::move(dag);
        return true;
    }

    [[nodiscard]] bool dag_enabled() const override {
        return dag_ != nullptr;
    }

    [[nodiscard]] std::string dag_dump() const override {
        return dag_ ? dag_->dump() : std::string{};
    }

   private:
    std::vector<std::shared_ptr<ISystem>> snapshot_systems(bool ensure_sorted = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ensure_sorted && !systems_sorted_) {
            std::sort(systems_.begin(), systems_.end(),
                      [](const auto& a, const auto& b) {
                          return a->get_priority() > b->get_priority();
                      });
            systems_sorted_ = true;
        }
        return systems_;
    }

    std::shared_ptr<ISystem> get_system_unlocked(std::string_view name) {
        auto it = systems_by_name_.find(std::string(name));
        if (it != systems_by_name_.end()) {
            return it->second;
        }
        return nullptr;
    }

    /**
     * @brief 填一个 SystemStats
     *
     * DAG 里的系统没有自己的线程，SystemBase 的自测统计不会更新，因此改从
     * 帧图快照取：invocations 即帧数，耗时即该节点的墙钟耗时。这样
     * 编辑器/日志里的"系统帧时间"在两种模式下口径一致。
     */
    void fill_stats(SystemStats& out, ISystem& sys) {
        out.name = sys.get_name();
        out.state = sys.get_state();
        out.target_fps = sys.get_target_fps();
        out.actual_fps = sys.get_actual_fps();
        out.average_frame_time_ms = sys.get_average_frame_time();
        out.max_frame_time_ms = sys.get_max_frame_time();
        out.total_frames = sys.get_total_frames();
        out.total_update_calls = sys.get_total_frames();

        if (!dag_) {
            return;
        }
        const FrameGraphStats snapshot = dag_->stats();
        const std::string_view name = sys.get_name();
        for (const auto& node : snapshot.nodes) {
            if (node.name != name) {
                continue;
            }
            out.total_frames = node.stats.invocations;
            out.total_update_calls = node.stats.invocations;
            out.average_frame_time_ms = static_cast<float>(node.stats.average_ms);
            out.max_frame_time_ms = static_cast<float>(node.stats.max_ms);

            const std::uint64_t ticks = dag_->ticks();
            if (ticks != 0) {
                out.actual_fps = static_cast<float>(
                    static_cast<double>(node.stats.invocations) * dag_->tick_rate() /
                    static_cast<double>(ticks));
            }
            // DAG 系统的 running/paused 由调度器统一表达，避免"系统状态 running
            // 但线程根本不存在"这种自相矛盾的读数
            out.state = dag_->is_running() ? SystemState::running : SystemState::stopped;
            return;
        }
    }

    std::unique_ptr<SystemContext> context_;
    std::vector<std::shared_ptr<ISystem>> systems_;
    std::map<std::string, std::shared_ptr<ISystem>> systems_by_name_;
    // 初始化失败的系统（只由主线程在 initialize_all/shutdown_all 中读写）
    std::unordered_set<const ISystem*> failed_systems_;
    // DAG 模式：把一批系统收进一条线程（由主线程在 configure/start/stop 里读写）
    std::unique_ptr<SystemsThread> dag_;
    SystemDagConfig dag_config_;
    std::unordered_set<std::string> dag_system_names_;
    std::mutex mutex_;
    bool systems_sorted_ = false;
};

std::unique_ptr<ISystemManager> create_system_manager() {
    return std::make_unique<SystemManager>();
}

}  // namespace Corona::Kernel
