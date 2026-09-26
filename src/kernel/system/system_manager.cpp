#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "horizon/core/logging.h"
#include "corona/kernel/core/kernel_context.h"
#include "corona/kernel/event/i_event_bus.h"
#include "corona/kernel/event/i_event_stream.h"
#include "corona/kernel/system/i_system_context.h"
#include "corona/kernel/system/i_system_manager.h"
#include "corona/kernel/system/system_base.h"

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
        for (auto& system : systems) {
            if (!system->initialize(context_.get())) {
                CFW_LOG_ERROR("Failed to initialize system: {}", system->get_name());
                return false;
            }

            CFW_LOG_INFO("Initialized system: {}", system->get_name());
        }

        return true;
    }

    void start_all() override {
        auto systems = snapshot_systems();
        for (auto& system : systems) {
            system->start();
            CFW_LOG_INFO("Started system: {}", system->get_name());
        }
    }

    void pause_all() override {
        auto systems = snapshot_systems();
        for (auto& system : systems) {
            system->pause();
        }
    }

    void resume_all() override {
        auto systems = snapshot_systems();
        for (auto& system : systems) {
            system->resume();
        }
    }

    void stop_all() override {
        auto systems = snapshot_systems();
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
        stats.name = sys->get_name();
        stats.state = sys->get_state();
        stats.target_fps = sys->get_target_fps();
        stats.actual_fps = sys->get_actual_fps();
        stats.average_frame_time_ms = sys->get_average_frame_time();
        stats.max_frame_time_ms = sys->get_max_frame_time();
        stats.total_frames = sys->get_total_frames();
        stats.total_update_calls = sys->get_total_frames();
        return stats;
    }

    std::vector<SystemStats> get_all_stats() override {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<SystemStats> all_stats;
        all_stats.reserve(systems_.size());

        for (const auto& sys : systems_) {
            SystemStats stats;
            stats.name = sys->get_name();
            stats.state = sys->get_state();
            stats.target_fps = sys->get_target_fps();
            stats.actual_fps = sys->get_actual_fps();
            stats.average_frame_time_ms = sys->get_average_frame_time();
            stats.max_frame_time_ms = sys->get_max_frame_time();
            stats.total_frames = sys->get_total_frames();
            stats.total_update_calls = sys->get_total_frames();

            all_stats.push_back(stats);
        }

        return all_stats;
    }

    SystemContext* get_context() {
        return context_.get();
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

    std::unique_ptr<SystemContext> context_;
    std::vector<std::shared_ptr<ISystem>> systems_;
    std::map<std::string, std::shared_ptr<ISystem>> systems_by_name_;
    std::mutex mutex_;
    bool systems_sorted_ = false;
};

std::unique_ptr<ISystemManager> create_system_manager() {
    return std::make_unique<SystemManager>();
}

}  // namespace Corona::Kernel
