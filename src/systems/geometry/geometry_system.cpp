#include <corona/events/engine_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/shared_data_hub.h>
#include <corona/kernel/utils/storage.h>
#include <corona/spatial/octree.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/utils/path_utils.h>
#include <ktm/ktm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <future>

#include <corona/resource/resource.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>

#include "geometry_internal.h"

namespace Corona::Systems {

using namespace GeometryInternal;

// ============================================================================
// 生命周期
// ============================================================================

GeometrySystem::GeometrySystem() : impl_(std::make_unique<Impl>()) {
    set_target_fps(60);
}

GeometrySystem::~GeometrySystem() = default;

bool GeometrySystem::initialize(Kernel::ISystemContext* ctx) {
    impl_->ctx = ctx;
    CFW_LOG_NOTICE("GeometrySystem: Initializing (octree host)");

    if (ctx && ctx->event_bus()) {
        auto id1 = ctx->event_bus()->subscribe<Events::ActorLoadCompletedEvent>(
            [this](const Events::ActorLoadCompletedEvent& e) {
                this->on_load_completed(e);
            });
        auto id2 = ctx->event_bus()->subscribe<Events::ActorUnloadCompletedEvent>(
           [this](const Events::ActorUnloadCompletedEvent& e) {
               this->on_unload_completed(e);
           });
        auto id3 = ctx->event_bus()->subscribe<Events::ActorLoadRequestedEvent>(
            [this](const Events::ActorLoadRequestedEvent& e) {
                this->on_load_requested(e);
            });
        auto id4 = ctx->event_bus()->subscribe<Events::ActorUnloadRequestedEvent>(
            [this](const Events::ActorUnloadRequestedEvent& e) {
                this->on_unload_requested(e);
            });

        impl_->event_subscriptions = {id1,id2,id3,id4};
    }

    return true;
}

void GeometrySystem::update() {
    auto& hub = SharedDataHub::instance();
    auto& scene_storage = hub.scene_storage();
    auto& camera_storage = hub.camera_storage();
    auto& geometry_storage = hub.geometry_storage();
    auto& transform_storage = hub.model_transform_storage();
    std::vector<std::uintptr_t> scene_handles;
    {
        for (auto it = scene_storage.cbegin(); it != scene_storage.cend(); ++it) {
            const SceneDevice& scene_dev = *it;
            scene_handles.push_back(reinterpret_cast<std::uintptr_t>(&scene_dev));
        }
    }

    process_async_tasks();

    // ---- 动态减面管线 ----
    // 用 shared_lock 快照 simplification_cfg，与 set_simplification_config 的 unique_lock 互斥
    bool run_simplification = false;
    {
        std::shared_lock lock(impl_->mtx);
        run_simplification = impl_->simplification_cfg.enabled;
    }
    if (run_simplification) {
        upload_lod_from_scene_data();
    }

    for (std::uintptr_t scene_handle : scene_handles) {
        const auto scene_begin = std::chrono::steady_clock::now();
        std::vector<std::uintptr_t> actor_handles;
        std::vector<std::uintptr_t> camera_handles;
        {
            auto scene_read = scene_storage.try_acquire_read(scene_handle);
            if ( !scene_read.valid() )  continue;
            actor_handles = scene_read->actor_handles;
            camera_handles = scene_read->camera_handles;
        }

        std::vector<typename Spatial::Octree<Impl::Payload>::Entry> octree_entries;
        std::unordered_set<Impl::Payload> added_actors;
        for (std::uintptr_t actor_handle : actor_handles) {
            if (added_actors.count(actor_handle)) continue;

            auto& actor_storage = hub.actor_storage();
            auto actor_read = actor_storage.acquire_read(actor_handle);
            if ( !actor_read ) continue;
            const ActorDevice& actor_dev = *actor_read;

            for (std::uintptr_t profile_handle : actor_dev.profile_handles) {
                auto& profile_storage = hub.profile_storage();
                auto profile_read = profile_storage.acquire_read(profile_handle);
                if (!profile_read.valid()) continue;

                const ProfileDevice& profile_dev = *profile_read;
                std::uintptr_t mechanics_handle = profile_dev.mechanics_handle;
                if ( !mechanics_handle ) continue;

                auto& mechanics_storage = hub.mechanics_storage();
                auto mechanics_read = mechanics_storage.acquire_read(mechanics_handle);
                if (!mechanics_read.valid()) continue;

                auto geometry_read = geometry_storage.acquire_read(mechanics_read->geometry_handle);
                if (!geometry_read.valid() || geometry_read->transform_handle == 0) continue;

                auto transform_read = transform_storage.acquire_read(geometry_read->transform_handle);
                if (!transform_read.valid()) continue;

                const MechanicsDevice& mechanics_dev = *mechanics_read;
                Spatial::AABB aabb;
                world_aabb_from_local_bounds(*transform_read, mechanics_dev.min_xyz, mechanics_dev.max_xyz, aabb);
                octree_entries.push_back({actor_handle,aabb});
                added_actors.insert(actor_handle);
                break;
            }
        }
        // 批量初始化 Actor 加载状态（单次加锁替代逐 Actor 加锁）
        // 当距离剔除关闭时，actor 视为始终已加载；否则从 Unloaded 开始由距离剔除系统管理
        {
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);
            const ActorLoadState initial_state = scene_state.cfg.enable_distance_culling
                                                     ? ActorLoadState::Unloaded
                                                     : ActorLoadState::Loaded;
            for (auto actor_handle : added_actors) {
                scene_state.actor_load_states.try_emplace(actor_handle, initial_state);
            }
        }

        Spatial::AABB root_aabb;
        if (!octree_entries.empty()) {
            root_aabb = octree_entries[0].bounds;
            for (const auto& entry : octree_entries) {
                root_aabb = root_aabb.merged(entry.bounds);
            }
            ktm::fvec3 extent = root_aabb.extent();

            //padding 添加10%的内边距
            float max_extent = std::max({extent[0], extent[1], extent[2]});
            float padding = max_extent * 0.1f;
            root_aabb = root_aabb.expanded(padding);
        }else {
            root_aabb.min = make_fvec3(-1.0f, -1.0f, -1.0f);
            root_aabb.max = make_fvec3(1.0f, 1.0f, 1.0f);
        }

        double rebuild_ms = 0.0;
        {
            const auto rebuild_begin = std::chrono::steady_clock::now();
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);
            scene_state.tree.rebuild(root_aabb,octree_entries);
            scene_state.actor_to_entry.clear();
            for (const auto& entry : octree_entries) {
                scene_state.actor_to_entry[entry.payload] = entry.bounds;
            }

            // 清理已经从场景中删除的Actor的状态
            std::unordered_set<Impl::Payload> current_actors(actor_handles.begin(),
                                                actor_handles.end());
            auto it = scene_state.actor_load_states.begin();
            while (it != scene_state.actor_load_states.end()) {
                if (!current_actors.count(it->first)) {
                    scene_state.loading_tasks.erase(it->first);
                    scene_state.unloading_tasks.erase(it->first);
                    scene_state.unload_retry_counts.erase(it->first);
                    scene_state.invisible_frames.erase(it->first);
                    it = scene_state.actor_load_states.erase(it);
                }else {
                    ++it;
                }
            }

            rebuild_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - rebuild_begin)
                             .count();
            std::lock_guard stats_lock(scene_state.stats_mutex);
            scene_state.stats.last_rebuild_ms = rebuild_ms;
        }
        // 发布粗筛碰撞候选对：SceneSystem 仅负责空间划分，不依赖物理系统
        {
            auto pairs = query_pairs(scene_handle);
            if (impl_->ctx && impl_->ctx->event_bus()) {
                impl_->ctx->event_bus()->publish(
                    Events::BroadphasePairsEvent{scene_handle, std::move(pairs)});
            }
        }

        std::vector<std::pair<ktm::fvec3,Math::Frustum>> cameras;
        std::unordered_set<Impl::Payload> visible_actors;
        double visible_query_ms_total = 0.0;
        for (std::uintptr_t camera_handle : camera_handles) {
            auto cam_read = camera_storage.try_acquire_read_nowait(camera_handle);
            if ( !cam_read.valid() ) continue;

            // 填充相机位置和视锥
            const CameraDevice& cam_dev = *cam_read;
            Math::Frustum frustum = Math::Frustum::from_camera(cam_dev);
            cameras.emplace_back(cam_dev.position,frustum);

            const auto visible_query_begin = std::chrono::steady_clock::now();
            std::vector<Impl::Payload> visible_for_camera = query_visible_for_camera(scene_handle,camera_handle);
            visible_query_ms_total += std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - visible_query_begin)
                                          .count();
            visible_actors.insert(visible_for_camera.begin(),visible_for_camera.end());
        }

        std::vector<Events::ActorUnloadRequestedEvent> pending_unloads;
        std::vector<Events::ActorLoadRequestedEvent> pending_loads;
        {
            // Phase 1: shared_lock — 收集候选、计算距离、决定转换（只读不写）
            std::shared_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);
            if (scene_state.cfg.enable_distance_culling && !cameras.empty()) {
                std::unordered_set<Impl::Payload> candidates;

                //仅收集预加载范围内的物体
                for (const auto& [cam_pos, _] : cameras) {
                    std::vector<Impl::Payload> sphere_results;
                    scene_state.tree.query_sphere(cam_pos, scene_state.cfg.preload_distance, sphere_results);
                    for (auto actor : sphere_results) {
                        candidates.insert(actor);
                    }
                }

                //保留所有非Unloaded状态的物体
                for (const auto& [actor,state] : scene_state.actor_load_states) {
                    if (state != ActorLoadState::Unloaded) {
                        candidates.insert(actor);
                    }
                }

                //仅处理候选物体
                for (auto actor : candidates) {
                    auto entry_it = scene_state.actor_to_entry.find(actor);
                    if (entry_it == scene_state.actor_to_entry.end()) continue;

                    const auto& aabb = entry_it->second;
                    auto state_it = scene_state.actor_load_states.find(actor);
                    if (state_it == scene_state.actor_load_states.end()) continue;
                    ActorLoadState state = state_it->second;  // 值拷贝，只读

                    // 计算物体到最近相机的欧氏距离
                    ktm::fvec3 center = aabb.center();
                    float min_distance = std::numeric_limits<float>::max();
                    for (const auto& [cam_pos,_] : cameras) {
                        min_distance = std::min(min_distance,ktm::distance(center,cam_pos));
                    }

                    // 状态机转换（只记录决策，不修改状态 — 由 Phase 2 统一应用）
                    switch (state) {
                        case ActorLoadState::Loaded:
                            if (min_distance > scene_state.cfg.unload_distance &&
                                !visible_actors.count(actor)) {
                                pending_unloads.push_back({scene_handle, actor});
                                }
                            break;

                        case ActorLoadState::Unloaded:
                            if (min_distance < scene_state.cfg.preload_distance) {
                                pending_loads.push_back({scene_handle, actor});
                            }
                            break;

                        default:
                            // 过渡状态不做任何操作，等待资源系统的完成事件
                            break;
                    }
                }
            }
        }
        // Phase 2: unique_lock — 应用状态转换（带 TOCTOU 重校验）
        if (!pending_unloads.empty() || !pending_loads.empty()) {
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);

            for (auto it = pending_unloads.begin(); it != pending_unloads.end(); ) {
                auto state_it = scene_state.actor_load_states.find(it->actor);
                if (state_it != scene_state.actor_load_states.end() &&
                    state_it->second == ActorLoadState::Loaded) {
                    state_it->second = ActorLoadState::Unloading;
                    CFW_LOG_NOTICE("[SceneSystem] Published unload request for actor {} (distance culling)",
                                  it->actor);
                    ++it;
                    } else {
                        it = pending_unloads.erase(it);  // 状态已被异步事件改变，取消此事件
                    }
            }

            for (auto it = pending_loads.begin(); it != pending_loads.end(); ) {
                auto state_it = scene_state.actor_load_states.find(it->actor);
                if (state_it != scene_state.actor_load_states.end() &&
                    state_it->second == ActorLoadState::Unloaded) {
                    state_it->second = ActorLoadState::Loading;
                    CFW_LOG_NOTICE("[SceneSystem] Published preload request for actor {} (distance culling)",
                                  it->actor);
                    ++it;
                    } else {
                        it = pending_loads.erase(it);
                    }
            }
        }
        for (const auto& evt : pending_unloads) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(evt);
        }
        for (const auto& evt : pending_loads) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(evt);
        }

        // 不可见帧计数与淘汰
        std::vector<Events::ActorEvictRequestedEvent> pending_evictions;
        {
            std::unique_lock lock(impl_->mtx);
            Impl::SceneState& scene_state = impl_->get_or_create(scene_handle);
            for (std::uintptr_t actor_handle : actor_handles) {
                auto state_it = scene_state.actor_load_states.find(actor_handle);
                if (state_it == scene_state.actor_load_states.end() ||
                    state_it->second != ActorLoadState::Loaded) {
                    continue;
                }

                if (!scene_state.actor_to_entry.count(actor_handle)) {
                    scene_state.invisible_frames.erase(actor_handle);
                    continue;
                }

                if ( visible_actors.count(actor_handle) ) {
                    scene_state.invisible_frames[actor_handle] = 0;
                } else {
                    uint32_t cnt = ++scene_state.invisible_frames[actor_handle];

                    if ( scene_state.cfg.invisible_frames_to_evict > 0 &&
                        cnt >= static_cast<uint32_t>(scene_state.cfg.invisible_frames_to_evict) ) {
                        pending_evictions.push_back({scene_handle, actor_handle});
                        CFW_LOG_NOTICE("GeometrySystem: Evict requested for actor {} (invisible {} frames)",
                               actor_handle, cnt);
                        scene_state.invisible_frames[actor_handle] = 0;
                    }
                }
            }
        }
        for (const auto& evt : pending_evictions) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(evt);
        }

        // 统计信息：使用读锁遍历，独立 stats_mutex 写入，减少主锁竞争
        {
            std::shared_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);

            std::size_t loaded = 0, loading = 0, unloading = 0, unloaded = 0;
            for (const auto& [actor_handle, state] : scene_state.actor_load_states) {
                switch (state) {
                    case ActorLoadState::Loaded:    loaded++; break;
                    case ActorLoadState::Loading:   loading++; break;
                    case ActorLoadState::Unloading: unloading++; break;
                    case ActorLoadState::Unloaded:  unloaded++; break;
                }
            }

            std::lock_guard stats_lock(scene_state.stats_mutex);
            scene_state.stats.actor_total    = actor_handles.size();
            scene_state.stats.actor_visible  = visible_actors.size();
            scene_state.stats.octree_entries = octree_entries.size();
            scene_state.stats.last_query_ms = visible_query_ms_total;
            scene_state.stats.actor_loaded    = loaded;
            scene_state.stats.actor_loading   = loading;
            scene_state.stats.actor_unloading = unloading;
            scene_state.stats.actor_unloaded  = unloaded;
        }
        auto scene_write = scene_storage.try_acquire_write(scene_handle);
        if (scene_write.valid()) {
            SceneDevice& scene_dev_write = *scene_write;
            scene_dev_write.min_world = root_aabb.min;
            scene_dev_write.max_world = root_aabb.max;
            scene_dev_write.center_world = root_aabb.center();
        }
    }
}

void GeometrySystem::shutdown() {
    CFW_LOG_NOTICE("GeometrySystem: Shutting down...");

    // 取消所有事件订阅
    if (impl_->ctx && impl_->ctx->event_bus()) {
        for (Kernel::EventId subscription_id : impl_->event_subscriptions) {
            impl_->ctx->event_bus()->unsubscribe(subscription_id);
        }
    }
    impl_->event_subscriptions.clear();

    std::unique_lock lock(impl_->mtx);
    std::vector<std::future<std::uint64_t>> load_futures;
    std::vector<std::future<bool>> unload_futures;
    for (auto& [scene,state] : impl_->scenes) {
        for (auto& [actor,future] : state.loading_tasks) {
            if (future.valid()) {
                load_futures.push_back(std::move(future));
            }
        }
        for (auto& [actor, future] : state.unloading_tasks) {
            if (future.valid()) {
                unload_futures.push_back(std::move(future));
            }
        }
    }
    lock.unlock();

    for (auto& f : load_futures) {
        if ( f.valid() ) {
            f.wait();
        }
    }
    for (auto& f : unload_futures) {
        if ( f.valid() ) {
            f.wait();
        }
    }
    lock.lock();
    for (auto& [scene,state] : impl_->scenes) {
        state.loading_tasks.clear();
        state.unloading_tasks.clear();
    }

    impl_->scenes.clear();
    impl_->offline_actors.clear();

    // 显式释放共享占位纹理，确保在 GPU device 仍存活时析构 HardwareImage
    impl_->shared_placeholder_texture.reset();
}

// ============================================================================
// 配置
// ============================================================================

void GeometrySystem::set_visibility_config(std::uintptr_t scene, SceneVisibilityConfig cfg) {
    std::unique_lock lock(impl_->mtx);
    impl_->get_or_create(scene).cfg = cfg;
}

void GeometrySystem::set_distance_config(std::uintptr_t scene, float unload_dist,
                                    float preload_dist,bool enable) {
    std::unique_lock lock(impl_->mtx);
    auto& scene_state = impl_->get_or_create(scene);
    scene_state.cfg.enable_distance_culling = enable;
    scene_state.cfg.unload_distance = unload_dist;
    scene_state.cfg.preload_distance = preload_dist;
}

// ============================================================================
// 私有事件处理
// ============================================================================

void GeometrySystem::on_load_completed(const Events::ActorLoadCompletedEvent& event) {
    ActorLoadState old_state = ActorLoadState::Unloaded;
    {
        std::unique_lock lock(impl_->mtx);
        auto scene_it = impl_->scenes.find(event.scene);
        if (scene_it != impl_->scenes.end()) {
            auto& state_map = scene_it->second.actor_load_states;
            auto actor_it = state_map.find(event.actor);
            if (actor_it != state_map.end() && actor_it->second == ActorLoadState::Loading) {
                old_state = actor_it->second;
                actor_it->second = ActorLoadState::Loaded;
                impl_->offline_actors[event.actor] = false;
            }
        }
    } // 释放锁后发布事件
    if (old_state == ActorLoadState::Loading) {
        CFW_LOG_NOTICE("GeometrySystem: Actor {} (scene: {}) load completed", event.actor, event.scene);
        impl_->ctx->event_bus()->publish(event);
    }
}

void GeometrySystem::on_unload_completed(const Events::ActorUnloadCompletedEvent& event) {
    ActorLoadState old_state = ActorLoadState::Loaded;
    {
        std::unique_lock lock(impl_->mtx);
        auto scene_it = impl_->scenes.find(event.scene);
        if (scene_it == impl_->scenes.end()) return;

        auto& state_map = scene_it->second.actor_load_states;
        auto actor_it = state_map.find(event.actor);
        if (actor_it != state_map.end() && actor_it->second == ActorLoadState::Unloading) {
            old_state = actor_it->second;
            actor_it->second = ActorLoadState::Unloaded;
            impl_->offline_actors[event.actor] = false;
        }
    }
    if (old_state == ActorLoadState::Unloading) {
        CFW_LOG_NOTICE("GeometrySystem: Actor {} (scene: {}) unload completed", event.actor, event.scene);
        impl_->ctx->event_bus()->publish(event);
    }
}

// ============================================================================
// GPU 资源释放（unload 时由 process_async_tasks 调用）
// ============================================================================

// ============================================================================
// release_actor_gpu_resources
// 功能：释放指定 actor 占用的全部 GPU 资源（显存中的顶点/索引缓冲和纹理）
// 调用时机：process_async_tasks() 中处理 ActorUnloadCompletedEvent 时
// 注意：只清理 GPU 端资源，不删除 SharedDataHub 中的存储槽位
// ============================================================================
void GeometrySystem::release_actor_gpu_resources(std::uintptr_t actor) {
    // ---- 第 0 步：获取全局数据中心单例 ----
    // SharedDataHub 是所有系统共享的数据仓库，存 Actor/Profile/Geometry 等设备数据
    auto& hub = SharedDataHub::instance();

    // ---- 第 1 步：以只读模式获取 actor 数据 ----
    // try_acquire_read 返回一个 RAII 读锁守卫，离开作用域自动释放
    auto actor_read = hub.actor_storage().try_acquire_read(actor);
    if (!actor_read.valid()) return;  // actor 句柄无效（可能已被销毁），直接返回

    // ---- 第 2 步：用 visited 集合去重 ----
    // 一个 actor 的多个 profile 可能共享同一个 geometry（例如 optics 和 mechanics 引用同一几何体）
    // 用 unordered_set 记录已处理的 geometry，避免重复释放
    std::unordered_set<std::uintptr_t> visited_geometry_handles;

    // ---- 第 3 步：遍历 actor 身上每个 Profile ----
    // Profile 是"配件槽位"——它聚合了 optics/mechanics/geometry/acoustics 的句柄
    for (auto profile_handle : actor_read->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;  // profile 句柄已失效

        // ---- 第 4 步：从 Profile 的 4 条路径收集 geometry 句柄 ----
        // 路径 A：Profile 自身直接挂载的 geometry_handle
        // 路径 B：Profile → OpticsDevice → geometry_handle（光学设备可能引用几何体）
        // 路径 C：Profile → MechanicsDevice → geometry_handle（力学设备必然引用几何体，最常用）
        // 路径 D：Profile → AcousticsDevice → geometry_handle（声学设备可能引用几何体）
        std::vector<std::uintptr_t> geom_handles;

        // 路径 A：Profile 自身的 geometry 直连
        if (profile->geometry_handle != 0) {
            geom_handles.push_back(profile->geometry_handle);
        }
        // 路径 B：OpticsDevice（视觉渲染设备）→ geometry
        if (profile->optics_handle != 0) {
            if (auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle)) {
                if (optics->geometry_handle != 0) {
                    geom_handles.push_back(optics->geometry_handle);
                }
            }
        }
        // 路径 C：MechanicsDevice（物理/变换设备）→ geometry（最常用的路径）
        if (profile->mechanics_handle != 0) {
            if (auto mech = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle)) {
                if (mech->geometry_handle != 0) {
                    geom_handles.push_back(mech->geometry_handle);
                }
            }
        }
        // 路径 D：AcousticsDevice（声学设备）→ geometry
        if (profile->acoustics_handle != 0) {
            if (auto acoustics = hub.acoustics_storage().try_acquire_read(profile->acoustics_handle)) {
                if (acoustics->geometry_handle != 0) {
                    geom_handles.push_back(acoustics->geometry_handle);
                }
            }
        }

        // ---- 第 5 步：对每个收集到的 geometry 释放 GPU 资源 ----
        for (auto geom_handle : geom_handles) {
            // visited_geometry_handles.insert() 返回 pair<iter, bool>
            // .second == false 表示已存在 → 跳过，避免重复处理
            if (!visited_geometry_handles.insert(geom_handle).second) continue;

            // ---- 第 5.1 步：统计该 geometry 有多少个 mesh（子网格）----
            // 一个 GeometryDevice 可能包含多个 MeshDevice（例如一个模型有多个材质）
            uint32_t mesh_count = 0;
            if (auto geom_read = hub.geometry_storage().try_acquire_read(geom_handle)) {
                mesh_count = static_cast<uint32_t>(geom_read->mesh_handles.size());
            } else {
                continue;  // geometry 句柄已失效
            }

            // ---- 第 5.2 步：清理 LOD 缓存 ----
            // 每个 mesh 可能在 upload_lod_from_scene_data() 中创建了多级 LOD GPU 缓冲
            // make_lod_key(geom_handle, i) 生成唯一键：(geometry_handle << 32) | mesh_index
            {
                std::unique_lock lod_lock(impl_->lod_cache_mutex);  // 独占锁（写操作）
                for (uint32_t i = 0; i < mesh_count; ++i) {
                    impl_->lod_cache.erase(Impl::make_lod_key(geom_handle, i));
                }
            }  // lod_lock 在此析构，自动释放互斥锁

            // ---- 第 5.3 步：销毁 mesh_handles 中的 GPU 缓冲 ----
            // mesh_handles 是 vector<MeshDevice>，每个 MeshDevice 内含：
            //   vertexBuffer / indexBuffer（渲染用）
            //   vertexStorageBuffer / indexStorageBuffer（Compute Shader 用）
            //   textureBuffer（纹理）
            // clear() 触发每个元素的析构 → HardwareBuffer/HardwareImage 析构 → GPU 显存归还
            // 注意：model_resource_handle 保留不删，以便 reload 时能找到模型资源条目
            if (auto geom_write = hub.geometry_storage().try_acquire_write(geom_handle)) {
                geom_write->mesh_handles.clear();
            }  // geom_write 析构时自动释放写锁

            // ---- 第 5.4 步：日志 ----
            CFW_LOG_NOTICE("[GeometrySystem] Released GPU resources for geometry {}, "
                           "{} mesh(es), actor {}",
                           geom_handle,   // geometry 在 SharedDataHub 中的句柄地址
                           mesh_count,    // 释放了多少个 mesh 的 GPU 缓冲
                           actor);        // 所属 actor 句柄
        }
    }
}

// ============================================================================
// rebuild_actor_gpu_resources
// 功能：释放后重新加载 actor 时，重建全部 GPU 资源（顶点/索引缓冲 + 纹理）
// 调用时机：process_async_tasks() 检测到 load 任务完成后，发布事件前
// 参数：
//   actor — actor 句柄（SharedDataHub 中的地址）
//   rid   — 资源 UID（ResourceManager 分配的唯一标识，由 import_async 返回）
// 说明：这个函数是 unload → reload 生命周期中"重建"环节的核心
// ============================================================================
void GeometrySystem::rebuild_actor_gpu_resources(std::uintptr_t actor, std::uint64_t rid) {
    // ---- 第 0 步：获取两个全局单例 ----
    // SharedDataHub：管理所有系统共享的设备数据（actor/profile/geometry 等）
    auto& hub = SharedDataHub::instance();
    // ResourceManager：管理所有资源文件（Scene/Image 等），通过 UID 查找
    auto& resource_manager = Resource::ResourceManager::get_instance();

    // ---- 第 1 步：读取 actor 数据 ----
    auto actor_read = hub.actor_storage().try_acquire_read(actor);
    if (!actor_read.valid()) return;  // actor 句柄无效

    // ---- 第 2 步：去重集合（与 release 函数逻辑相同）----
    // 多个 profile 可能引用同一 geometry，用 set 防止重复重建
    std::unordered_set<std::uintptr_t> visited_geometry_handles;

    // ---- 第 3 步：遍历 actor 的所有 profile ----
    for (auto profile_handle : actor_read->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;

        // ---- 第 4 步：4 条路径收集 geometry 句柄（同 release 逻辑）----
        // 路径 A：Profile 直连 geometry
        // 路径 B：Profile → OpticsDevice → geometry
        // 路径 C：Profile → MechanicsDevice → geometry（最常用）
        // 路径 D：Profile → AcousticsDevice → geometry
        std::vector<std::uintptr_t> geom_handles;

        // 路径 A：profile 自身的 geometry_handle
        if (profile->geometry_handle != 0) {
            geom_handles.push_back(profile->geometry_handle);
        }
        // 路径 B：光学设备 → geometry
        if (profile->optics_handle != 0) {
            if (auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle)) {
                if (optics->geometry_handle != 0) {
                    geom_handles.push_back(optics->geometry_handle);
                }
            }
        }
        // 路径 C：力学/物理设备 → geometry（渲染对象的主要路径）
        if (profile->mechanics_handle != 0) {
            if (auto mech = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle)) {
                if (mech->geometry_handle != 0) {
                    geom_handles.push_back(mech->geometry_handle);
                }
            }
        }
        // 路径 D：声学设备 → geometry
        if (profile->acoustics_handle != 0) {
            if (auto acoustics = hub.acoustics_storage().try_acquire_read(profile->acoustics_handle)) {
                if (acoustics->geometry_handle != 0) {
                    geom_handles.push_back(acoustics->geometry_handle);
                }
            }
        }

        // ---- 第 5 步：对每个 geometry 重建 GPU 资源 ----
        for (auto geom_handle : geom_handles) {
            // 去重：同一 geometry 只处理一次
            if (!visited_geometry_handles.insert(geom_handle).second) continue;

            // ---- 第 5.1 步：判断是否需要重建 ----
            // model_resource_handle 是 SharedDataHub 中 ModelResource 条目的句柄
            // release() 时保留了它（未置零），通过它找到对应的模型资源条目
            // mesh_handles 在 release() 时已 clear()，所以 empty() == true 表示需要重建
            // 初始加载时 Python API 已填充 mesh_handles，此时不为空 → 无需重建
            std::uintptr_t model_res_handle = 0;  // ModelResource 句柄
            bool needs_rebuild = false;            // 是否需要重建 GPU 缓冲
            {
                auto geom_read = hub.geometry_storage().try_acquire_read(geom_handle);
                if (!geom_read) continue;  // geometry 已失效
                model_res_handle = geom_read->model_resource_handle;
                // 关键判断：mesh_handles 为空 → 被 release() 清理过 → 需要重建
                //          mesh_handles 不为空 → 初始加载已完成 → 无需重建
                needs_rebuild = geom_read->mesh_handles.empty();
            }  // geom_read 析构，释放读锁

            // ---- 第 5.2 步：更新 ModelResource 中的 model_id ----
            // reload 时 import_async 可能分配新的资源 UID，必须更新
            // 无论是否需要 rebuild 都要更新，确保后续 LOD 上传能正确查找到 Scene 数据
            if (model_res_handle != 0) {
                if (auto model_res = hub.model_resource_storage().try_acquire_write(model_res_handle)) {
                    model_res->model_id = rid;  // 写入新的资源 UID
                }
            }

            // ---- 第 5.3 步：如果不需要重建，跳过此 geometry ----
            // 初始加载场景：mesh_handles 已在 Python API 层创建完毕
            if (!needs_rebuild) {
                continue;  // 无需重建，直接处理下一个 geometry
            }

            // ---- 第 5.4 步：从 ResourceManager 获取导入的 Scene 数据 ----
            // rid 是 import_async 完成后返回的资源唯一标识
            // Scene 资源包含完整的模型数据：顶点/索引/材质/纹理/LOD 等
            auto scene_read = resource_manager.acquire_read<Resource::Scene>(rid);
            if (!scene_read.valid()) {
                CFW_LOG_ERROR("[GeometrySystem] Failed to acquire Scene resource for rid={}", rid);
                continue;  // 资源无效，跳过
            }
            auto& scene = *scene_read;  // 解引用读锁守卫，获得 Scene 数据引用

            // ================================================================
            // 阶段 A：创建 MeshDevice 数组
            // 为 Scene 中的每个 mesh 创建 GPU 缓冲（顶点/索引/纹理）
            // 分两步：先创建所有 HardwareBuffer/HardwareImage，再批量上传纹理数据
            // ================================================================
            std::vector<MeshDevice> mesh_devices;                     // 输出：新的 mesh 设备数组
            mesh_devices.reserve(scene.data.meshes.size());            // 预分配内存，避免 realloc

            // ---- 待上传纹理列表（第一阶段收集，第二阶段批量执行）----
            // 纹理上传涉及 GPU 传输，批量处理比逐个处理效率高
            struct PendingTextureUpload {
                std::uint32_t mesh_idx;               // 对应 mesh_devices 中的索引
                HardwareImage* texture;               // 指向已创建的 HardwareImage 对象
                std::vector<unsigned char> rgba_data; // 纹理像素数据（RGBA 格式）
                unsigned char* data_ptr;              // 指向 rgba_data 中数据的指针
            };
            std::vector<PendingTextureUpload> pending_uploads;
            pending_uploads.reserve(scene.data.meshes.size());

            // ---- 创建共享的 1x1 白色占位纹理 ----
            // 用于无纹理的 mesh，确保渲染管线始终有纹理可采样
            // 生命周期由 Impl::shared_placeholder_texture 管理，shutdown() 中显式释放
            // 避免 static 局部变量在 GPU device 析构后才析构导致 crash
            if (!impl_->shared_placeholder_texture) {
                HardwareImageCreateInfo placeholder_info{};
                placeholder_info.width        = 1;                       // 1 像素宽
                placeholder_info.height       = 1;                       // 1 像素高
                placeholder_info.format       = ImageFormat::RGBA8_SRGB; // sRGB 色彩空间
                placeholder_info.usage        = ImageUsage::SampledImage; // 可作为着色器采样源
                placeholder_info.arrayLayers  = 1;                       // 无数组层
                placeholder_info.mipLevels    = 1;                       // 无 mipmap

                static const unsigned char white_pixel[4] = {255, 255, 255, 255};  // 不透明白色
                impl_->shared_placeholder_texture = std::make_unique<HardwareImage>(placeholder_info);
                HardwareExecutor temp_executor;                        // 临时命令执行器
                // 将白色像素数据拷贝到 GPU 纹理，提交执行
                temp_executor << impl_->shared_placeholder_texture->copyFrom(white_pixel)
                              << temp_executor.commit();
            }

            // ---- 第一阶段：遍历所有 mesh，创建 GPU 缓冲 ----
            for (std::uint32_t mesh_idx = 0; mesh_idx < scene.data.meshes.size(); ++mesh_idx) {
                const auto& mesh = scene.data.meshes[mesh_idx];  // 当前 mesh 的 CPU 端数据
                MeshDevice dev{};  // 零初始化 MeshDevice（所有句柄为 0/null）

                // ---- 创建顶点/索引缓冲（4 个）----
                // vertexBuffer / indexBuffer：渲染管线使用（Vertex Shader 读取）
                // vertexStorageBuffer / indexStorageBuffer：Compute Shader 使用（可读写）
                // get_mesh_vertices() 返回 meshopt 优化后的顶点数组
                // get_mesh_indices() 返回 meshopt 优化后的索引数组
                dev.vertexBuffer        = HardwareBuffer(scene.get_mesh_vertices(mesh_idx), BufferUsage::VertexBuffer);
                dev.indexBuffer         = HardwareBuffer(scene.get_mesh_indices(mesh_idx),  BufferUsage::IndexBuffer);
                dev.vertexStorageBuffer = HardwareBuffer(scene.get_mesh_vertices(mesh_idx), BufferUsage::StorageBuffer);
                dev.indexStorageBuffer  = HardwareBuffer(scene.get_mesh_indices(mesh_idx),  BufferUsage::StorageBuffer);

                // ---- 材质索引 ----
                // material_index 指向 scene.data.materials 数组
                // InvalidIndex（最大值）表示无材质 → 降级为 0（使用默认材质）
                dev.materialIndex = (mesh.material_index != Resource::InvalidIndex)
                                        ? mesh.material_index                    // 有效材质索引
                                        : 0;                                    // 降级为默认材质

                // ---- 读取材质颜色（base_color：RGBA 漫反射颜色）----
                if (mesh.material_index != Resource::InvalidIndex &&
                    mesh.material_index < scene.data.materials.size()) {
                    dev.materialColor = scene.data.materials[mesh.material_index].base_color;
                }

                // ---- 纹理处理 ----
                bool texture_created = false;               // 标记：是否已创建纹理
                HardwareImageCreateInfo create_info{};       // 纹理创建参数（零初始化）

                // 检查是否有有效材质和纹理
                if (mesh.material_index != Resource::InvalidIndex &&
                    mesh.material_index < scene.data.materials.size()) {
                    // 从材质中获取 albedo（漫反射）纹理 ID
                    auto texture_id = scene.data.materials[mesh.material_index].albedo_texture;

                    if (texture_id != Resource::InvalidTextureId) {
                        // 尝试从资源管理器获取纹理图像数据
                        auto texture_data = resource_manager.acquire_read<Resource::Image>(texture_id);
                        if (texture_data && texture_data->get_data() != nullptr) {
                            const int tex_width    = texture_data->get_width();     // 纹理宽度（像素）
                            const int tex_height   = texture_data->get_height();    // 纹理高度（像素）
                            const int tex_channels = texture_data->get_channels();  // 颜色通道数（1/3/4）

                            if (tex_width > 0 && tex_height > 0 && tex_channels > 0) {
                                // ========================================
                                // 分支 A：压缩纹理（BC1/BC3/ASTC）
                                // ========================================
                                if (texture_data->is_compressed()) {
                                    // 获取压缩后的数据（GPU 可直接使用的格式）
                                    const auto& compressed = texture_data->get_compressed_data();
                                    create_info.width        = tex_width;
                                    create_info.height       = tex_height;
                                    create_info.usage        = ImageUsage::SampledImage;
                                    create_info.arrayLayers  = 1;
                                    create_info.mipLevels    = 1;

                                    // 根据压缩格式设置对应的 GPU 图像格式
                                    if (compressed.format == Resource::CompressedData::Format::BC1) {
                                        create_info.format = ImageFormat::BC1_RGB_SRGB;     // DXT1，无 alpha
                                    } else if (compressed.format == Resource::CompressedData::Format::BC3) {
                                        create_info.format = ImageFormat::BC3_RGBA_SRGB;    // DXT5，含 alpha
                                    } else if (compressed.format == Resource::CompressedData::Format::ASTC_4x4) {
                                        create_info.format = ImageFormat::ASTC_4x4_SRGB;    // 移动端常用
                                    }

                                    // 将压缩数据加入待上传队列
                                    PendingTextureUpload upload{mesh_idx, nullptr, {}, nullptr};
                                    upload.rgba_data.assign(compressed.data.begin(), compressed.data.end());
                                    upload.data_ptr = upload.rgba_data.data();

                                    // 创建 GPU 纹理对象（此时尚未上传像素数据）
                                    dev.textureBuffer = HardwareImage(create_info);
                                    upload.texture = &dev.textureBuffer;
                                    pending_uploads.push_back(std::move(upload));
                                    texture_created = true;
                                }
                                // ========================================
                                // 分支 B：未压缩纹理（RGBA 像素数据）
                                // ========================================
                                else {
                                    create_info.width        = tex_width;
                                    create_info.height       = tex_height;
                                    create_info.format       = ImageFormat::RGBA8_SRGB;   // 统一转为 RGBA8
                                    create_info.usage        = ImageUsage::SampledImage;
                                    create_info.arrayLayers  = 1;
                                    create_info.mipLevels    = 1;

                                    unsigned char* src_data = texture_data->get_data();  // 原始像素数据指针
                                    PendingTextureUpload upload{mesh_idx, nullptr, {}, nullptr};

                                    // ---- 根据通道数转换为 RGBA ----
                                    if (tex_channels == 4) {
                                        // RGBA：直接拷贝，无需转换
                                        upload.rgba_data.assign(src_data,
                                            src_data + static_cast<size_t>(tex_width) * tex_height * 4);
                                        upload.data_ptr = upload.rgba_data.data();
                                    } else if (tex_channels == 3) {
                                        // RGB → RGBA：补充 alpha=255（完全不透明）
                                        upload.rgba_data.resize(static_cast<size_t>(tex_width) * tex_height * 4);
                                        for (int i = 0; i < tex_width * tex_height; ++i) {
                                            upload.rgba_data[i * 4 + 0] = src_data[i * 3 + 0];  // R
                                            upload.rgba_data[i * 4 + 1] = src_data[i * 3 + 1];  // G
                                            upload.rgba_data[i * 4 + 2] = src_data[i * 3 + 2];  // B
                                            upload.rgba_data[i * 4 + 3] = 255;                  // A=不透明
                                        }
                                        upload.data_ptr = upload.rgba_data.data();
                                    } else if (tex_channels == 1) {
                                        // 灰度 → RGBA：R=G=B=灰度值, A=255
                                        upload.rgba_data.resize(static_cast<size_t>(tex_width) * tex_height * 4);
                                        for (int i = 0; i < tex_width * tex_height; ++i) {
                                            upload.rgba_data[i * 4 + 0] = src_data[i];  // R=灰度
                                            upload.rgba_data[i * 4 + 1] = src_data[i];  // G=灰度
                                            upload.rgba_data[i * 4 + 2] = src_data[i];  // B=灰度
                                            upload.rgba_data[i * 4 + 3] = 255;          // A=不透明
                                        }
                                        upload.data_ptr = upload.rgba_data.data();
                                    }

                                    // 如果有有效数据，创建 GPU 纹理并加入上传队列
                                    if (upload.data_ptr != nullptr) {
                                        dev.textureBuffer = HardwareImage(create_info);   // 创建 GPU 纹理对象
                                        upload.texture = &dev.textureBuffer;              // 指向刚创建的纹理
                                        pending_uploads.push_back(std::move(upload));    // 入队等待批量上传
                                        texture_created = true;
                                    }
                                }
                            }
                        }
                    }
                }

                // ---- 无纹理的兜底：使用共享白色占位纹理 ----
                // 确保每个 mesh 都有纹理句柄，避免渲染时空指针
                if (!texture_created) {
                    dev.textureBuffer = *impl_->shared_placeholder_texture;  // 拷贝共享纹理句柄
                }

                // ---- 将构建好的 MeshDevice 加入数组 ----
                mesh_devices.emplace_back(std::move(dev));
            }  // 第一阶段结束：所有 mesh 的 GPU 缓冲已创建，纹理像素尚未上传

            // ================================================================
            // 阶段 B：批量上传纹理像素到 GPU
            // 使用 HardwareExecutor 执行异步 GPU 传输
            // 每 32 个纹理一批，平衡内存占用和批次开销
            // ================================================================
            if (!pending_uploads.empty()) {
                constexpr size_t kBatchSize = 32;  // 每批最多 32 个纹理
                for (size_t batch_start = 0; batch_start < pending_uploads.size(); batch_start += kBatchSize) {
                    size_t batch_end = std::min(batch_start + kBatchSize, pending_uploads.size());

                    HardwareExecutor batch_executor;  // GPU 命令执行器
                    // 将本批次所有纹理的 copyFrom 命令加入执行器
                    for (size_t i = batch_start; i < batch_end; ++i) {
                        auto& upload = pending_uploads[i];
                        HardwareImage& tex = mesh_devices[upload.mesh_idx].textureBuffer;
                        batch_executor << tex.copyFrom(upload.data_ptr);  // 将像素数据拷贝到 GPU
                    }
                    // 提交所有命令到 GPU 队列并执行
                    batch_executor << batch_executor.commit();
                    batch_executor.waitForDeferredResources();  // 等待本批次传输完成
                }
            }

            // ================================================================
            // 阶段 C：写回 GeometryDevice
            // 将重建好的 mesh_handles 写回 SharedDataHub
            // ================================================================

            // ---- 先清理旧 LOD 缓存 ----
            // mesh_handles 已经重建（新的 GPU 缓冲句柄），旧 LOD 条目指向已销毁的缓冲
            // 必须清除，否则下一帧 upload_lod_from_scene_data() 会检测到 mismatched handles 并重建
            {
                std::unique_lock lod_lock(impl_->lod_cache_mutex);  // 独占锁
                for (uint32_t i = 0; i < static_cast<uint32_t>(mesh_devices.size()); ++i) {
                    impl_->lod_cache.erase(Impl::make_lod_key(geom_handle, i));
                }
            }  // lod_lock 析构

            // ---- 将新 mesh_handles 写入 GeometryDevice ----
            if (auto geom_write = hub.geometry_storage().try_acquire_write(geom_handle)) {
                geom_write->mesh_handles = std::move(mesh_devices);  // move 语义，避免拷贝
            }  // geom_write 析构，释放写锁

            // ---- 日志：记录重建完成 ----
            CFW_LOG_NOTICE("[GeometrySystem] Rebuilt GPU resources for geometry {}, "
                           "{} mesh(es), actor {}, rid={}",
                           geom_handle,                // geometry 句柄
                           scene.data.meshes.size(),   // 重建的 mesh 数量
                           actor,                      // 所属 actor
                           rid);                       // 资源 UID
        }
    }
}

// ============================================================================
// 异步资源任务处理
// ============================================================================

void GeometrySystem::process_async_tasks() {
    auto& hub = SharedDataHub::instance();
    auto& actor_storage = hub.actor_storage();

    struct CompletedLoadTask {
        std::uintptr_t scene_handle;
        std::uintptr_t actor;
        std::uint64_t rid;
    };
    struct CompletedUnloadTask {
        std::uintptr_t scene_handle;
        std::uintptr_t actor;
        bool success;
    };
    struct DeferredLoadTask {
        std::uintptr_t scene_handle;
        std::uintptr_t actor;
        std::future<std::uint64_t> future;
    };
    struct DeferredUnloadTask {
        std::uintptr_t scene_handle;
        std::uintptr_t actor;
        std::future<bool> future;
    };

    std::vector<CompletedLoadTask> completed_loads;
    std::vector<CompletedUnloadTask> completed_unloads;
    std::vector<DeferredLoadTask> deferred_loads;
    std::vector<DeferredUnloadTask> deferred_unloads;
    {
        std::unique_lock lock(impl_->mtx);
        for (auto& [scene_handle, scene_state] : impl_->scenes) {
            auto load_it = scene_state.loading_tasks.begin();
            while (load_it != scene_state.loading_tasks.end()) {
                if (load_it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    deferred_loads.push_back({scene_handle, load_it->first, std::move(load_it->second)});
                    load_it = scene_state.loading_tasks.erase(load_it);
                } else {
                    ++load_it;
                }
            }

            auto unload_it = scene_state.unloading_tasks.begin();
            while (unload_it != scene_state.unloading_tasks.end()) {
                if (unload_it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    deferred_unloads.push_back({scene_handle, unload_it->first, std::move(unload_it->second)});
                    unload_it = scene_state.unloading_tasks.erase(unload_it);
                } else {
                    ++unload_it;
                }
            }
        }
    }

    // 无锁阶段调用 future.get()，处理结果
    for (auto& task : deferred_loads) {
        completed_loads.push_back({task.scene_handle, task.actor, task.future.get()});
    }
    for (auto& task : deferred_unloads) {
        completed_unloads.push_back({task.scene_handle, task.actor, task.future.get()});
    }

    for (const auto& task : completed_loads) {
        // 检查加载是否已被 on_unload_requested 取消（状态被改为非 Loading）
        {
            std::shared_lock lock(impl_->mtx);
            auto scene_it = impl_->scenes.find(task.scene_handle);
            if (scene_it != impl_->scenes.end()) {
                auto state_it = scene_it->second.actor_load_states.find(task.actor);
                if (state_it == scene_it->second.actor_load_states.end() ||
                    state_it->second != ActorLoadState::Loading) {
                    CFW_LOG_DEBUG("[SceneSystem] Actor {} load completed but was cancelled — skipping", task.actor);
                    continue;
                }
            }
        }

        if (task.rid != Resource::IResource::INVALID_UID) {
            // 重建 GPU 资源（mesh_handles + 纹理），恢复 model_resource_handle
            // 必须在发布 ActorLoadCompletedEvent 之前完成，
            // 以保证事件订阅者（渲染线程等）能读到有效的 GPU 缓冲
            rebuild_actor_gpu_resources(task.actor, task.rid);

            impl_->ctx->event_bus()->publish(Events::ActorLoadCompletedEvent{task.scene_handle,task.actor});
            CFW_LOG_DEBUG("[SceneSystem] Actor {} loaded (resource: {})", task.actor, task.rid);
        }else {
            CFW_LOG_ERROR("[SceneSystem] Failed to load actor {}", task.actor);
            // 加载失败，回滚到Unloaded状态
            {
                std::unique_lock lock(impl_->mtx);
                auto scene_it = impl_->scenes.find(task.scene_handle);
                if (scene_it != impl_->scenes.end()) {
                    scene_it->second.actor_load_states[task.actor] = ActorLoadState::Unloaded;
                }
            }
            impl_->ctx->event_bus()->publish(Events::ActorUnloadCompletedEvent{task.scene_handle, task.actor});
        }
    }

    std::vector<CompletedUnloadTask> failed_unloads;
    for (const auto& task : completed_unloads) {
        if (task.success) {
            // 释放 actor 关联的 GPU 资源（HardwareBuffer / HardwareImage）
            release_actor_gpu_resources(task.actor);

            {
                std::unique_lock lock(impl_->mtx);
                auto scene_it = impl_->scenes.find(task.scene_handle);
                if (scene_it != impl_->scenes.end()) {
                    scene_it->second.unload_retry_counts.erase(task.actor);
                }
            }
            impl_->ctx->event_bus()->publish(Events::ActorUnloadCompletedEvent{task.scene_handle, task.actor});
            CFW_LOG_DEBUG("[SceneSystem] Actor {} unloaded", task.actor);
        } else {
            // 卸载失败，保存到列表中后续处理重试
            failed_unloads.push_back(task);
        }
    }

    //卸载失败重试
    if (!failed_unloads.empty()) {
        std::vector<Events::ActorUnloadCompletedEvent> deferred_events;
        {
            std::unique_lock lock(impl_->mtx);
            for (const auto& task : failed_unloads) {
                auto scene_it = impl_->scenes.find(task.scene_handle);
                if (scene_it == impl_->scenes.end()) {
                    continue;
                }
                auto& scene_state = scene_it->second;

                auto state_it = scene_state.actor_load_states.find(task.actor);
                if (state_it == scene_state.actor_load_states.end()) {
                    continue;
                }

                int& retry_count = scene_state.unload_retry_counts[task.actor];
                CFW_LOG_WARNING("[SceneSystem] Actor 0x%lx unload delayed (resource in use), retry %d/10",
                               (unsigned long)task.actor, retry_count + 1);

                if (++retry_count >= 10) {
                    CFW_LOG_ERROR("[SceneSystem] Actor 0x%lx unload failed after 10 retries, resource is permanently in use — reverting to Loaded",
                                 (unsigned long)task.actor);
                    scene_state.unload_retry_counts.erase(task.actor);
                    scene_state.actor_load_states[task.actor] = ActorLoadState::Loaded;
                } else {
                    auto actor_read = actor_storage.try_acquire_read(task.actor);
                    if (actor_read.valid()) {
                        if (!actor_read->model_path.empty()) {
                            auto normalized = actor_read->model_path.is_relative()
                                ? std::filesystem::absolute(actor_read->model_path)
                                : actor_read->model_path;
                            std::error_code ec;
                            normalized = std::filesystem::weakly_canonical(normalized, ec);
                            if (ec) normalized = actor_read->model_path;
                            auto rid = Resource::IResource::generate_uid(normalized);
                            scene_state.unloading_tasks[task.actor] =
                                Resource::ResourceManager::get_instance().remove_cache_async(rid);
                        } else {
                            CFW_LOG_WARNING("[SceneSystem] Actor 0x%lx model path empty, mark as unloaded",
                                           (unsigned long)task.actor);
                            scene_state.unload_retry_counts.erase(task.actor);
                            scene_state.actor_load_states[task.actor] = ActorLoadState::Unloaded;
                            deferred_events.push_back({task.scene_handle, task.actor});
                        }
                    } else {
                        CFW_LOG_WARNING("[SceneSystem] Actor 0x%lx handle invalid, clean up all states",
                                       (unsigned long)task.actor);
                        scene_state.unload_retry_counts.erase(task.actor);
                        scene_state.actor_load_states.erase(task.actor);
                        impl_->offline_actors.erase(task.actor);
                    }
                }
            }
        }
        for (const auto& evt : deferred_events) {
            impl_->ctx->event_bus()->publish(evt);
        }
    }
}

// ============================================================================
// 资源请求事件处理
// ============================================================================

// 锁顺序: impl_->mtx → Storage 槽位锁 (try_acquire_read)。
// 不要在持有 Storage ReadHandle/WriteHandle 的作用域内获取 impl_->mtx，
// 否则会与 update() 中的 Storage→释放→impl_->mtx 路径形成死锁环。
void GeometrySystem::on_load_requested(const Events::ActorLoadRequestedEvent& e) {
    std::unique_lock lock(impl_->mtx);
    auto scene_it = impl_->scenes.find(e.scene);
    if (scene_it == impl_->scenes.end()) {
        return;
    }

    auto& scene_state = scene_it->second;
    if (scene_state.loading_tasks.count(e.actor) || scene_state.unloading_tasks.count(e.actor)) {
        return;
    }

    auto& actor_storage = SharedDataHub::instance().actor_storage();
    auto actor_read = actor_storage.try_acquire_read(e.actor);
    if (!actor_read.valid() || actor_read->model_path.empty()) {
        CFW_LOG_ERROR("[GeometrySystem] Invalid actor or empty model path: {}", e.actor);
        scene_state.actor_load_states[e.actor] = ActorLoadState::Unloaded;
        lock.unlock();
        impl_->ctx->event_bus()->publish(Events::ActorUnloadCompletedEvent{e.scene,e.actor});
        return;
    }

    CFW_LOG_NOTICE("[GeometrySystem] Start loading actor {} (path: {})",
                  e.actor, Utils::path_to_utf8(actor_read->model_path));
    scene_state.loading_tasks[e.actor] = Resource::ResourceManager::get_instance().import_async(actor_read->model_path);
}

// 锁顺序同 on_load_requested: impl_->mtx → Storage。
void GeometrySystem::on_unload_requested(const Events::ActorUnloadRequestedEvent& e) {
    std::unique_lock lock(impl_->mtx);
    auto scene_it = impl_->scenes.find(e.scene);
    if (scene_it == impl_->scenes.end()) return;

    auto& scene_state = scene_it->second;
    if (scene_state.loading_tasks.count(e.actor) || scene_state.unloading_tasks.count(e.actor)) {
        if (scene_state.unloading_tasks.count(e.actor)) {
            scene_state.unloading_tasks.erase(e.actor);
            scene_state.unload_retry_counts.erase(e.actor);
            scene_state.actor_load_states[e.actor] = ActorLoadState::Loaded;
            CFW_LOG_NOTICE("[GeometrySystem] Cancelled pending unload for actor {}", e.actor);
        }
        if (scene_state.loading_tasks.count(e.actor)) {
            // 加载进行中 — 将状态设为 Unloaded，加载完成时 process_async_tasks 检测到非 Loading 状态会跳过
            scene_state.actor_load_states[e.actor] = ActorLoadState::Unloaded;
            CFW_LOG_NOTICE("[GeometrySystem] Unload requested during load for actor {} — cancelling load", e.actor);
        }
        return;
    }

    auto& actor_storage = SharedDataHub::instance().actor_storage();
    auto actor_read = actor_storage.try_acquire_read(e.actor);
    if (!actor_read.valid() || actor_read->model_path.empty()) {
        scene_state.actor_load_states[e.actor] = ActorLoadState::Unloaded;
        lock.unlock();
        impl_->ctx->event_bus()->publish(Events::ActorUnloadCompletedEvent{e.scene, e.actor});
        return;
    }

    auto normalized = actor_read->model_path.is_relative()
        ? std::filesystem::absolute(actor_read->model_path)
        : actor_read->model_path;
    std::error_code ec;
    normalized = std::filesystem::weakly_canonical(normalized, ec);
    if (ec) normalized = actor_read->model_path;
    auto rid = Resource::IResource::generate_uid(normalized);

    CFW_LOG_NOTICE("[GeometrySystem] Start unloading actor {} (path: {})",
                  e.actor, Utils::path_to_utf8(actor_read->model_path));
    scene_state.unload_retry_counts[e.actor] = 0;
    scene_state.unloading_tasks[e.actor] = Resource::ResourceManager::get_instance().remove_cache_async(rid);
}

// ============================================================================
// 动态减面 (Mesh Simplification) 公共 API
// ============================================================================

void GeometrySystem::set_simplification_config(const MeshSimplificationConfig& cfg) {
    std::unique_lock lock(impl_->mtx);
    impl_->simplification_cfg = cfg;
}

const MeshSimplificationConfig& GeometrySystem::get_simplification_config() const {
    std::shared_lock lock(impl_->mtx);
    return impl_->simplification_cfg;
}

const LODMeshBuffers* GeometrySystem::get_lod_buffers(
    std::uintptr_t geometry_handle,
    uint32_t       mesh_index,
    int            lod_level) const {

    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) return nullptr;
    if (lod_level < 0 || static_cast<size_t>(lod_level) >= it->second.levels.size())
        return nullptr;
    auto& level = it->second.levels[lod_level];
    // 降级：如果目标级别未就绪，回退到 LOD 0
    if (!level.ready && lod_level > 0) {
        auto& lod0 = it->second.levels[0];
        return lod0.ready ? &lod0 : nullptr;
    }
    return level.ready ? &level : nullptr;
}

int GeometrySystem::get_lod_count(std::uintptr_t geometry_handle,
                                  uint32_t       mesh_index) const {
    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) return 0;
    return static_cast<int>(it->second.levels.size());
}

int GeometrySystem::resolve_lod_level(std::uintptr_t geometry_handle,
                                      uint32_t       mesh_index,
                                      float          screen_ratio) const {

    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) return 0;

    std::vector<float> thresholds;
    for (size_t i = 1; i < it->second.levels.size(); ++i) {
        thresholds.push_back(it->second.levels[i].screen_threshold);
    }

    int selected = select_lod_level(screen_ratio, thresholds);

    // 降级到最近的已就绪级别
    while (selected > 0) {
        if (static_cast<size_t>(selected) < it->second.levels.size()
            && it->second.levels[selected].ready)
            break;
        selected--;
    }
    return selected;
}

const LODMeshBuffers* GeometrySystem::resolve_lod_buffers(
    std::uintptr_t geometry_handle,
    uint32_t       mesh_index,
    float          screen_ratio) const {

    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) return nullptr;

    // 构建阈值列表（LOD 1..N 的 screen_threshold）
    std::vector<float> thresholds;
    for (size_t i = 1; i < it->second.levels.size(); ++i) {
        thresholds.push_back(it->second.levels[i].screen_threshold);
    }

    int selected = select_lod_level(screen_ratio, thresholds);

    // 降级到最近的已就绪级别
    while (selected > 0) {
        if (static_cast<size_t>(selected) < it->second.levels.size()
            && it->second.levels[selected].ready)
            break;
        selected--;
    }

    // 返回缓冲（与 get_lod_buffers 相同的降级策略）
    if (selected < 0 || static_cast<size_t>(selected) >= it->second.levels.size())
        return nullptr;

    auto& level = it->second.levels[selected];
    if (!level.ready && selected > 0) {
        auto& lod0 = it->second.levels[0];
        return lod0.ready ? &lod0 : nullptr;
    }
    return level.ready ? &level : nullptr;
}

// ============================================================================
// 动态减面内部管线
// ============================================================================

void GeometrySystem::upload_lod_from_scene_data() {
    // 快照 simplification_cfg，与 set_simplification_config 同步
    bool auto_on_load;
    int max_lod_levels;
    {
        std::shared_lock lock(impl_->mtx);
        auto_on_load = impl_->simplification_cfg.auto_on_load;
        max_lod_levels = impl_->simplification_cfg.max_lod_levels;
    }
    if (!auto_on_load) return;

    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& hub = SharedDataHub::instance();
    auto& geom_storage = hub.geometry_storage();

    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        const GeometryDevice& geom_dev = *it;
        auto geom_handle = reinterpret_cast<std::uintptr_t>(&geom_dev);
        if (!geom_dev.model_resource_handle) continue;

        // 通过 ModelResource 解析真正的 ResourceManager UID
        std::uint64_t model_id = 0;
        if (auto model_res = hub.model_resource_storage().try_acquire_read(geom_dev.model_resource_handle)) {
            model_id = model_res->model_id;
        }
        if (model_id == 0) continue;

        for (uint32_t mesh_idx = 0; mesh_idx < static_cast<uint32_t>(geom_dev.mesh_handles.size()); ++mesh_idx) {
            uint64_t lod_key = Impl::make_lod_key(geom_handle, mesh_idx);

            // 已有缓存且模型未变更则跳过（model_id 比较防止 slot 复用）
            {
                std::shared_lock lock(impl_->lod_cache_mutex);
                auto cache_it = impl_->lod_cache.find(lod_key);
                if (cache_it != impl_->lod_cache.end()
                    && cache_it->second.model_id == model_id)
                    continue;
            }

            // 从 ResourceManager 读取 Scene 数据
            auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
            if (!scene_read.valid()) continue;

            auto& scene = *scene_read;
            if (mesh_idx >= scene.data.meshes.size()) continue;

            auto& mesh = scene.data.meshes[mesh_idx];
            if (mesh.lod_levels.empty()) continue;

            // 创建缓存条目
            Impl::LODCacheEntry entry;
            entry.model_id = model_id;
            auto& mesh_dev = geom_dev.mesh_handles[mesh_idx];

            // LOD 0：复用现有的 GPU 缓冲
            LODMeshBuffers lod0;
            lod0.vertex_buffer    = mesh_dev.vertexBuffer;
            lod0.index_buffer     = mesh_dev.indexBuffer;
            lod0.vertex_storage   = mesh_dev.vertexStorageBuffer;
            lod0.index_storage    = mesh_dev.indexStorageBuffer;
            lod0.error            = 0.0f;
            lod0.screen_threshold = 1.0f;
            lod0.ready            = true;
            entry.levels.push_back(std::move(lod0));

            // LOD 1..N：从导入时 meshoptimizer 生成的数据创建 GPU 缓冲
            for (size_t lod_idx = 0; lod_idx < mesh.lod_levels.size() && lod_idx < static_cast<size_t>(max_lod_levels - 1); ++lod_idx) {
                auto& lod_data = mesh.lod_levels[lod_idx];
                if (lod_data.vertices.empty() || lod_data.indices.empty()) continue;

                LODMeshBuffers lod_buf;
                // LOD 索引保持 uint16（与 LOD0 一致），不转换为 uint32。
                // 之前转为 uint32 导致 GPU 索引缓冲格式与 pipeline 预期不匹配→渲染缺口。
                lod_buf.vertex_buffer    = HardwareBuffer(lod_data.vertices, BufferUsage::VertexBuffer);
                lod_buf.index_buffer     = HardwareBuffer(lod_data.indices,  BufferUsage::IndexBuffer);
                lod_buf.vertex_storage   = HardwareBuffer(lod_data.vertices, BufferUsage::StorageBuffer);
                lod_buf.index_storage    = HardwareBuffer(lod_data.indices,  BufferUsage::StorageBuffer);
                lod_buf.error            = lod_data.error;
                lod_buf.screen_threshold = lod_data.screen_threshold;
                lod_buf.ready            = true;
                entry.levels.push_back(std::move(lod_buf));
            }

            std::unique_lock lock(impl_->lod_cache_mutex);
            impl_->lod_cache.insert_or_assign(lod_key, std::move(entry));
        }
    }
}

}  // namespace Corona::Systems


