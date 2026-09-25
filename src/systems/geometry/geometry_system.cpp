#include <corona/events/engine_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/utils/storage.h>
#include <corona/resource/resource.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/resource/types/animation_pose.h>
#include <corona/resource/types/image.h>
#include <corona/shared_data_hub.h>
#include <corona/spatial/octree.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/utils/path_utils.h>
#include <ktm/ktm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "geometry_internal.h"
#include "storage_snapshot.h"
#include "../common/diagnostic_env.h"

#include <corona/systems/geometry/geometry_mesh_builder.h>

namespace Corona::Systems {

using namespace GeometryInternal;

namespace {

std::mutex g_invalid_mesh_slot_log_mutex;
std::unordered_set<std::string> g_invalid_mesh_slot_logs;

[[nodiscard]] bool geometry_diagnostics_enabled() {
    static const bool enabled = Diagnostics::parse_env_flag(
        std::getenv("CORONA_GEOMETRY_DIAG_PROFILE"));
    return enabled;
}

template <typename T>
Horizon::HardwareBuffer make_geometry_buffer(const std::vector<T>& data,
                                             Horizon::BufferUsageFlags usage,
                                             std::string name = {}) {
    Horizon::HardwareBufferDesc desc;
    desc.element_count = data.size();
    desc.element_size = static_cast<uint32_t>(sizeof(T));
    desc.usage = usage;
    desc.debug_name = std::move(name);
    return Horizon::HardwareBuffer(desc, std::as_bytes(std::span<const T>(data.data(), data.size())));
}

}  // namespace

// ============================================================================
// LOD 磁盘缓存 — 编译期结构体大小保护
// ============================================================================
// 若 Vertex/BoneWeights 因平台或编译器差异发生变化，编译直接失败，避免磁盘缓存
// 格式静默损坏（二进制 blob 的 header 中记录顶点/权重数量，反序列化按 sizeof 计算偏移）。

static_assert(sizeof(Resource::Vertex)      == 32,
    "Vertex size changed — update LOD blob format version in serialize_lod_record");
static_assert(sizeof(Resource::BoneWeights) == 32,
    "BoneWeights size changed — update LOD blob format version in serialize_lod_record");

// ============================================================================
// LOD 磁盘缓存 — 内部辅助函数
// ============================================================================

/// 构造磁盘缓存 key：三元素唯一标识一个 LOD 级
/// 格式："lod:<model_id_hex>:<mesh_index>:<lod_level_index>"
[[nodiscard]] static std::string make_lod_disk_key(
    std::uint64_t model_id,
    std::uint32_t mesh_index,
    int           lod_level_idx)
{
    std::ostringstream oss;
    oss << "lod:0x" << std::hex << model_id
        << ":" << std::dec << mesh_index
        << ":" << lod_level_idx;
    return oss.str();
}

/// 将 LodDiskRecord 序列化为固定布局的二进制 blob
///
/// 布局（全部小端序，x86/x64 原生即为 LE，memcpy 直接写入即 LE）：
///   [0..3]   魔数 'L''O''D''1'（第4字节为版本号，格式升级时递增）
///   [4..7]   顶点数量  (uint32_t)
///   [8..11]  索引数量  (uint32_t)
///   [12..15] 骨骼权重数量 (uint32_t)
///   [16..19] error            (float)
///   [20..23] screen_threshold (float)
///   [24..27] geometric_error  (float)
///   [28..]   顶点数组 (每顶点32字节，packed Vertex，连续 memcpy)
///   [...]    索引数组 (每索引2字节，uint16_t，连续 memcpy)
///   [...]    骨骼权重数组 (每顶点32字节，BoneWeights，连续 memcpy)
///
/// Vertex 有 #pragma pack(1) 保证无填充，BoneWeights 为定长32字节，
/// 因此直接 memcpy 整块拷贝比逐字段读写快一个数量级，且不会漏字段。
[[nodiscard]] static std::vector<char> serialize_lod_record(const LodDiskRecord& rec) {
    const std::uint32_t vc = static_cast<std::uint32_t>(rec.vertices.size());
    const std::uint32_t ic = static_cast<std::uint32_t>(rec.indices.size());
    const std::uint32_t bc = static_cast<std::uint32_t>(rec.bone_weights.size());

    constexpr size_t kHeaderBytes = 28;
    const size_t total = kHeaderBytes
                       + vc * sizeof(Resource::Vertex)
                       + ic * sizeof(std::uint16_t)
                       + bc * sizeof(Resource::BoneWeights);

    std::vector<char> blob(total);
    char* p = blob.data();

    // 写入 header：魔数 + 版本号 + 数组大小 + 误差值
    p[0] = 'L'; p[1] = 'O'; p[2] = 'D'; p[3] = '1'; p += 4;
    std::memcpy(p, &vc, 4); p += 4;
    std::memcpy(p, &ic, 4); p += 4;
    std::memcpy(p, &bc, 4); p += 4;
    std::memcpy(p, &rec.error, 4); p += 4;
    std::memcpy(p, &rec.screen_threshold, 4); p += 4;
    std::memcpy(p, &rec.geometric_error, 4); p += 4;

    // 写入 payload：三个数组连续原始内存（无填充，直接整块拷贝）
    if (vc) { std::memcpy(p, rec.vertices.data(), vc * sizeof(Resource::Vertex)); p += vc * sizeof(Resource::Vertex); }
    if (ic) { std::memcpy(p, rec.indices.data(),  ic * sizeof(std::uint16_t)); p += ic * sizeof(std::uint16_t); }
    if (bc) { std::memcpy(p, rec.bone_weights.data(), bc * sizeof(Resource::BoneWeights)); }

    return blob;
}

/// 从二进制 blob 反序列化为 LodDiskRecord
///
/// 校验流程（任一失败返回 nullopt，该级数据视为不可恢复，调用方保持其为空）：
///   1. blob 至少 28 字节（一个空 header 的下限）
///   2. 前 3 字节必须为 'L''O''D'——防止误读其他文件
///   3. 第 4 字节版本号必须为 '1'——引擎升级格式变化时递增，旧版本 blob 直接丢弃
///   4. 读出 3 个 uint32_t 数量 + 3 个 float 误差值
///   5. 用数量反算期望总字节数，与实际 blob 大小比对——防止数据截断/损坏导致 memcpy 越界
///   6. 按数量 resize 目标 vector，memcpy 整块拷入数据
[[nodiscard]] static std::optional<LodDiskRecord> deserialize_lod_record(const std::vector<char>& blob) {
    // 最小大小检查：不足一个 header 的长度必为损坏/截断数据
    if (blob.size() < 28) return std::nullopt;
    const char* p = blob.data();

    // 魔数 + 版本号校验
    if (p[0] != 'L' || p[1] != 'O' || p[2] != 'D') return std::nullopt;
    const char version = p[3];
    if (version != '1') {
        CFW_LOG_WARNING("[LOD-Disk] 无法识别的 blob 版本 '{}'，丢弃", version);
        return std::nullopt;
    }
    p += 4;

    // 读取 header 中的数量和误差值
    std::uint32_t vc, ic, bc;
    std::memcpy(&vc, p, 4); p += 4;
    std::memcpy(&ic, p, 4); p += 4;
    std::memcpy(&bc, p, 4); p += 4;

    float error, st, ge;
    std::memcpy(&error, p, 4); p += 4;
    std::memcpy(&st,    p, 4); p += 4;
    std::memcpy(&ge,    p, 4); p += 4;

    // 用读出的数量反算期望总大小，校验 blob 是否完整（防截断数据导致 memcpy 越界）
    const size_t expected = 28
                          + vc * sizeof(Resource::Vertex)
                          + ic * sizeof(std::uint16_t)
                          + bc * sizeof(Resource::BoneWeights);
    if (blob.size() < expected) {
        CFW_LOG_WARNING("[LOD-Disk] blob 大小不匹配：期望 {} 实际 {}",
                        expected, blob.size());
        return std::nullopt;
    }

    // 按数量分配空间，整块拷贝数据
    LodDiskRecord rec;
    rec.error            = error;
    rec.screen_threshold = st;
    rec.geometric_error  = ge;

    if (vc) { rec.vertices.resize(vc); std::memcpy(rec.vertices.data(), p, vc * sizeof(Resource::Vertex)); p += vc * sizeof(Resource::Vertex); }
    if (ic) { rec.indices.resize(ic);   std::memcpy(rec.indices.data(),  p, ic * sizeof(std::uint16_t)); p += ic * sizeof(std::uint16_t); }
    if (bc) { rec.bone_weights.resize(bc); std::memcpy(rec.bone_weights.data(), p, bc * sizeof(Resource::BoneWeights)); }

    return rec;
}

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
        auto id1 = ctx->event_bus()->subscribe<Events::ActorLoadFinishedEvent>(
            [this](const Events::ActorLoadFinishedEvent& e) {
                this->on_load_finished(e);
            });
        auto id2 = ctx->event_bus()->subscribe<Events::ActorUnloadFinishedEvent>(
           [this](const Events::ActorUnloadFinishedEvent& e) {
               this->on_unload_finished(e);
           });
        auto id3 = ctx->event_bus()->subscribe<Events::ActorLoadRequestedEvent>(
            [this](const Events::ActorLoadRequestedEvent& e) {
                this->on_load_requested(e);
            });
        auto id4 = ctx->event_bus()->subscribe<Events::ActorUnloadRequestedEvent>(
            [this](const Events::ActorUnloadRequestedEvent& e) {
                this->on_unload_requested(e);
            });
        // ---- M3 LRU：订阅 evict / restore 事件 ----
        auto id5 = ctx->event_bus()->subscribe<Events::ActorEvictRequestedEvent>(
            [this](const Events::ActorEvictRequestedEvent& e) {
                this->on_evict_requested(e);
            });
        auto id6 = ctx->event_bus()->subscribe<Events::ActorRestoreRequestedEvent>(
            [this](const Events::ActorRestoreRequestedEvent& e) {
                this->on_restore_requested(e);
            });

        impl_->event_subscriptions = {id1, id2, id3, id4, id5, id6};
    }

    // 同步默认资源预算到 ResourceManager
    if (impl_->resource_memory_budget_mb > 0) {
        Resource::ResourceManager::get_instance().set_memory_budget(
            impl_->resource_memory_budget_mb * 1024 * 1024);
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

    // ---- 延迟 GPU 释放（LRU evict 路径） ----
    // on_evict_requested 将 actor 加入 pending_gpu_releases 集合，
    // 延迟到此处（下一帧 update 头部）释放 GPU 资源，避免与
    // OpticsSystem 渲染线程产生 data race。
    if (!impl_->pending_gpu_releases.empty()) {
        std::unordered_set<Impl::Payload> to_release;
        {
            std::unique_lock lock(impl_->mtx);
            to_release.swap(impl_->pending_gpu_releases);
        }
        for (auto actor : to_release) {
            release_actor_gpu_resources(actor);
        }
    }

    // ---- 异步导入（方案 A 承接点）----
    // 扫描 PendingImport 的 GeometryDevice，发起 import_async / 轮询完成，
    // 填 model_id 后转 PendingBuild，并回填 MechanicsDevice AABB。
    // 磁盘 IO / assimp 解析全部在本（引擎）线程之外的 ResourceManager 线程池完成，
    // 不阻塞前端 CEF UI 线程。
    process_pending_geometry_imports();

    // ---- 初始构建（异步加载承接点）----
    // 为标记 PendingBuild 的 GeometryDevice 构建 GPU 资源（mesh_handles）。
    // 放在 LOD 上传之前，使本帧新建的 mesh 同帧即可上传其 LOD 数据。
    process_pending_geometry_builds();

    // ---- 动态减面管线 ----
    // LOD 由 GeometrySystem 内部自动管理，无外部开关。
    // upload：为新模型登记 LOD 缓存条目（LOD0 就绪 + LOD1..N 仅元数据，不建 GPU 缓冲）。
    upload_lod_from_scene_data();
    // 轮询回写已完成的异步 LOD 构建（方案 C）：放在 reconcile 之前，让本帧完成的级
    // 即刻参与本帧的驻留决策（计入 ready，避免对同级重复发起任务）。
    process_pending_lod_builds();
    // reconcile（Step 3a 按需驻留）和 reconcile_cpu_residency 已移到场景循环之后。
    // 这样 evict_lods_for_actor（场景循环内，按距离淘汰 LOD1..N）先于 reconcile 执行：
    // 避免 reconcile 刚恢复的 mesh 在同帧被 evict_lods_for_actor 重新淘汰，
    // 造成"恢复→淘汰→恢复"的每帧 ping-pong（GPU 缓冲反复创建/释放）。
    //
    // 必须在 upload 之后（缓存条目已建）、skin 之前（蒙皮需写本帧新建级）。

    // ---- 骨骼动画 CPU 蒙皮 ----
    // 已迁移到 MechanicsSystem::update_skinned_geometry()（物理线程帧尾执行）。
    // Geometry 仅负责创建/驻留 GPU 缓冲；蒙皮计算 + write_bytes 由 Mechanics 通过
    // get_skinning_targets() 借出句柄完成。蒙皮结果仍写回 GeometryDevice
    // （skinned_cpu_vertices / skinned_aabb），供 Vision / 物理消费。

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

                // 无网格的 actor（如音频物体，空 model_path）不参与几何加载/距离剔除，
                // 否则距离剔除每帧反复请求加载空路径并报 "empty model path" 错误。
                if (geometry_read->model_path_utf8.empty()) continue;

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
        // 对于初始即为 Loaded 的 actor，需要手动发布 ActorResidencyChangedEvent
        // 因为不经过 load 流程，on_load_finished 不会触发
        std::vector<Events::ActorResidencyChangedEvent> initial_resident;
        {
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);
            const ActorLoadState initial_state = scene_state.cfg.enable_distance_culling
                                                     ? ActorLoadState::Unloaded
                                                     : ActorLoadState::Loaded;
            for (auto actor_handle : added_actors) {
                auto [it, inserted] = scene_state.actor_load_states.try_emplace(
                    actor_handle, initial_state);
                if (inserted && initial_state == ActorLoadState::Loaded) {
                    initial_resident.push_back(
                        {scene_handle, actor_handle, /*loaded=*/true});
                }
            }
        }
        for (const auto& evt : initial_resident) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(evt);
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
                    impl_->last_load_finished_time.erase(it->first);
                    // 检查 actor 是否还存在于其他场景，若否，清理全局状态
                    bool exists_elsewhere = false;
                    for (auto& [other_scene, other_state] : impl_->scenes) {
                        if (&other_state != &scene_state &&
                            other_state.actor_load_states.count(it->first)) {
                            exists_elsewhere = true; break;
                        }
                    }
                    if (!exists_elsewhere) {
                        impl_->offline_actors.erase(it->first);
                        impl_->pending_gpu_releases.erase(it->first);
                    }
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

        // ---- M3 LRU 唤醒触发器 ----
        // 遍历可见 actor，检查是否有被 evict 后 offline 的。
        std::vector<Events::ActorRestoreRequestedEvent> pending_restores;
        {
            std::shared_lock lock(impl_->mtx);
            auto scene_it = impl_->scenes.find(scene_handle);
            if (scene_it != impl_->scenes.end()) {
                auto& scene_state = scene_it->second;
                for (auto actor : visible_actors) {
                    auto it = impl_->offline_actors.find(actor);
                    if (it == impl_->offline_actors.end() || !it->second) continue;
                    auto state_it = scene_state.actor_load_states.find(actor);
                    if (state_it == scene_state.actor_load_states.end()) continue;
                    if (state_it->second != ActorLoadState::Unloaded) continue;
                    if (scene_state.loading_tasks.count(actor)) continue;
                    if (scene_state.unloading_tasks.count(actor)) continue;

                    // 距离门禁：防止 P1 距离剔除与 M3 恢复互相推拉
                    if (scene_state.cfg.enable_distance_culling) {
                        auto entry_it = scene_state.actor_to_entry.find(actor);
                        if (entry_it != scene_state.actor_to_entry.end() && !cameras.empty()) {
                            float min_d = std::numeric_limits<float>::max();
                            for (const auto& [cam_pos, _] : cameras) {
                                min_d = std::min(min_d,
                                    ktm::distance(entry_it->second.center(), cam_pos));
                            }
                            if (min_d > scene_state.cfg.preload_distance) continue;
                        }
                    }

                    pending_restores.push_back({scene_handle, actor});
                }
            }
        }
        for (const auto& evt : pending_restores) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(evt);
        }

        struct PendingLoad   { Events::ActorLoadRequestedEvent evt;   float distance; };
        std::vector<PendingLoad>   pending_loads;
        struct PendingUnload { Events::ActorUnloadRequestedEvent evt; float distance; };
        std::vector<PendingUnload> pending_unloads;
        {
            // Phase 1: shared_lock — 收集候选、计算距离、决定转换（只读不写）
            // 持读锁时必须只读：用 find() 而非 get_or_create()，后者会 try_emplace
            // 改写 scenes map，在 shared_lock 下与并发读者构成 data race（UB）。
            // scene 已在本帧前面的 unique_lock 段（L197/L228）创建，find 必命中；
            // 万一不存在则跳过本阶段（无可剔除对象）。
            std::shared_lock lock(impl_->mtx);
            auto scene_state_it = impl_->scenes.find(scene_handle);
            if (scene_state_it != impl_->scenes.end()
                && scene_state_it->second.cfg.enable_distance_culling && !cameras.empty()) {
                auto& scene_state = scene_state_it->second;

                // 统一提取相机位置（preload + unload 共用）
                std::vector<ktm::fvec3> cam_positions;
                cam_positions.reserve(cameras.size());
                for (const auto& [cam_pos, _] : cameras) {
                    cam_positions.push_back(cam_pos);
                }

                // ============================================================
                // 八叉树自适应分块 preload
                // 替代 query_sphere → 逐 actor 中心距离 → 4 个/帧。
                // 节点 AABB 完全在 radius 内 → 收集其所有 Unloaded actor；
                // 部分在内 → 递归子节点；完全在外 → 跳过。
                // ============================================================
                std::vector<Spatial::Octree<Impl::Payload>::NodeInRange> nodes_in_range;
                scene_state.tree.collect_nodes_in_range(
                    cam_positions,
                    scene_state.cfg.preload_distance,
                    [&scene_state](Impl::Payload actor) {
                        auto it = scene_state.actor_load_states.find(actor);
                        return it != scene_state.actor_load_states.end()
                            && it->second == ActorLoadState::Unloaded;
                    },
                    nodes_in_range);

                // 保留非 Unloaded 的 actor（已在范围内、维持状态不变）
                std::unordered_set<Impl::Payload> in_range_actors;
                for (auto& node : nodes_in_range) {
                    for (auto actor : node.actors) {
                        in_range_actors.insert(actor);
                        pending_loads.push_back(
                            {{scene_handle, actor}, node.min_cam_distance});
                    }
                }
                // 补充：已在场景中非 Unloaded 的 actor 也纳入（防止被"未候选"误判）
                for (const auto& [actor, state] : scene_state.actor_load_states) {
                    if (state != ActorLoadState::Unloaded && !in_range_actors.count(actor)) {
                        in_range_actors.insert(actor);
                    }
                }

                // ---- 收集卸载候选：八叉树节点级批量判定 ----
                // 对每个节点：若其 AABB 在所有相机的 unload_distance 之外 →
                // 整棵子树一次性收集（不逐 actor、不逐相机计数）。
                // 与 preload 的 query_sphere（球内）对称，形成完整加载/卸载闭环。
                //
                // 有效卸载半径：取 max(unload, preload×1.2)，确保卸载边界严格在预加载
                // 边界之外。×1.2 系数提供 20% 滞回死区，避免 actor 在边界被
                // 卸载→预加载→卸载的 ping-pong（模型反复消失/出现闪烁）。
                // 例：preload=25 → 卸载边界≥30；unload=50 → 卸载边界=60。
                {
                    const float effective_unload_radius = std::max(
                        scene_state.cfg.unload_distance,
                        scene_state.cfg.preload_distance * 1.2f);

                    // LOD 空间淘汰距离（自动或手动）
                    const float lod_evict_dist = (scene_state.cfg.lod_evict_distance > 0)
                        ? scene_state.cfg.lod_evict_distance
                        : (scene_state.cfg.preload_distance + effective_unload_radius) * 0.5f;

                    // 空间淘汰启用时缩小遍历半径：collect_outside_spheres 收集半径外的节点，
                    // 更小半径 = 收集更多候选 = LOD 淘汰 + actor 卸载全覆盖
                    const float traversal_radius = scene_state.cfg.enable_lod_spatial_eviction
                        ? std::min(lod_evict_dist, effective_unload_radius)
                        : effective_unload_radius;

                    std::vector<Impl::Payload> outside_results;
                    scene_state.tree.collect_outside_spheres(
                        cam_positions, traversal_radius, outside_results);

                    // ---- 分级：LOD 空间淘汰 / actor 卸载 ----
                    struct LodEvictCandidate { Impl::Payload actor; float distance; };
                    std::vector<LodEvictCandidate> lod_evict_candidates;

                    auto& actor_storage = SharedDataHub::instance().actor_storage();
                    for (auto actor : outside_results) {
                        auto state_it = scene_state.actor_load_states.find(actor);
                        if (state_it == scene_state.actor_load_states.end()) continue;
                        if (state_it->second != ActorLoadState::Loaded) continue;
                        if (scene_state.loading_tasks.count(actor)) continue;
                        if (scene_state.unloading_tasks.count(actor)) continue;

                        auto actor_read = actor_storage.try_acquire_read(actor);
                        if (actor_read.valid() && actor_read->pinned) continue;

                        auto entry_it = scene_state.actor_to_entry.find(actor);
                        if (entry_it == scene_state.actor_to_entry.end()) continue;
                        float min_dist = std::numeric_limits<float>::max();
                        for (const auto& [cam_pos, _] : cameras) {
                            min_dist = std::min(min_dist,
                                ktm::distance(entry_it->second.center(), cam_pos));
                        }

                        if (min_dist > effective_unload_radius) {
                            pending_unloads.push_back({{scene_handle, actor}, min_dist});
                        } else if (scene_state.cfg.enable_lod_spatial_eviction
                                   && min_dist > lod_evict_dist * 1.15f) {
                            // 15% 滞回外扩：淘汰阈值略大于回读阈值，防止边界 ping-pong
                            lod_evict_candidates.push_back({actor, min_dist});
                        }
                    }

                    // ---- LOD 空间淘汰执行（远者优先，限速）----
                    if (!lod_evict_candidates.empty()) {
                        std::sort(lod_evict_candidates.begin(), lod_evict_candidates.end(),
                            [](const LodEvictCandidate& a, const LodEvictCandidate& b) {
                                return a.distance > b.distance;
                            });
                        if (lod_evict_candidates.size() > Impl::kMaxLodSpatialEvictPerFrame)
                            lod_evict_candidates.resize(Impl::kMaxLodSpatialEvictPerFrame);

                        for (const auto& c : lod_evict_candidates)
                            evict_lods_for_actor(c.actor);
                    }
                }
            }
        }

        // 按距离排序加载近者优先。每帧 actor 总量限制（替代旧的 4 个/帧）
        constexpr size_t kMaxActorsPerFrame  = 256;
        if (pending_loads.size() > kMaxActorsPerFrame) {
            std::sort(pending_loads.begin(), pending_loads.end(),
                [](const PendingLoad& a, const PendingLoad& b) {
                    return a.distance < b.distance;  // 近的优先
                });
            pending_loads.resize(kMaxActorsPerFrame);
        }
        // Phase 2: unique_lock — 应用状态转换（带 TOCTOU 重校验）
        if (!pending_loads.empty()) {
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);

            for (auto it = pending_loads.begin(); it != pending_loads.end(); ) {
                auto state_it = scene_state.actor_load_states.find(it->evt.actor);
                if (state_it != scene_state.actor_load_states.end() &&
                    state_it->second == ActorLoadState::Unloaded) {
                    state_it->second = ActorLoadState::Loading;
                    ++it;
                    } else {
                        it = pending_loads.erase(it);
                    }
            }
        }
        for (const auto& p : pending_loads) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(p.evt);
        }

        // ---- 卸载排序（远者优先）与 Phase 2 ----
        constexpr size_t kMaxUnloadsPerFrame = 8;   //每帧最多卸载8个
        if (pending_unloads.size() > kMaxUnloadsPerFrame) {
            std::sort(pending_unloads.begin(), pending_unloads.end(),
                [](const PendingUnload& a, const PendingUnload& b) {
                    return a.distance > b.distance;  // 远者优先
                });
            pending_unloads.resize(kMaxUnloadsPerFrame);
        }
        // Phase 2: unique_lock — TOCTOU 重校验后发起卸载
        if (!pending_unloads.empty()) {
            std::unique_lock lock(impl_->mtx);
            auto& scene_state = impl_->get_or_create(scene_handle);

            for (auto it = pending_unloads.begin(); it != pending_unloads.end(); ) {
                auto state_it = scene_state.actor_load_states.find(it->evt.actor);
                if (state_it != scene_state.actor_load_states.end() &&
                    state_it->second == ActorLoadState::Loaded) {
                    // TOCTOU：Phase 1 和 Phase 2 之间状态可能已被其他路径改变，
                    // 必须持写锁重新确认仍为 Loaded 再发起卸载。
                    state_it->second = ActorLoadState::Unloading;
                    ++it;
                } else {
                    it = pending_unloads.erase(it);
                }
            }
        }
        for (const auto& p : pending_unloads) {
            if (impl_->ctx && impl_->ctx->event_bus())
                impl_->ctx->event_bus()->publish(p.evt);
        }

        // 统计信息：使用读锁遍历，独立 stats_mutex 写入，减少主锁竞争
        {
            std::shared_lock lock(impl_->mtx);
            // 只读查找：scene 必然已在本帧早先的 unique_lock 段（八叉树重建）创建。
            // 不调用 get_or_create()——它会 try_emplace 改写 map，在 shared_lock 下是
            // 数据竞争（与其他读者并发 rehash → UB）。
            auto scene_it = impl_->scenes.find(scene_handle);
            if (scene_it != impl_->scenes.end()) {
                auto& scene_state = scene_it->second;

                std::size_t loaded = 0, loading = 0, unloading = 0, unloaded = 0, offline_count = 0;
                for (const auto& [actor_handle, state] : scene_state.actor_load_states) {
                    switch (state) {
                        case ActorLoadState::Loaded:    loaded++; break;
                        case ActorLoadState::Loading:   loading++; break;
                        case ActorLoadState::Unloading: unloading++; break;
                        case ActorLoadState::Unloaded:  unloaded++; break;
                    }
                    auto off_it = impl_->offline_actors.find(actor_handle);
                    if (off_it != impl_->offline_actors.end() && off_it->second)
                        offline_count++;
                }

                std::lock_guard stats_lock(scene_state.stats_mutex);
                scene_state.stats.actor_total    = actor_handles.size();
                scene_state.stats.actor_visible  = visible_actors.size();
                scene_state.stats.actor_offline  = offline_count;
                scene_state.stats.octree_entries = octree_entries.size();
                scene_state.stats.last_query_ms = visible_query_ms_total;
                scene_state.stats.actor_loaded    = loaded;
                scene_state.stats.actor_loading   = loading;
                scene_state.stats.actor_unloading = unloading;
                scene_state.stats.actor_unloaded  = unloaded;
            }
        }
        auto scene_write = scene_storage.try_acquire_write(scene_handle);
        if (scene_write.valid()) {
            SceneDevice& scene_dev_write = *scene_write;
            scene_dev_write.min_world = root_aabb.min;
            scene_dev_write.max_world = root_aabb.max;
            scene_dev_write.center_world = root_aabb.center();
            scene_dev_write.visible_actor_handles.assign(visible_actors.begin(),
                                                         visible_actors.end());
        }
    }

    // ---- 动态减面驻留协调（Step 3a：按需驻留）----
    // 必须在场景循环之后调用：场景循环中的 evict_lods_for_actor 先按距离淘汰
    // LOD1..N GPU 缓冲并置 lod_spatially_evicted，然后 reconcile 统一处理驻留——
    // 包括对已淘汰 mesh 保持 LOD0、对回到恢复区的 mesh 清除标记并重建需求级。
    // 若在场景循环之前调用，reconcile 先恢复 mesh，随后 evict_lods_for_actor
    // 同帧重新淘汰，造成"恢复→淘汰→恢复"的每帧 ping-pong。
    reconcile_lod_residency();

    // ---- CPU LOD 驻留协调（RAM 3层窗口管理）----
    // 每 kCpuWindowEvalInterval 帧运行一次：确定各 model_id 的 CPU 驻留窗口
    // {lod_levels[0], lod_levels[demand_median-1], lod_levels[N-1]}，
    // 窗口外的 LODLevel 数据移交异步队列写入磁盘缓存以节省 RAM，
    // 窗口内数据缺失的级从磁盘缓存回读，供 GPU 侧按需重建该级缓冲。
    if (++impl_->cpu_window_eval_counter >= Impl::kCpuWindowEvalInterval) {
        impl_->cpu_window_eval_counter = 0;
        reconcile_cpu_residency();
    }

    // ========================================
    // 每帧资源预算检查：超过预算时主动淘汰最久未访问的冷资源
    // ========================================
    if (impl_->resource_memory_budget_mb > 0) {
        auto& rm = Resource::ResourceManager::get_instance();
        auto used = rm.used_memory_bytes();
        auto budget = rm.memory_budget();
        if (budget > 0 && used > budget) {
            CFW_LOG_NOTICE("[GeometrySystem] Resource memory {} MB over budget {} MB, evicting...",
                           used / (1024 * 1024), budget / (1024 * 1024));
            auto result = rm.evict_until_under_budget();
            if (result.success) {
                CFW_LOG_NOTICE("[GeometrySystem] Evicted resource {}, freed {} bytes "
                               "({} MB used after eviction)",
                               result.rid, result.bytes_freed,
                               rm.used_memory_bytes() / (1024 * 1024));
            } else {
                CFW_LOG_WARNING("[GeometrySystem] Eviction stalled: all resources pinned or in use "
                                "({} MB still over {} MB budget)",
                                rm.used_memory_bytes() / (1024 * 1024),
                                budget / (1024 * 1024));
            }
        }
    }

    // 每秒输出一次流式加载统计
    {
        static int frame_counter = 0;
        if (++frame_counter >= 60) {
            frame_counter = 0;
            // ===== 临时诊断：reconcile 一秒行为画像（定位卡顿/LOD切换根因）=====
            // 解读：
            //  - mesh_visits ≈ 可见 mesh 数 × 60（每帧每 mesh 一次）→ 正常
            //  - scene_acquires 同量级 → 每帧每 geom 取 Scene（merge 引入，疑似开销源）
            //  - builds/frees 若每秒成百上千 → reconcile 在 GPU churn（卡顿+LOD切换坐实）
            //  - demand_changes 高 → 选级在抖动；低但 builds 高 → 别处反复失效缓存
            if (geometry_diagnostics_enabled()) {
                CFW_LOG_NOTICE("[GeoDiag/1s] mesh_visits={} scene_acquires={} builds={} frees={} demand_changes={} launches={} discards={} inflight={} upload_queued={} upload_published={} upload_discarded={}"
                               " lod_budget_checks={} lod_budget_degraded={} lod_budget_entries={} lod_budget_est_vram={}KB",
                               impl_->diag_reconcile_mesh_visits, impl_->diag_scene_acquires,
                               impl_->diag_lod_builds, impl_->diag_lod_frees,
                               impl_->diag_demand_changes,
                               impl_->diag_lod_build_launches, impl_->diag_lod_build_discards,
                               impl_->pending_lod_builds.size(),
                               impl_->diag_geometry_upload_queued,
                               impl_->diag_geometry_upload_published,
                               impl_->diag_geometry_upload_discarded,
                               impl_->diag_lod_budget_checks,
                               impl_->diag_lod_budget_degraded,
                               impl_->diag_lod_budget_entries,
                               impl_->diag_lod_budget_est_vram / 1024);
            }
            impl_->diag_reconcile_mesh_visits = 0;
            impl_->diag_scene_acquires = 0;
            impl_->diag_lod_builds = 0;
            impl_->diag_lod_frees = 0;
            impl_->diag_demand_changes = 0;
            impl_->diag_lod_build_launches = 0;
            impl_->diag_lod_build_discards = 0;
            impl_->diag_geometry_upload_queued = 0;
            impl_->diag_geometry_upload_published = 0;
            impl_->diag_geometry_upload_discarded = 0;

            // LOD 级 LRU 诊断计数器清零
            impl_->diag_lod_budget_checks   = 0;
            impl_->diag_lod_budget_degraded = 0;
            impl_->diag_lod_budget_entries  = 0;
            impl_->diag_lod_budget_est_vram = 0;

            std::shared_lock lock(impl_->mtx);
            for (auto& [scene_handle, scene_state] : impl_->scenes) {
                std::lock_guard stats_lock(scene_state.stats_mutex);
                auto& s = scene_state.stats;
                if (geometry_diagnostics_enabled()) {
                    CFW_LOG_NOTICE("[GeometrySystem] Scene stats: total={} visible={} "
                                   "loaded={} loading={} unloading={} unloaded={} offline={}",
                                   s.actor_total, s.actor_visible,
                                   s.actor_loaded, s.actor_loading, s.actor_unloading,
                                   s.actor_unloaded, s.actor_offline);
                }
            }
            // auto& rm = Resource::ResourceManager::get_instance();
            // auto entries = rm.list_entries();
            // auto res_used = rm.used_memory_bytes();
            // CFW_LOG_NOTICE("[GeometrySystem] Resource: {}KB used / {}MB budget, {} entries",
            //                res_used / 1024,
            //                rm.memory_budget() / (1024 * 1024),
            //                entries.size());
            // if (impl_->actor_cache) {
            //     auto mem_bytes = impl_->actor_cache->memory_used();
            //     CFW_LOG_NOTICE("[GeometrySystem] ActorCache: {}B mem / {}B disk",
            //                    mem_bytes, impl_->actor_cache->disk_used() );
            // } else {
            //     CFW_LOG_NOTICE("[GeometrySystem] ActorCache: not created yet (no eviction has occurred)");
            // }
        }
    }

    // ---- P0：mesh/texture 内存账本维护（登记 + 对账）+ 预算报告（~1Hz）----
    // 刻意不持 impl_->mtx：内部仅用 storage 锁 / ResourceManager / cpu_ledger_mutex，
    // 避免与 update() 其它锁段构成锁序纠缠。
    {
        static int ledger_counter = 0;
        if (++ledger_counter >= 60) {
            ledger_counter = 0;
            update_cpu_resource_ledger();
            const MemoryReport mr = compute_memory_report();
            if (geometry_diagnostics_enabled()) {
                CFW_LOG_NOTICE("[GeometrySystem] Mem mesh: VRAM={}KB RAM={}KB | tex: VRAM={}KB RAM={}KB "
                               "| VRAM total={}KB (peak {}KB){} | RAM total={}KB{}",
                               mr.vram.mesh_bytes / 1024, mr.ram.mesh_bytes / 1024,
                               mr.vram.texture_bytes / 1024, mr.ram.texture_bytes / 1024,
                               mr.vram.used_bytes / 1024,
                               (mr.vram_mesh_peak + mr.vram_texture_peak) / 1024,
                               mr.vram.pressured ? " OVER-VRAM-BUDGET" : "",
                               mr.ram.used_bytes / 1024,
                               mr.ram.pressured ? " OVER-RAM-BUDGET" : "");
            }
        }
    }

    // ---- 满载淘汰（VRAM/RAM 达 90% 时启用）----
    // 每隔 pressure_eval_interval 帧评估一次；不持 impl_->mtx（内部自管锁 + 锁外发事件）。
    if (++impl_->pressure_eval_counter >= impl_->pressure_eval_interval) {
        impl_->pressure_eval_counter = 0;
        evict_under_memory_pressure();
    }
}

void GeometrySystem::evict_lods_for_actor(std::uintptr_t actor) {
    auto& hub = SharedDataHub::instance();
    auto actor_read = hub.actor_storage().try_acquire_read(actor);
    if (!actor_read.valid()) return;

    std::unordered_set<std::uintptr_t> visited_geom;

    for (auto profile_handle : actor_read->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;

        std::vector<std::uintptr_t> geom_handles;
        if (profile->geometry_handle) geom_handles.push_back(profile->geometry_handle);
        if (profile->mechanics_handle)
            if (auto m = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle))
                if (m->geometry_handle) geom_handles.push_back(m->geometry_handle);
        if (profile->optics_handle)
            if (auto o = hub.optics_storage().try_acquire_read(profile->optics_handle))
                if (o->geometry_handle) geom_handles.push_back(o->geometry_handle);
        if (profile->acoustics_handle)
            if (auto a = hub.acoustics_storage().try_acquire_read(profile->acoustics_handle))
                if (a->geometry_handle) geom_handles.push_back(a->geometry_handle);

        for (auto geom_handle : geom_handles) {
            if (!visited_geom.insert(geom_handle).second) continue;

            auto geom_read = hub.geometry_storage().try_acquire_read(geom_handle);
            if (!geom_read) continue;

            for (uint32_t mesh_idx = 0;
                 mesh_idx < static_cast<uint32_t>(geom_read->mesh_handles.size());
                 ++mesh_idx) {

                uint64_t lod_key = Impl::make_lod_key(geom_handle, mesh_idx);

                {
                    std::unique_lock lock(impl_->lod_cache_mutex);
                    auto cit = impl_->lod_cache.find(lod_key);
                    if (cit == impl_->lod_cache.end()) continue;
                    if (cit->second.levels.size() <= 1) continue;
                    if (cit->second.lod_spatially_evicted) continue;  // 已淘汰，幂等跳过

                    // 释放 LOD1..N 的 GPU 缓冲，LOD0 保留
                    for (size_t lvl = 1; lvl < cit->second.levels.size(); ++lvl) {
                        auto& buf = cit->second.levels[lvl];
                        if (!buf.ready) continue;
                        buf.vertex_buffer  = Horizon::HardwareBuffer{};
                        buf.index_buffer   = Horizon::HardwareBuffer{};
                        buf.vertex_storage = Horizon::HardwareBuffer{};
                        buf.index_storage  = Horizon::HardwareBuffer{};
                        buf.mesh_mem       = Corona::Memory::GpuMemToken{};
                        buf.ready          = false;
                    }

                    cit->second.lod_spatially_evicted = true;
                    cit->second.committed_demand = 0;
                    // 空间淘汰 LOD1..N 后，shadow 必须回退 LOD0，否则会引用已释放的缓冲
                    cit->second.shadow_committed_demand = -1;
                    cit->second.shadow_prev_committed   = -1;
                    cit->second.shadow_swap_in_progress = false;
                    // 主视图 swap 状态也必须重置：prev_committed 可能指向已被释放的级，
                    // 残留的 swap_in_progress=true 会在恢复时阻止 prev_committed 正确更新
                    cit->second.swap_in_progress = false;
                    cit->second.prev_committed   = -1;
                }  // unique_lock 释放

                // 取消该 lod_key 的在途异步构建：LOD1..N 已被释放，
                // 若 TBB worker 稍后完成构建，process_pending_lod_builds 回写
                // 会重新设 ready=true，撤销本次淘汰。
                //
                // 线程安全：evict_lods_for_actor 仅在几何线程 update() 中调用，
                // pending_lod_builds/pending_shadow_lod_builds 的所有其他访问
                // （reconcile 插入、process_pending_lod_builds 轮询/移除）也在
                // 同一几何线程，无竞态，无需额外加锁。
                impl_->pending_lod_builds.erase(lod_key);
                impl_->pending_shadow_lod_builds.erase(lod_key);
                 }
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
    impl_->pending_gpu_releases.clear();

    // 等待并清理在途的异步 import 任务，避免 future 析构阻塞或悬挂回调。
    for (auto& [geom_handle, task] : impl_->pending_import_tasks) {
        if (task.future.valid()) task.future.wait();
    }
    impl_->pending_import_tasks.clear();

    impl_->geometry_build_tasks.wait();
    impl_->pending_geometry_builds.clear();

    // 等待在途的异步 LOD 构建任务（方案 C）：task_group::wait() 阻塞至全部完成，
    // 避免 task_group 析构时仍有任务运行 / promise 悬挂。结果丢弃（缓冲 RAII 释放）。
    impl_->lod_build_tasks.wait();
    impl_->pending_lod_builds.clear();

    // 停止 LOD 磁盘缓存写入线程：先置停止标志再唤醒，worker 醒来后会把队列中
    // 剩余任务排空再退出；join 确保线程在 Impl 成员销毁前结束，防止 worker
    // 访问悬空的 this / 已析构的队列与缓存对象。
    if (impl_->lod_disk_worker_running.load(std::memory_order_acquire)) {
        impl_->lod_disk_worker_running.store(false, std::memory_order_release);
        impl_->lod_disk_write_cv.notify_one();
        if (impl_->lod_disk_worker && impl_->lod_disk_worker->joinable()) {
            impl_->lod_disk_worker->join();
        }
    }

    // join 之后不会再有新任务入队，但仍做一次防御性排空：覆盖"置停止标志与
    // worker 检查标志之间"极窄窗口内入队、worker 退出前未及处理的任务，
    // 确保已从 lod_levels 移出的数据不丢失（该数据是此级唯一的副本，
    // 丢失后该级不再可恢复）。
    while (true) {
        LodDiskWriteTask task;
        {
            std::lock_guard qlock(impl_->lod_disk_write_mutex);
            if (impl_->pending_lod_disk_writes.empty()) break;
            task = std::move(impl_->pending_lod_disk_writes.front());
            impl_->pending_lod_disk_writes.pop_front();
        }
        // 任务只会在 ensure_lod_disk_cache() 之后入队，此处 lod_disk_cache
        // 必然非空；判空仅为防御（空则任务只能丢弃，无处可写）
        if (impl_->lod_disk_cache) {
            bool ok = impl_->write_one_lod_record(task);
            if (!ok && task.retry_count < Impl::kMaxLodDiskWriteRetries) {
                ++task.retry_count;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                impl_->pending_lod_disk_writes.push_front(std::move(task));
                continue;
            }
            if (!ok) {
                CFW_LOG_ERROR("[LOD-Disk] 关闭时写入失败已达最大重试次数，丢弃: {}"
                              " (model={:#x} mesh={} lod={})",
                              task.key, task.model_id, task.mesh_index, task.lod_level);
            }
        }
    }

    // 释放 LOD 磁盘缓存（CacheManager 析构时将内存级数据刷盘）。
    // 磁盘条目虽跨进程保留，但不会被下次启动命中：restore 的前提
    // vertices.empty() 只在本会话 evict（会覆盖写同 key blob）之后成立，
    // 而下次启动导入会重新生成全部 lod_levels。旧条目仅占用磁盘 LRU
    // 容量，直到被自然淘汰。
    impl_->lod_disk_cache.reset();

    // 释放 LRU ActorCache（确保在 shutdown 时清理磁盘/内存）
    impl_->actor_cache.reset();

    // 显式释放共享占位纹理，确保在 GPU device 仍存活时析构 HardwareImage。
    // 占位纹理现由 geometry_mesh_builder 模块持有（进程级单例，唯一所有者）。
    release_geometry_placeholder_texture();
}

// ============================================================================
// 配置
// ============================================================================

void GeometrySystem::set_visibility_config(std::uintptr_t scene, SceneVisibilityConfig cfg) {
    std::unique_lock lock(impl_->mtx);
    impl_->get_or_create(scene).cfg = cfg;

    // 关闭 LOD 空间淘汰时，立即清除所有已存在的 lod_spatially_evicted 标记。
    // 否则已被标记的 mesh 永久卡在 LOD0，直到碰巧走进 lod_evict_distance 内。
    if (!cfg.enable_lod_spatial_eviction) {
        std::unique_lock lod_lock(impl_->lod_cache_mutex);
        for (auto& [key, entry] : impl_->lod_cache) {
            if (entry.lod_spatially_evicted) {
                entry.lod_spatially_evicted = false;
                // 不清除 committed_demand：reconcile 下帧会正常重算。
            }
        }
    }

    // 同步全局值（reconcile 使用）。若显式设置则用设置值，
    // 否则自动计算。注意：多 scene 时以最后一次调用为准（v1 限制）。
    if (cfg.lod_evict_distance > 0)
        impl_->lod_evict_distance = cfg.lod_evict_distance;
    else if (cfg.preload_distance > 0) {
        const float eff = std::max(cfg.unload_distance, cfg.preload_distance * 1.2f);
        impl_->lod_evict_distance = (cfg.preload_distance + eff) * 0.5f;
    }
}

void GeometrySystem::set_distance_config(std::uintptr_t scene, float unload_dist,
                                    float preload_dist,bool enable) {
    std::unique_lock lock(impl_->mtx);
    auto& scene_state = impl_->get_or_create(scene);
    scene_state.cfg.enable_distance_culling = enable;
    scene_state.cfg.unload_distance = unload_dist;
    scene_state.cfg.preload_distance = preload_dist;
    // 同步全局值：preload/unload 改变时自动重算
    // 注：即使 enable=false（关闭距离剔除），仍更新 lod_evict_distance，
    // 因为由 enable_lod_spatial_eviction 独立控制，与 distance culling 开关正交。
    const float eff_unload = std::max(unload_dist, preload_dist * 1.2f);
    impl_->lod_evict_distance = (preload_dist + eff_unload) * 0.5f;
}

void GeometrySystem::set_cache_directory(std::filesystem::path dir) {
    impl_->actor_cache_dir = std::move(dir);
    // 如果 ActorCache 已创建，下次 evict 时会沿用旧目录；
    // 如果尚未创建，ensure_actor_cache() 将使用新目录。
    CFW_LOG_NOTICE("[GeometrySystem] Cache directory set to {}", impl_->actor_cache_dir.string());
}

void GeometrySystem::set_resource_memory_budget_mb(std::size_t mb) {
    impl_->resource_memory_budget_mb = mb;
    if (mb > 0) {
        Resource::ResourceManager::get_instance().set_memory_budget(mb * 1024 * 1024);
        CFW_LOG_NOTICE("[GeometrySystem] Resource memory budget set to {} MB", mb);
    } else {
        Resource::ResourceManager::get_instance().set_memory_budget(0);
        CFW_LOG_NOTICE("[GeometrySystem] Resource memory budget disabled (unlimited)");
    }
}

void GeometrySystem::set_vram_budget_mb(std::size_t mb) {
    impl_->vram_budget_bytes = mb * 1024ull * 1024ull;
    CFW_LOG_NOTICE("[GeometrySystem] VRAM budget set to {} MB ({})", mb,
                   mb ? "report-only, no eviction yet" : "unlimited");
}

// ============================================================================
// 私有事件处理
// ============================================================================

void GeometrySystem::on_load_finished(const Events::ActorLoadFinishedEvent& event) {
    {
        std::unique_lock lock(impl_->mtx);
        auto scene_it = impl_->scenes.find(event.scene);
        if (scene_it != impl_->scenes.end()) {
            auto& state_map = scene_it->second.actor_load_states;
            auto actor_it = state_map.find(event.actor);
            if (actor_it != state_map.end() && actor_it->second == ActorLoadState::Loading) {
                actor_it->second = ActorLoadState::Loaded;
                impl_->offline_actors[event.actor] = false;
                impl_->last_load_finished_time[event.actor] = std::chrono::steady_clock::now();
                CFW_LOG_NOTICE("GeometrySystem: Actor {} (scene: {}) load finished",
                               event.actor, event.scene);
            }
        }
    }

    // 加载完成，解除 pin（后续由 Optics/Mechanics 的 touch 续期维持热度）
    {
        auto actor_read = SharedDataHub::instance().actor_storage().try_acquire_read(event.actor);
        if (actor_read && !actor_read->model_path.empty()) {
            auto normalized = actor_read->model_path.is_relative()
                ? std::filesystem::absolute(actor_read->model_path)
                : actor_read->model_path;
            std::error_code ec;
            normalized = std::filesystem::weakly_canonical(normalized, ec);
            if (ec) normalized = actor_read->model_path;  // 回退，防止 pin 泄漏
            auto rid = Resource::IResource::generate_uid(normalized);
            Resource::ResourceManager::get_instance().unpin(rid);
        }
    }

    // 不再重新发布 ActorLoadFinishedEvent（由 process_async_tasks 发布）。
    // 对外发布统一的驻留变更事件，外部系统只需订阅 ActorResidencyChangedEvent。
    if (impl_->ctx && impl_->ctx->event_bus()) {
        impl_->ctx->event_bus()->publish(Events::ActorResidencyChangedEvent{
            event.scene, event.actor, /*loaded=*/true});
    }
}

void GeometrySystem::on_unload_finished(const Events::ActorUnloadFinishedEvent& event) {
    {
        std::unique_lock lock(impl_->mtx);
        auto scene_it = impl_->scenes.find(event.scene);
        if (scene_it == impl_->scenes.end()) return;

        auto& state_map = scene_it->second.actor_load_states;
        auto actor_it = state_map.find(event.actor);
        if (actor_it != state_map.end() && actor_it->second == ActorLoadState::Unloading) {
            actor_it->second = ActorLoadState::Unloaded;
            impl_->offline_actors[event.actor] = true;
            CFW_LOG_NOTICE("GeometrySystem: Actor {} (scene: {}) unload finished",
                           event.actor, event.scene);
        }
    }
    // 不再重新发布 ActorUnloadFinishedEvent（由 process_async_tasks 发布）。
    // 对外发布统一的驻留变更事件，外部系统只需订阅 ActorResidencyChangedEvent。
    if (impl_->ctx && impl_->ctx->event_bus()) {
        impl_->ctx->event_bus()->publish(Events::ActorResidencyChangedEvent{
            event.scene, event.actor, /*loaded=*/false});
    }
}

// ============================================================================
// GPU 资源释放（unload 时由 process_async_tasks 调用）
// ============================================================================

// ============================================================================
// release_actor_gpu_resources
// 功能：释放指定 actor 占用的全部 GPU 资源（显存中的顶点/索引缓冲和纹理）
// 调用时机：process_async_tasks() 中处理 ActorUnloadFinishedEvent 时
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
            // 阶段 A：创建 MeshDevice 数组（GPU 缓冲 + 纹理）
            // 构建逻辑收敛到 build_mesh_devices_from_scene（单一来源），
            // 与 Python API 层 Geometry 构造函数共用同一份实现。
            // 占位纹理由 builder 模块持有（进程级单例），无需在此创建。
            // ================================================================
            std::vector<MeshDevice> mesh_devices = build_mesh_devices_from_scene(scene);

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
                            continue;
                }
            }
        }

        if (task.rid != Resource::IResource::INVALID_UID) {
            // 重建 GPU 资源（mesh_handles + 纹理），恢复 model_resource_handle
            // 必须在发布 ActorLoadFinishedEvent 之前完成，
            // 以保证事件订阅者（渲染线程等）能读到有效的 GPU 缓冲
            rebuild_actor_gpu_resources(task.actor, task.rid);

            impl_->ctx->event_bus()->publish(Events::ActorLoadFinishedEvent{task.scene_handle,task.actor});
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
            impl_->ctx->event_bus()->publish(Events::ActorUnloadFinishedEvent{task.scene_handle, task.actor});
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
            impl_->ctx->event_bus()->publish(Events::ActorUnloadFinishedEvent{task.scene_handle, task.actor});
        } else {
            // 卸载失败，保存到列表中后续处理重试
            failed_unloads.push_back(task);
        }
    }

    //卸载失败重试
    if (!failed_unloads.empty()) {
        std::vector<Events::ActorUnloadFinishedEvent> deferred_events;
        std::vector<Events::ActorResidencyChangedEvent> deferred_residency;
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
                CFW_LOG_WARNING("[SceneSystem] Actor {} unload delayed (resource in use), retry {}/10",
                               reinterpret_cast<void*>(task.actor), retry_count + 1);

                if (++retry_count >= 10) {
                    CFW_LOG_ERROR("[SceneSystem] Actor {} unload failed after 10 retries, forcing GPU release",
                                 reinterpret_cast<void*>(task.actor));
                    scene_state.unload_retry_counts.erase(task.actor);
                    scene_state.actor_load_states[task.actor] = ActorLoadState::Unloaded;
                    impl_->pending_gpu_releases.insert(task.actor);  // 下一帧强制释放
                    deferred_residency.push_back(
                        {task.scene_handle, task.actor, /*loaded=*/false});
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
                            CFW_LOG_WARNING("[SceneSystem] Actor {} model path empty, mark as unloaded",
                                           reinterpret_cast<void*>(task.actor));
                            scene_state.unload_retry_counts.erase(task.actor);
                            scene_state.actor_load_states[task.actor] = ActorLoadState::Unloaded;
                            deferred_events.push_back({task.scene_handle, task.actor});
                            // on_unload_finished 检查 Unloading 状态，此处已是 Unloaded 不会通过
                            // 所以直接在此 push residency 事件
                            deferred_residency.push_back(
                                {task.scene_handle, task.actor, /*loaded=*/false});
                        }
                    } else {
                        CFW_LOG_WARNING("[SceneSystem] Actor {} handle invalid, clean up all states",
                                       reinterpret_cast<void*>(task.actor));
                        scene_state.unload_retry_counts.erase(task.actor);
                        scene_state.actor_load_states.erase(task.actor);
                        impl_->offline_actors.erase(task.actor);
                        deferred_residency.push_back(
                            {task.scene_handle, task.actor, /*loaded=*/false});
                    }
                }
            }
        }
        for (const auto& evt : deferred_events) {
            impl_->ctx->event_bus()->publish(evt);
        }
        for (const auto& evt : deferred_residency) {
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
        impl_->ctx->event_bus()->publish(Events::ActorUnloadFinishedEvent{e.scene,e.actor});
        return;
    }

    CFW_LOG_NOTICE("[GeometrySystem] Start loading actor {} (path: {})",
                  e.actor, Utils::path_to_utf8(actor_read->model_path));

    // pin 住资源防止加载过程中被预算淘汰踢掉
    // 加载完成后由 on_load_finished 解除 pin，后续由 touch 续期维持热度
    auto normalized = actor_read->model_path.is_relative()
        ? std::filesystem::absolute(actor_read->model_path)
        : actor_read->model_path;
    std::error_code ec;
    normalized = std::filesystem::weakly_canonical(normalized, ec);
    if (!ec) {
        auto rid = Resource::IResource::generate_uid(normalized);
        Resource::ResourceManager::get_instance().pin(rid);
    }

    scene_state.loading_tasks[e.actor] = Resource::ResourceManager::get_instance().import_async(actor_read->model_path);
}

// 锁顺序同 on_load_requested: impl_->mtx → Storage。
void GeometrySystem::on_unload_requested(const Events::ActorUnloadRequestedEvent& e) {
    std::unique_lock lock(impl_->mtx);
    auto scene_it = impl_->scenes.find(e.scene);
    if (scene_it == impl_->scenes.end()) return;

    auto& scene_state = scene_it->second;

    // 已经在卸载中 → 取消卸载，恢复 Loaded
    if (scene_state.unloading_tasks.count(e.actor)) {
        scene_state.unloading_tasks.erase(e.actor);
        scene_state.unload_retry_counts.erase(e.actor);
        scene_state.actor_load_states[e.actor] = ActorLoadState::Loaded;
        lock.unlock();
        impl_->ctx->event_bus()->publish(
            Events::ActorResidencyChangedEvent{e.scene, e.actor, /*loaded=*/true});
        CFW_LOG_NOTICE("[GeometrySystem] Cancelled pending unload for actor {}", e.actor);
        return;
    }

    // 正在加载中 → 取消加载，必须清除 loading_tasks 避免 future 阻塞后续加载
    if (scene_state.loading_tasks.count(e.actor)) {
        scene_state.loading_tasks.erase(e.actor);
        scene_state.actor_load_states[e.actor] = ActorLoadState::Unloaded;
        lock.unlock();
        impl_->ctx->event_bus()->publish(
            Events::ActorResidencyChangedEvent{e.scene, e.actor, /*loaded=*/false});
        CFW_LOG_NOTICE("[GeometrySystem] Unload requested during load for actor {} — cancelling load", e.actor);
        return;
    }

    auto& actor_storage = SharedDataHub::instance().actor_storage();
    auto actor_read = actor_storage.try_acquire_read(e.actor);
    if (!actor_read.valid() || actor_read->model_path.empty()) {
        scene_state.actor_load_states[e.actor] = ActorLoadState::Unloaded;
        lock.unlock();
        impl_->ctx->event_bus()->publish(Events::ActorUnloadFinishedEvent{e.scene, e.actor});
        impl_->ctx->event_bus()->publish(
            Events::ActorResidencyChangedEvent{e.scene, e.actor, /*loaded=*/false});
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
// LRU ActorCache：ensure + evict/restore 事件处理
// ============================================================================

void GeometrySystem::Impl::ensure_actor_cache() {
    if (actor_cache) return;
    if (actor_cache_dir.empty()) {
        // 默认目录：可执行文件同级的 cache/actors/
        actor_cache_dir = std::filesystem::current_path() / "cache" / "actors";
    }
    actor_cache = std::make_unique<Corona::Cache::ActorCache>(
        kDefaultMemCacheBytes,
        kDefaultDiskCacheBytes,
        actor_cache_dir);
    actor_cache->set_evict_callback(
        [](std::uintptr_t actor, const std::string&) {
            CFW_LOG_WARNING("[GeometrySystem] Actor {} snapshot evicted from ActorCache, "
                           "next restore will fall back to disk reimport", actor);
        });
    CFW_LOG_NOTICE("[GeometrySystem] ActorCache initialized: mem={}MB disk={}MB dir={}",
                   kDefaultMemCacheBytes / (1024 * 1024),
                   kDefaultDiskCacheBytes / (1024 * 1024),
                   actor_cache_dir.string());
}

// ============================================================================
// LOD 磁盘缓存 — 延迟初始化 + 后台异步写入线程
// ============================================================================

void GeometrySystem::Impl::ensure_lod_disk_cache() {
    // std::call_once 保证即使将来从多线程调用也只初始化一次
    std::call_once(lod_disk_cache_once, [this]() {
        auto dir = actor_cache_dir.empty()
            ? std::filesystem::current_path() / "cache" / "lod"
            : actor_cache_dir / "lod";

        lod_disk_cache = std::make_unique<Corona::Cache::CacheManager>(
            128 * 1024 * 1024,   // 128MB 内存缓存
            512 * 1024 * 1024,   // 512MB 磁盘缓存
            dir);

        // 淘汰回调仅做诊断日志。此时对应 Scene::lod_levels 的 vector 已被
        // std::move 清空（RAM 已释放），磁盘副本被 LRU 淘汰后该级数据即不可
        // 恢复——GPU 侧无法再重建该级缓冲，渲染继续使用已驻留的级（通常
        // LOD0），直到模型重新导入。容量配置应确保此情形罕见。
        lod_disk_cache->set_evict_callback(
            [](const std::string& key, const std::vector<char>&) {
                CFW_LOG_NOTICE("[LOD-Disk] LRU 淘汰: {} (该级数据不再可恢复)", key);
            });

        // 启动后台序列化+写盘线程
        lod_disk_worker_running = true;
        lod_disk_worker = std::make_unique<std::thread>([this]() {
            lod_disk_writer_loop();
        });

        CFW_LOG_NOTICE("[LOD-Disk] 缓存已初始化: dir={} 内存=128MB 磁盘=512MB",
                       dir.string());
    });
}

void GeometrySystem::Impl::lod_disk_writer_loop() {
    CFW_LOG_NOTICE("[LOD-Disk] 写入线程已启动");

    while (lod_disk_worker_running.load(std::memory_order_acquire)) {
        LodDiskWriteTask task;

        {
            std::unique_lock lock(lod_disk_write_mutex);
            lod_disk_write_cv.wait(lock, [this]() {
                return !pending_lod_disk_writes.empty()
                    || !lod_disk_worker_running.load(std::memory_order_acquire);
            });

            if (!lod_disk_worker_running.load(std::memory_order_acquire)) {
                // 关闭中：排空剩余任务后再退出（含重试逻辑，与正常路径一致）
                while (!pending_lod_disk_writes.empty()) {
                    task = std::move(pending_lod_disk_writes.front());
                    pending_lod_disk_writes.pop_front();
                    lod_disk_write_inflight_key = task.key;
                    lock.unlock();
                    bool ok = write_one_lod_record(task);
                    lock.lock();
                    lod_disk_write_inflight_key.clear();

                    if (!ok && task.retry_count < Impl::kMaxLodDiskWriteRetries) {
                        ++task.retry_count;
                        CFW_LOG_WARNING("[LOD-Disk] 关闭时写入失败，重试 {}/{}: {}",
                                       task.retry_count, Impl::kMaxLodDiskWriteRetries,
                                       task.key);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        pending_lod_disk_writes.push_front(std::move(task));
                        continue;
                    }
                    if (!ok) {
                        CFW_LOG_ERROR("[LOD-Disk] 关闭时写入失败已达最大重试次数，丢弃: {}"
                                      " (model={:#x} mesh={} lod={})",
                                      task.key, task.model_id, task.mesh_index,
                                      task.lod_level);
                    }
                }
                break;
            }

            task = std::move(pending_lod_disk_writes.front());
            pending_lod_disk_writes.pop_front();
            // 标记在写 key：restore 侧在"已出队、blob 未写完"窗口内跳过磁盘回读
            lod_disk_write_inflight_key = task.key;
        }  // 解锁后再写盘，避免长时间持锁阻塞几何线程入队

        bool write_ok = write_one_lod_record(task);
        {
            std::lock_guard clear_lock(lod_disk_write_mutex);
            lod_disk_write_inflight_key.clear();
        }

        // 写盘失败重试：瞬时错误（磁盘满、CacheManager 内部异常等）短暂退避后重新入队。
        // 超大 blob（超过内存缓存容量）同样返回 false，但 retry 计数仍递增——超大项
        // 每次都会失败，达到上限后丢弃，避免无限循环。
        if (!write_ok && task.retry_count < Impl::kMaxLodDiskWriteRetries) {
            ++task.retry_count;
            CFW_LOG_WARNING("[LOD-Disk] 写入失败，重试 {}/{}: {}",
                           task.retry_count, Impl::kMaxLodDiskWriteRetries, task.key);
            // 短暂退避：给瞬时错误（磁盘满等）恢复窗口，也避免快速空转
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            {
                std::lock_guard qlock(lod_disk_write_mutex);
                pending_lod_disk_writes.push_front(std::move(task));
            }
        } else if (!write_ok) {
            CFW_LOG_ERROR("[LOD-Disk] 写入失败已达最大重试次数，丢弃: {} "
                          "(model={:#x} mesh={} lod={}, 该级不再可恢复)",
                          task.key, task.model_id, task.mesh_index, task.lod_level);
        }
    }

    CFW_LOG_NOTICE("[LOD-Disk] 写入线程已退出");
}

bool GeometrySystem::Impl::write_one_lod_record(const LodDiskWriteTask& task) {
    // 序列化为二进制 blob（恒含 28 字节 header，不会为空）
    auto blob = serialize_lod_record(task.record);

    // 超大项保护：超过内存缓存容量 → 永久失败，不可重试
    // （该级数据量本身超过 LRU 内存级容量，无论重试多少次都无法放入，
    //   唯一副本随任务销毁，该级不再可恢复）
    if (blob.size() > lod_disk_cache->memory_capacity()) {
        CFW_LOG_WARNING("[LOD-Disk] blob {} 字节超出缓存容量 {} 字节; "
                        "丢弃 model={:#x} mesh={} lod={} (超大项，不重试)",
                        blob.size(), lod_disk_cache->memory_capacity(),
                        task.model_id, task.mesh_index, task.lod_level);
        return false;
    }

    // 写入两级缓存（可能触发 LRU 淘汰刷盘，发生在 worker 线程不阻塞几何帧循环）
    if (!lod_disk_cache->put(task.key, blob.data(), blob.size())) {
        CFW_LOG_WARNING("[LOD-Disk] 写入失败: {} (model={:#x} mesh={} lod={})",
                       task.key, task.model_id, task.mesh_index, task.lod_level);
        return false;
    }

    CFW_LOG_NOTICE("[LOD-Disk] 已写入: {} ({} 字节, model={:#x} mesh={} lod={})",
                   task.key, blob.size(), task.model_id, task.mesh_index, task.lod_level);
    return true;
}

void GeometrySystem::on_evict_requested(const Events::ActorEvictRequestedEvent& event) {
    impl_->ensure_actor_cache();

    auto& hub = SharedDataHub::instance();

    // 锁顺序：impl_->mtx → Storage，必须在读 Storage 之前获取
    std::unique_lock lock(impl_->mtx);

    const auto now = std::chrono::steady_clock::now();

    // ---- 第 1 步：防抖检查（5 秒内不重复快照） ----
    {
        auto it = impl_->last_snapshot_time.find(event.actor);
        if (it != impl_->last_snapshot_time.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);
            if (elapsed.count() < 5) return;
        }
    }

    // ---- 第 2 步：读取 actor 完整状态，构建 ActorStreamingRecord ----
    auto actor_read = hub.actor_storage().try_acquire_read(event.actor);
    if (!actor_read) return;

    // pinned 的 actor 不淘汰
    if (actor_read->pinned) return;

    Corona::Cache::ActorStreamingRecord rec;
    rec.pinned    = actor_read->pinned;
    rec.scene     = event.scene;
    rec.actor     = event.actor;
    rec.model_path    = actor_read->model_path;
    rec.follow_camera = actor_read->follow_camera;
    rec.profile_handles = actor_read->profile_handles;

    // 遍历 profiles 收集 geometry_handles / resource_ids / transform / 运行时标志
    bool transform_captured = false;
    bool optics_captured   = false;
    bool physics_captured  = false;
    std::unordered_set<std::uintptr_t> seen_geoms;
    auto collect_rid = [&](std::uintptr_t geom_handle) {
        if (!seen_geoms.insert(geom_handle).second) return;  // 已处理
        auto geom = hub.geometry_storage().try_acquire_read(geom_handle);
        if (geom && geom->model_resource_handle) {
            auto mr = hub.model_resource_storage().try_acquire_read(geom->model_resource_handle);
            if (mr && mr->model_id != 0) {
                rec.resource_ids.push_back(mr->model_id);
            }
        }
    };

    for (auto profile_handle : actor_read->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;

        // 从 profile.geometry_handle 收集
        if (profile->geometry_handle && !seen_geoms.count(profile->geometry_handle)) {
            rec.geometry_handles.push_back(profile->geometry_handle);
            collect_rid(profile->geometry_handle);
        }

        // 从 profile.mechanics_handle → geometry 收集
        if (profile->mechanics_handle) {
            auto mech = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle);
            if (mech) {
                if (!physics_captured) {
                    rec.physics_enabled = mech->physics_enabled;
                    physics_captured = true;
                }
                if (mech->geometry_handle && !seen_geoms.count(mech->geometry_handle)) {
                    rec.geometry_handles.push_back(mech->geometry_handle);
                    collect_rid(mech->geometry_handle);
                    if (!transform_captured) {
                        auto geom = hub.geometry_storage().try_acquire_read(mech->geometry_handle);
                        if (geom && geom->transform_handle) {
                            auto xform = hub.model_transform_storage().try_acquire_read(geom->transform_handle);
                            if (xform) {
                                rec.transform = *xform;
                                transform_captured = true;
                            }
                        }
                    }
                }
            }
        }

        // 从首个有效 profile 收集 optics_visible
        if (!optics_captured && profile->optics_handle) {
            auto optics = hub.optics_storage().try_acquire_read(profile->optics_handle);
            if (optics) {
                rec.optics_visible = optics->visible;
                optics_captured = true;
            }
        }
    }

    // ---- 第 3 步：存入 ActorCache ----
    if (!impl_->actor_cache->put(event.actor, rec)) {
        CFW_LOG_ERROR("[GeometrySystem] Failed to cache actor {} stream record", event.actor);
    }

    // ---- 第 4 步：标记 offline + 延迟 GPU 释放 ----
    impl_->offline_actors[event.actor] = true;
    impl_->pending_gpu_releases.insert(event.actor);
    auto scene_it = impl_->scenes.find(event.scene);
    if (scene_it != impl_->scenes.end()) {
        scene_it->second.actor_load_states[event.actor] = ActorLoadState::Unloaded;
    }
    impl_->last_snapshot_time[event.actor] = now;

    // ---- 第 5 步：cascade 淘汰底层资源（仅非 gpu_only）----
    // gpu_only=true（VRAM 压力路径）：跳过 cascade，保留 Scene/Image 的 CPU 内存。
    //   GPU 压力绝不清 CPU；且保留 CPU 使后续可快速从 CPU 重建 GPU（无需磁盘重导）。
    // gpu_only=false（不可见帧 / RAM 压力路径）：级联 try_evict 释放底层 CPU。
    if (!event.gpu_only && !rec.resource_ids.empty()) {
        auto& rm = Resource::ResourceManager::get_instance();
        for (auto rid : rec.resource_ids) {
            auto result = rm.try_evict(rid);
            if (result.success) {
                CFW_LOG_NOTICE("[GeometrySystem] Cascade evicted resource {} ({} bytes) "
                               "for evicted actor {}", rid, result.bytes_freed, event.actor);
            }
        }
    }

    CFW_LOG_NOTICE("[GeometrySystem] Actor {} evicted: cached stream record "
                   "({} profiles, {} geometries, {} resource_ids, path={}, "
                   "physics={}, optics_visible={}, follow_camera={})",
                   event.actor,
                   rec.profile_handles.size(), rec.geometry_handles.size(),
                   rec.resource_ids.size(), rec.model_path.string(),
                   rec.physics_enabled, rec.optics_visible, rec.follow_camera);

    lock.unlock();
    if (impl_->ctx && impl_->ctx->event_bus()) {
        impl_->ctx->event_bus()->publish(Events::ActorResidencyChangedEvent{
            event.scene, event.actor, /*loaded=*/false});
    }
}

void GeometrySystem::on_restore_requested(const Events::ActorRestoreRequestedEvent& event) {
    impl_->ensure_actor_cache();

    // 防抖：2 秒内刚被 evict 的不 restore，避免 preload/unload 边界反复横跳
    {
        auto it = impl_->last_snapshot_time.find(event.actor);
        if (it != impl_->last_snapshot_time.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - it->second);
            if (elapsed.count() < 2) return;
        }
    }

    auto& hub = SharedDataHub::instance();

    // ---- 第 1 步：从 ActorCache 获取流式记录 ----
    auto rec = impl_->actor_cache->get(event.actor);
    std::filesystem::path model_path;

    if (rec) {
        model_path = rec->model_path;
        CFW_LOG_NOTICE("[GeometrySystem] Restoring actor {} from cache: "
                       "path={}, profiles={}, geometries={}, resource_ids={}, "
                       "follow_camera={}, physics_enabled={}, optics_visible={}, priority={}",
                       event.actor, model_path.string(),
                       rec->profile_handles.size(), rec->geometry_handles.size(),
                       rec->resource_ids.size(), rec->follow_camera,
                       rec->physics_enabled, rec->optics_visible, rec->priority);
    } else {
        // 缓存未命中：回退到从 ActorDevice 读取 model_path
        auto actor_read = hub.actor_storage().try_acquire_read(event.actor);
        if (!actor_read) {
            CFW_LOG_ERROR("[GeometrySystem] Restore failed: actor {} not in storage", event.actor);
            return;
        }
        model_path = actor_read->model_path;
        CFW_LOG_NOTICE("[GeometrySystem] Restoring actor {} from disk (cache miss, path={})",
                       event.actor, model_path.string());
    }

    if (model_path.empty()) {
        CFW_LOG_ERROR("[GeometrySystem] Restore failed: actor {} has empty model path", event.actor);
        return;
    }

    // ---- 第 1.5 步：恢复运行时状态到 SharedDataHub ----
    // evict 时存入 ActorStreamingRecord 的 transform / physics_enabled /
    // optics_visible 在 restore 时写回对应的存储槽位，保证 actor 恢复后
    // 渲染/物理看到的是 evict 前的状态而非默认值。
    if (rec) {
        auto actor_read = hub.actor_storage().try_acquire_read(event.actor);
        if (actor_read) {
            for (auto ph : actor_read->profile_handles) {
                auto profile = hub.profile_storage().try_acquire_read(ph);
                if (!profile) continue;

                // 恢复 transform（通过 geometry → transform_handle）
                if (profile->geometry_handle) {
                    auto geom = hub.geometry_storage().try_acquire_read(
                        profile->geometry_handle);
                    if (geom && geom->transform_handle) {
                        auto xform = hub.model_transform_storage().try_acquire_write(
                            geom->transform_handle);
                        if (xform) {
                            *xform = rec->transform;
                        }
                    }
                }

                // 恢复 physics_enabled
                if (profile->mechanics_handle) {
                    auto mech = hub.mechanics_storage().try_acquire_write(
                        profile->mechanics_handle);
                    if (mech) {
                        mech->physics_enabled = rec->physics_enabled;
                    }
                }

                // 恢复 optics_visible
                if (profile->optics_handle) {
                    auto optics = hub.optics_storage().try_acquire_write(
                        profile->optics_handle);
                    if (optics) {
                        optics->visible = rec->optics_visible;
                    }
                }
            }
        }
        // CFW_LOG_NOTICE("[GeometrySystem] Restored actor {} state: pos=({:.1f},{:.1f},{:.1f}) "
        //                "physics={} optics_visible={}",
        //                event.actor,
        //                rec->transform.position.x, rec->transform.position.y,
        //                rec->transform.position.z,
        //                rec->physics_enabled, rec->optics_visible);
    }

    // ---- 第 2 步：检查是否已在加载/卸载中，然后启动异步导入 ----
    {
        std::unique_lock lock(impl_->mtx);
        auto scene_it = impl_->scenes.find(event.scene);
        if (scene_it == impl_->scenes.end()) return;
        auto& scene_state = scene_it->second;

        if (scene_state.loading_tasks.count(event.actor) ||
            scene_state.unloading_tasks.count(event.actor)) {
            CFW_LOG_NOTICE("[GeometrySystem] Restore: actor {} already in transition, skip",
                           event.actor);
            return;
        }

        // 如果该 actor 还在待释放 GPU 队列中，移除（不需要释放了）
        impl_->pending_gpu_releases.erase(event.actor);

        scene_state.actor_load_states[event.actor] = ActorLoadState::Loading;
        // pin 住资源防止加载中被预算淘汰踢掉，on_load_finished 会 unpin
        {
            auto norm = model_path.is_relative()
                ? std::filesystem::absolute(model_path) : model_path;
            std::error_code ec;
            norm = std::filesystem::weakly_canonical(norm, ec);
            if (!ec) {
                auto rid = Resource::IResource::generate_uid(norm);
                Resource::ResourceManager::get_instance().pin(rid);
            }
        }
        scene_state.loading_tasks[event.actor] =
            Resource::ResourceManager::get_instance().import_async(model_path);
    }

    CFW_LOG_NOTICE("[GeometrySystem] Restore started for actor {} ({})", event.actor,
                   model_path.string());
}

// ============================================================================
// 动态减面 (Mesh Simplification) 公共 API
// ============================================================================

GeometrySystem::RenderMeshBuffers GeometrySystem::select_render_buffers(
    std::uintptr_t          geometry_handle,
    uint32_t                mesh_index,
    const ktm::fvec3&       camera_pos,
    float                   camera_fov_deg,
    const ktm::fvec3&       world_center,
    float                   bounding_radius,
    const RenderMeshBuffers& fallback) const {

    // 选级权已完全交给 reconcile_lod_residency（方案 B 滞回）：本路径直接读
    // committed_demand，不再按屏占比选级，故无需在此 compute_screen_ratio。
    // camera_pos / camera_fov_deg / world_center / bounding_radius 保留在签名中
    // 以兼容调用方与 resident 路径对称，本实现不再使用。
    (void)camera_pos; (void)camera_fov_deg; (void)world_center; (void)bounding_radius;

    // ------------------------------------------------------------------
    // 选级 + 降级 + 句柄拷贝全部在同一个 shared_lock 作用域内完成。
    //
    // 不再调用 resolve_lod_buffers()（其返回裸 const LODMeshBuffers*，调用方在锁
    // 释放后解引用——逐级 LOD 淘汰使缓存频繁增删后会触发悬垂访问）。这里改为持锁
    // 期间把选中级的 HardwareBuffer 句柄（引用计数，可拷贝）拷入返回值，锁外仅持有
    // 句柄副本：即便下一帧该级被释放，本帧拷走的副本仍经 refcount 存活到用完。
    //
    // fallback 语义：调用方持 geom 槽锁时从 MeshDevice 读出的 LOD0 候选缓冲。
    // - 无 LOD 缓存条目（如 from_image 程序化几何）→ 原样返回 fallback。
    // - LOD0 候选被释放（Tier2 降级）→ fallback 缓冲为空句柄，由各级常驻判断接管。
    // ------------------------------------------------------------------
    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it  = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) {
        return fallback;  // 无 LOD 数据：原样返回（保证始终可渲染）
    }
    const auto& levels = it->second.levels;

    // 1) 直接读 reconcile 已提交的滞回级（方案 B）。
    //    不再独立调用 select_lod_level —— 那会让 render 选到 reconcile 未驻留的级、
    //    被迫降级到 LOD0（与 reconcile 决策脱钩，反而放大视觉 pop）。
    //    committed_demand 由 reconcile_lod_residency 用滞回死区每帧维护，
    //    并保证 {LOD0, committed_demand} 两级 GPU 缓冲已建好。
    int selected = it->second.committed_demand;
    if (selected < 0) selected = 0;
    if (static_cast<size_t>(selected) >= levels.size())
        selected = static_cast<int>(levels.size()) - 1;

    // 2) committed 级未就绪时，单调回退到"不比 committed 更粗的最近已就绪级"（Fix 2）。
    //    旧实现做双向就近搜索：committed 未就绪时可能选到更粗的已驻级，而目标（更细）级
    //    一旦异步 build 完成又跳回——构建窗口内渲染级在粗/细之间逐帧来回 = 闪烁。
    //    改为只向更细方向（更小级号）搜索：LOD0 恒驻，必然命中一个 ready 级，且渲染级
    //    "永不比 committed 更粗"。构建期间显示的细级只会随目标 build 完成单调变粗、绝不反弹，
    //    彻底消除回退抖动。代价：粗级 build 完成前短暂多画几帧更细网格（视觉无 pop，仅微小带宽）。
    //    committed 级本身 ready 时此分支零开销跳过。
    if (static_cast<size_t>(selected) < levels.size() && !levels[selected].ready) {
        int best = -1;
        for (int lo = selected - 1; lo >= 0; --lo) {
            if (levels[lo].ready) { best = lo; break; }
        }

        // 向更细方向搜索失败时（含 selected==0 的情形），检查 swap 保活级。
        // swap_in_progress=true 时 prev_committed 由 process_pending_lod_builds 保证 ready，
        // 覆盖"demand=0 且 LOD0 已卸载"场景：LOD0 rebuild 完成前用 prev 渲染，不中断。
        if (best < 0 && it->second.swap_in_progress) {
            const int guard = it->second.prev_committed;
            if (guard >= 0
                && static_cast<size_t>(guard) < levels.size()
                && levels[guard].ready) {
                best = guard;
            }
        }

        selected = best;  // 仍为 -1 → 下方回退 fallback
    }
    if (selected < 0 || static_cast<size_t>(selected) >= levels.size()) {
        return fallback;
    }

    // 3) 选中级仍未就绪（含 selected==0 但 LOD0 未常驻的情形）→ 回退 fallback
    const LODMeshBuffers& level = levels[selected];
    if (!level.ready || !level.vertex_buffer || !level.index_buffer) {
        return fallback;
    }

    // 4) 持锁拷出句柄（值语义）。StorageBuffer 缺失时沿用 fallback，避免 compute 拿空句柄。
    RenderMeshBuffers out;
    out.vertex         = level.vertex_buffer;
    out.index          = level.index_buffer;
    out.vertex_storage = level.vertex_storage ? level.vertex_storage : fallback.vertex_storage;
    out.index_storage  = level.index_storage  ? level.index_storage  : fallback.index_storage;
    out.vertex_count   = level.vertex_count;
    out.index_count    = level.index_count;
    out.max_index      = level.max_index;
    return out;
}

GeometrySystem::RenderMeshBuffers GeometrySystem::select_shadow_render_buffers(
    std::uintptr_t geometry_handle, uint32_t mesh_index,
    float world_units_per_texel, float max_abs_scale,
    const RenderMeshBuffers& fallback) const {
    std::shared_lock lock(impl_->lod_cache_mutex);
    auto it = impl_->lod_cache.find(Impl::make_lod_key(geometry_handle, mesh_index));
    if (it == impl_->lod_cache.end() || it->second.levels.empty()) return fallback;
    const auto& levels = it->second.levels;
    int target = it->second.committed_demand;
    if (world_units_per_texel > 0.0f && std::isfinite(world_units_per_texel)) {
        target = 0;
        const float scale = std::max(std::abs(max_abs_scale), 1.0e-6f);
        for (int level = 0; level < static_cast<int>(levels.size()); ++level) {
            const float error = levels[level].geometric_error * scale;
            if (std::isfinite(error) && error <= world_units_per_texel) target = level;
        }
    }
    target = std::clamp(target, 0, static_cast<int>(levels.size()) - 1);
    for (int level = target; level >= 0; --level) {
        const auto& candidate = levels[static_cast<size_t>(level)];
        if (!candidate.ready || !candidate.vertex_buffer || !candidate.index_buffer) continue;
        return RenderMeshBuffers{candidate.vertex_buffer, candidate.index_buffer,
            candidate.vertex_storage ? candidate.vertex_storage : fallback.vertex_storage,
            candidate.index_storage ? candidate.index_storage : fallback.index_storage,
            candidate.vertex_count, candidate.index_count, candidate.max_index};
    }
    return fallback;
}

GeometrySystem::RenderMeshBuffers GeometrySystem::resident_render_buffers(
    std::uintptr_t           geometry_handle,
    uint32_t                 mesh_index,
    const RenderMeshBuffers& fallback) const {

    // ------------------------------------------------------------------
    // 仅做"常驻路由"，不做屏幕占比选级（无相机入参）。
    // 用途：无相机上下文的渲染路径（如 V-buffer 可见性主路径 / actor 拾取），
    // 这些路径需要"读到当前实际常驻的几何缓冲"，而非按距离选 LOD。
    //
    // 策略：从 LOD0 向高精度方向扫描，返回**最高精度的已就绪级**的句柄拷贝。
    //   - 今天 LOD0 恒常驻 → 返回 LOD0（= fallback 同一批句柄），行为零变化。
    //   - Tier2 降级释放 LOD0 后 → 自动路由到下一个已就绪级，主路径不至于读到空缓冲。
    //   - 无 LOD 缓存条目（from_image 程序化几何）→ 原样返回 fallback。
    //
    // 与 select_render_buffers 同样：选级 + 句柄拷贝全部在单个 shared_lock 内完成，
    // 锁外仅持有引用计数句柄副本，无悬垂、无对 geom 槽锁的再入。
    // ------------------------------------------------------------------
    std::shared_lock lock(impl_->lod_cache_mutex);
    auto key = Impl::make_lod_key(geometry_handle, mesh_index);
    auto it  = impl_->lod_cache.find(key);
    if (it == impl_->lod_cache.end()) {
        return fallback;  // 无 LOD 数据：原样返回
    }
    const auto& levels = it->second.levels;

    // 从 LOD0 向后扫，取第一个 ready 且缓冲有效的级（= 最高精度常驻级）
    for (size_t i = 0; i < levels.size(); ++i) {
        const LODMeshBuffers& level = levels[i];
        if (!level.ready || !level.vertex_buffer || !level.index_buffer) {
            continue;
        }
        RenderMeshBuffers out;
        out.vertex         = level.vertex_buffer;
        out.index          = level.index_buffer;
        out.vertex_storage = level.vertex_storage ? level.vertex_storage : fallback.vertex_storage;
        out.index_storage  = level.index_storage  ? level.index_storage  : fallback.index_storage;
        out.vertex_count   = level.vertex_count;
        out.index_count    = level.index_count;
        out.max_index      = level.max_index;
        return out;
    }

    // 全级皆不常驻：返回 fallback（其几何缓冲可能为空 → 调用方据此跳过该 mesh）
    return fallback;
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

std::vector<std::pair<Horizon::HardwareBuffer, Horizon::HardwareBuffer>>
GeometrySystem::get_skinning_targets(std::uintptr_t geometry_handle, uint32_t mesh_index) const {
    // 单次 shared_lock 拷出该 mesh 所有 LOD 级别的 (vertex, vertex_storage) 句柄对。
    // 句柄为引用计数拷贝，拷出后保活底层 buffer，供 MechanicsSystem 锁外 write_bytes。
    std::vector<std::pair<Horizon::HardwareBuffer, Horizon::HardwareBuffer>> out;
    std::shared_lock lock(impl_->lod_cache_mutex);
    auto it = impl_->lod_cache.find(Impl::make_lod_key(geometry_handle, mesh_index));
    if (it == impl_->lod_cache.end()) return out;
    out.reserve(it->second.levels.size());
    for (auto& lvl : it->second.levels) {
        out.emplace_back(lvl.vertex_buffer, lvl.vertex_storage);
    }
    return out;
}

// ============================================================================
// 动态减面内部管线
// ============================================================================

namespace {

// 收集所有场景所有相机的世界位置（用于流式加载的距离排序）。
[[nodiscard]] std::vector<ktm::fvec3> collect_camera_positions() {
    auto& hub = SharedDataHub::instance();
    auto& camera_storage = hub.camera_storage();
    std::vector<ktm::fvec3> positions;
    for (auto sit = hub.scene_storage().cbegin(); sit != hub.scene_storage().cend(); ++sit) {
        for (std::uintptr_t cam_handle : sit->camera_handles) {
            if (auto cam = camera_storage.try_acquire_read_nowait(cam_handle)) {
                positions.push_back(cam->position);
            }
        }
    }
    return positions;
}

// 点 p 到最近相机的距离平方；无相机时返回 0（不影响排序稳定性）。
[[nodiscard]] float nearest_camera_dist2(const ktm::fvec3& p,
                                         const std::vector<ktm::fvec3>& cameras) {
    if (cameras.empty()) return 0.0f;
    float best = std::numeric_limits<float>::max();
    for (const auto& c : cameras) {
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        best = std::min(best, dx * dx + dy * dy + dz * dz);
    }
    return best;
}

}  // namespace

void GeometrySystem::process_pending_geometry_imports() {
    auto& hub = SharedDataHub::instance();
    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& geom_storage = hub.geometry_storage();

    // ---- 阶段 1：轮询已完成的 import 任务 ----
    // future 就绪 → 取 model_id → 写入 ModelResource 槽 → 转 PendingBuild。
    // 失败（rid==0）→ 转 Failed，不再重试。
    {
        std::vector<Impl::Payload> done_handles;
        for (auto& [geom_handle, task] : impl_->pending_import_tasks) {
            if (task.future.valid() &&
                task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                done_handles.push_back(geom_handle);
            }
        }
        for (auto geom_handle : done_handles) {
            const std::uint64_t task_epoch = impl_->pending_import_tasks[geom_handle].epoch;
            std::uint64_t rid = impl_->pending_import_tasks[geom_handle].future.get();
            impl_->pending_import_tasks.erase(geom_handle);

            // 第 1 步：更新 geometry 状态 / model_id（geom_write 作用域内完成并尽早释放，
            // 避免与下方 mechanics 槽锁同时持有 → 杜绝跨槽锁序倒置）。
            bool import_ok = false;
            {
                auto geom_write = geom_storage.try_acquire_write(geom_handle);
                if (!geom_write.valid()) continue;  // geometry 已销毁

                // 防 slot 复用 ABA：本槽可能在 import 在途期间被 deallocate→复用为新对象
                // （allocate 时 T{} 把 import_epoch 重置为 0 或新对象有自己的 epoch）。
                // epoch 不符说明这已不是当初发起 import 的那个对象，丢弃本次结果。
                if (geom_write->import_epoch != task_epoch) {
                    CFW_LOG_NOTICE("[GeometrySystem] Stale async import discarded for geometry {} "
                                   "(epoch {} != {}, slot reused)",
                                   geom_handle, task_epoch, geom_write->import_epoch);
                    continue;
                }

                if (rid == Resource::IResource::INVALID_UID || rid == 0) {
                    CFW_LOG_ERROR("[GeometrySystem] Async import failed for geometry {} (path={})",
                                  geom_handle, geom_write->model_path_utf8);
                    geom_write->gpu_build_state = GeometryDevice::GpuBuildState::Failed;
                    geom_write->import_epoch = 0;  // 任务结束
                    continue;
                }

                if (geom_write->model_resource_handle) {
                    if (auto mr = hub.model_resource_storage().try_acquire_write(geom_write->model_resource_handle)) {
                        mr->model_id = rid;
                    }
                }
                geom_write->gpu_build_state = GeometryDevice::GpuBuildState::PendingBuild;
                geom_write->import_epoch = 0;  // import 阶段完成，清零
                import_ok = true;
            }  // geom_write 释放
            if (!import_ok) continue;

            // 第 2 步：回填 MechanicsDevice AABB（geometry 槽锁已释放）。
            // Mechanics 在 Python 同步构造时读到 model_id=0 → AABB 为 0；
            // 这里用 Scene 的 AABB 回填，八叉树下帧重建即自愈。
            // 注意：Storage 迭代器在当前槽持有锁，不能在迭代中对同槽再 acquire_write
            // （会重入死锁）。故先 const 遍历收集句柄，迭代结束后再写。
            if (auto scene = resource_manager.acquire_read<Resource::Scene>(rid)) {
                const auto aabb_min = scene->get_scene_aabb().min;  // std::array<float,3>
                const auto aabb_max = scene->get_scene_aabb().max;
                auto& mech_storage = hub.mechanics_storage();

                std::vector<std::uintptr_t> mech_handles;
                for (auto mit = mech_storage.cbegin(); mit != mech_storage.cend(); ++mit) {
                    if (mit->geometry_handle == geom_handle) {
                        mech_handles.push_back(reinterpret_cast<std::uintptr_t>(&(*mit)));
                    }
                }
                for (std::uintptr_t mh : mech_handles) {
                    if (auto mw = mech_storage.try_acquire_write(mh)) {
                        mw->min_xyz = make_fvec3(aabb_min[0], aabb_min[1], aabb_min[2]);
                        mw->max_xyz = make_fvec3(aabb_max[0], aabb_max[1], aabb_max[2]);
                    }
                }
            }

            CFW_LOG_NOTICE("[GeometrySystem] Async import finished for geometry {} (rid={})",
                           geom_handle, rid);
        }
    }

    // ---- 阶段 2：为新的 PendingImport 发起 import_async（距离排序 + 每帧预算）----
    // 仅对尚无在途任务的 PendingImport geometry 发起，避免重复 import。
    // 近处对象先 import（流式体验），且每帧最多发起 kMaxImportsPerFrame 个，
    // 避免一次性把大量模型全压进 TBB 线程池造成内存/解析突发。
    struct ToImport {
        std::uintptr_t geom_handle;
        std::string    path;
        ktm::fvec3     world_pos;
    };
    std::vector<ToImport> to_import;
    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        const GeometryDevice& geom_dev = *it;
        if (geom_dev.gpu_build_state != GeometryDevice::GpuBuildState::PendingImport) continue;
        auto geom_handle = reinterpret_cast<std::uintptr_t>(&geom_dev);
        if (impl_->pending_import_tasks.count(geom_handle)) continue;  // 已在途
        if (geom_dev.model_path_utf8.empty()) {
            CFW_LOG_WARNING("[GeometrySystem] Skipping PendingImport geometry {:#x} — model_path is empty",
                            geom_handle);
            continue;
        }

        ktm::fvec3 world_pos = make_fvec3(0.0f, 0.0f, 0.0f);
        if (geom_dev.transform_handle != 0) {
            if (auto tr = hub.model_transform_storage().try_acquire_read(geom_dev.transform_handle)) {
                world_pos = tr->position;
            }
        }
        to_import.push_back({geom_handle, geom_dev.model_path_utf8, world_pos});
    }
    if (to_import.empty()) return;

    // 按到最近相机的距离排序：近处先 import
    const std::vector<ktm::fvec3> cameras = collect_camera_positions();
    if (!cameras.empty()) {
        std::sort(to_import.begin(), to_import.end(),
                  [&](const ToImport& a, const ToImport& b) {
                      return nearest_camera_dist2(a.world_pos, cameras)
                           < nearest_camera_dist2(b.world_pos, cameras);
                  });
    }

    // 每帧发起预算：剩余的下帧继续（仍为 PendingImport，不丢失）
    constexpr size_t kMaxImportsPerFrame = 4;
    size_t launched = 0;
    for (auto& item : to_import) {
        if (launched >= kMaxImportsPerFrame) break;

        // 分配 epoch 并写入 GeometryDevice（防 slot 复用 ABA）。
        // 写锁内再校验仍为 PendingImport 且无 epoch，避免与并发状态变更竞争。
        const std::uint64_t epoch = impl_->next_import_epoch++;
        {
            auto geom_write = geom_storage.try_acquire_write(item.geom_handle);
            if (!geom_write.valid()) continue;  // 已销毁
            if (geom_write->gpu_build_state != GeometryDevice::GpuBuildState::PendingImport ||
                geom_write->import_epoch != 0) {
                continue;  // 状态已变 / 已有在途任务，跳过
            }
            geom_write->import_epoch = epoch;
        }

        impl_->pending_import_tasks[item.geom_handle] =
            Impl::PendingImportTask{epoch,
                                    resource_manager.import_async(Utils::utf8_to_path(item.path))};
        ++launched;
    }
}

void GeometrySystem::process_pending_geometry_builds() {
    auto& hub = SharedDataHub::instance();
    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& geom_storage = hub.geometry_storage();

    // 先回收 worker 结果。Geometry 更新线程只做短暂状态写回，不在这里创建
    // HardwareBuffer/HardwareImage，避免把 GPU 构建时间算进主更新帧。
    std::vector<std::uintptr_t> completed;
    for (const auto& [handle, task] : impl_->pending_geometry_builds) {
        if (task.future.valid() &&
            task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            completed.push_back(handle);
        }
    }
    for (const auto handle : completed) {
        auto task_it = impl_->pending_geometry_builds.find(handle);
        if (task_it == impl_->pending_geometry_builds.end()) continue;
        auto task = std::move(task_it->second);
        impl_->pending_geometry_builds.erase(task_it);
        std::vector<MeshDevice> mesh_devices;
        try {
            mesh_devices = task.future.get();
        } catch (...) {
            mesh_devices.clear();
        }
        if (mesh_devices.empty()) {
            ++impl_->diag_geometry_upload_discarded;
            CFW_LOG_WARNING("[GeometryUpload] build failed geometry={} model={}", handle, task.model_id);
            continue;
        }
        if (auto geom_write = geom_storage.try_acquire_write(handle)) {
            if (geom_write->gpu_build_state == GeometryDevice::GpuBuildState::PendingBuild &&
                geom_write->mesh_handles.empty() && geom_write->model_resource_handle != 0) {
                std::uint64_t current_model = 0;
                if (auto model = hub.model_resource_storage().try_acquire_read(geom_write->model_resource_handle)) {
                    current_model = model->model_id;
                }
                if (current_model == task.model_id) {
                    geom_write->mesh_handles = std::move(mesh_devices);
                    geom_write->gpu_build_state = GeometryDevice::GpuBuildState::Ready;
                    CFW_LOG_NOTICE("[GeometryUpload] published geometry={} meshes={} model={}",
                                   handle, geom_write->mesh_handles.size(), task.model_id);
                    ++impl_->diag_geometry_upload_published;
                } else {
                    ++impl_->diag_geometry_upload_discarded;
                }
            }
        }
    }

    // ---- 阶段 1：只读收集待构建项 ----
    // 收集 gpu_build_state==PendingBuild 且 model_id 已就绪的 geometry。
    // 仅记录 (geom_handle, model_id, world_pos)，不在持有遍历期间做重活。
    struct PendingBuild {
        std::uintptr_t geom_handle;
        std::uint64_t  model_id;
        ktm::fvec3     world_pos;  // 用于按相机距离排序（近处先建）
    };
    std::vector<PendingBuild> pending;
    std::vector<std::uintptr_t> completed_while_queued;
    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        const GeometryDevice& geom_dev = *it;
        if (geom_dev.gpu_build_state != GeometryDevice::GpuBuildState::PendingBuild) continue;
        if (!geom_dev.model_resource_handle) continue;
        const auto geom_handle = reinterpret_cast<std::uintptr_t>(&geom_dev);
        // Actor reload/rebuild may have filled the mesh handles while the
        // geometry was still marked PendingBuild. Record it and transition the
        // state after the read-only iteration; never acquire a write lock while
        // cbegin() is holding the storage read lock.
        if (!geom_dev.mesh_handles.empty()) {
            completed_while_queued.push_back(geom_handle);
            continue;
        }
        std::uint64_t model_id = 0;
        if (auto model_res = hub.model_resource_storage().try_acquire_read(geom_dev.model_resource_handle)) {
            model_id = model_res->model_id;
        }
        if (model_id == 0) continue;  // import 尚未完成，下帧再试

        // 读取世界位置（transform 缺失时退化为原点，仍可构建、只是排序权重为 0）
        ktm::fvec3 world_pos = make_fvec3(0.0f, 0.0f, 0.0f);
        if (geom_dev.transform_handle != 0) {
            if (auto tr = hub.model_transform_storage().try_acquire_read(geom_dev.transform_handle)) {
                world_pos = tr->position;
            }
        }

        pending.push_back({geom_handle, model_id, world_pos});
    }

    for (const auto geom_handle : completed_while_queued) {
        if (auto geom_write = geom_storage.try_acquire_write(geom_handle)) {
            if (geom_write->gpu_build_state == GeometryDevice::GpuBuildState::PendingBuild &&
                !geom_write->mesh_handles.empty()) {
                geom_write->gpu_build_state = GeometryDevice::GpuBuildState::Ready;
            }
        }
    }
    if (pending.empty()) return;

    // ---- 按相机距离排序：近处对象先构建（流式加载体验）----
    const std::vector<ktm::fvec3> cameras = collect_camera_positions();
    if (!cameras.empty()) {
        std::sort(pending.begin(), pending.end(),
                  [&](const PendingBuild& a, const PendingBuild& b) {
                      return nearest_camera_dist2(a.world_pos, cameras)
                           < nearest_camera_dist2(b.world_pos, cameras);
                  });
    }

    // ---- 阶段 2：异步启动 GPU 构建 ----
    // 每个 geometry 最多一个在途任务，整个系统最多一个大型模型上传，
    // 避免多个 GLB 同时争用 Vulkan 设备；当前帧只排队，不等待结果。
    for (const auto& item : pending) {
        if (impl_->pending_geometry_builds.size() >= Impl::kMaxInflightGeometryBuilds) break;
        if (impl_->pending_geometry_builds.find(item.geom_handle) != impl_->pending_geometry_builds.end()) continue;

        const auto epoch = impl_->next_geometry_build_epoch++;
        auto promise = std::make_shared<std::promise<std::vector<MeshDevice>>>();
        impl_->pending_geometry_builds.emplace(
            item.geom_handle,
            Impl::PendingGeometryBuild{item.model_id, epoch, promise->get_future()});
        impl_->geometry_build_tasks.run(
            [model_id = item.model_id, promise, &resource_manager]() {
                std::vector<MeshDevice> result;
                try {
                    auto scene = resource_manager.acquire_read<Resource::Scene>(model_id);
                    if (scene.valid()) result = build_mesh_devices_from_scene(*scene);
                } catch (...) {
                    result.clear();
                }
                promise->set_value(std::move(result));
            });
        CFW_LOG_DEBUG("[GeometryUpload] queued geometry={} model={} epoch={}",
                      item.geom_handle, item.model_id, epoch);
        ++impl_->diag_geometry_upload_queued;
        break;
    }
}

void GeometrySystem::upload_lod_from_scene_data() {
    // LOD 由本系统内部决策，无外部配置开关。
    // 最大 LOD 级别数（含 LOD0 原始精度）为内部常量；作为显存安全上限，
    // 应 >= LODGenerationOptions::max_levels + 1（默认 max_levels=8 → 9）。
    // 生成端已按 max_levels/skinned_max_levels 限制实际级数，此处仅兜底防失控。
    constexpr int max_lod_levels = 9;

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

            // 已有缓存且模型未变更则跳过（model_id 比较防止 slot 复用）。
            // 检测到变更时记下旧 model_id，锁外清理其磁盘缓存条目——
            // 避免在 lod_cache_mutex 持有期间做 CacheManager 的磁盘 IO。
            std::uint64_t stale_model_id = 0;  // 非 0 表示同一 slot 换了新模型
            std::vector<std::uint32_t> stale_extra_meshes;  // 旧模型多出的 mesh 下标
            {
                std::shared_lock lock(impl_->lod_cache_mutex);
                auto cache_it = impl_->lod_cache.find(lod_key);
                if (cache_it != impl_->lod_cache.end()) {
                    if (cache_it->second.model_id == model_id)
                        continue;
                    stale_model_id = cache_it->second.model_id;

                    // 旧模型 mesh 数可能多于新模型：多出的 mesh 不会被本循环
                    // 遍历到，其磁盘条目须在此一并收集。仅在 mesh 0（旧模型必有）
                    // 检测到变更时扫一次 lod_cache——模型重载是罕见事件，线性扫
                    // 描可接受。make_lod_key 高 32 位 = handle 低 32 位（见其定义），
                    // 据此归属到本 geometry 并提取 mesh 下标。
                    if (mesh_idx == 0) {
                        const std::uint64_t geom_hi =
                            static_cast<std::uint64_t>(geom_handle) & 0xffffffffull;
                        const auto new_mesh_count =
                            static_cast<std::uint32_t>(geom_dev.mesh_handles.size());
                        for (const auto& [k, e] : impl_->lod_cache) {
                            if ((k >> 32) != geom_hi || e.model_id != stale_model_id)
                                continue;
                            const auto old_mesh =
                                static_cast<std::uint32_t>(k & 0xffffffffull);
                            if (old_mesh >= new_mesh_count)
                                stale_extra_meshes.push_back(old_mesh);
                        }
                    }
                }
            }

            // 模型已变更（同一 geometry slot 重新导入了新模型）：旧 model_id 的
            // 磁盘缓存条目已永久失效——新模型的顶点数据与旧 blob 无任何关系，
            // 不清理则这些条目会占着 LRU 容量直到被自然淘汰。
            // 逐级 erase：max_lod_levels 与下方 GPU 侧级数上限一致，CPU 侧
            // lod_levels 级数由 LODGenerationOptions::max_levels（默认8）控制，
            // 不会超过此值；erase 不存在的 key 是无害空操作。
            if (stale_model_id != 0 && impl_->lod_disk_cache) {
                for (int lod_idx = 0; lod_idx < max_lod_levels; ++lod_idx) {
                    impl_->lod_disk_cache->erase(
                        make_lod_disk_key(stale_model_id, mesh_idx, lod_idx));
                    for (auto extra_mesh : stale_extra_meshes) {
                        impl_->lod_disk_cache->erase(
                            make_lod_disk_key(stale_model_id, extra_mesh, lod_idx));
                    }
                }
                CFW_LOG_NOTICE("[LOD-Disk] 已清理旧模型的失效缓存条目: "
                               "旧 model={:#x} mesh={} (+{} 个旧模型多出的 mesh)",
                               stale_model_id, mesh_idx, stale_extra_meshes.size());
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
            entry.residency_epoch = impl_->next_residency_epoch++;  // 身份版本（方案 C ABA 守卫）

            // per-mesh 局部 AABB（屏幕空间误差选级用）：缓存本 mesh 自身的局部包围盒，
            // 而非整场景 AABB——旧逻辑用 scene.get_scene_aabb()（所有 mesh 合并）会让大场景里的
            // 小 mesh 拿到巨大半径而恒选 LOD0。reconcile 用完整 transform 矩阵把这套局部 AABB
            // 映到世界，求相机到最近点距离（各向异性、不受轴心偏移影响）。
            // bounding_radius 保留（旧 fallback 路径仍读），= 0.5·diag(局部 AABB)。
            {
                entry.local_aabb_min = make_fvec3(mesh.aabb_min[0], mesh.aabb_min[1], mesh.aabb_min[2]);
                entry.local_aabb_max = make_fvec3(mesh.aabb_max[0], mesh.aabb_max[1], mesh.aabb_max[2]);
                const float ex = mesh.aabb_max[0] - mesh.aabb_min[0];
                const float ey = mesh.aabb_max[1] - mesh.aabb_min[1];
                const float ez = mesh.aabb_max[2] - mesh.aabb_min[2];
                float r = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
                if (!(r > 0.0f)) r = 1.0f;
                entry.bounding_radius = r;
            }
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
            lod0.vertex_count     = static_cast<std::uint32_t>(mesh.vertices.size());
            lod0.index_count      = static_cast<std::uint32_t>(mesh.indices.size());
            lod0.max_index        = mesh_dev.max_index;
            entry.levels.push_back(std::move(lod0));

            // LOD 1..N：仅登记元数据，**不**在此创建 GPU 缓冲 / BVH（Step 3a 按需驻留）。
            // 每级记录 source_lod_index（映射回 mesh.lod_levels，因为空级被跳过导致下标不连续），
            // 供 reconcile_lod_residency() 按需从 Scene CPU 即时构建该级 GPU 缓冲。
            // ready=false、缓冲为空 → 渲染选级时自动降级到 LOD0，直到该级被构建。
            for (size_t lod_idx = 0; lod_idx < mesh.lod_levels.size() && lod_idx < static_cast<size_t>(max_lod_levels - 1); ++lod_idx) {
                auto& lod_data = mesh.lod_levels[lod_idx];
                if (lod_data.vertices.empty() || lod_data.indices.empty()) continue;

                LODMeshBuffers lod_buf;  // 缓冲全空、ready=false、mesh_mem 计 0
                lod_buf.error            = lod_data.error;
                // 屏幕空间误差选级主用此值：模型空间几何误差（meshopt 相对偏差 × simplifyScale）。
                // reconcile 运行时 × actor_scale 得世界误差，再除以相机距离得角误差判级。
                lod_buf.geometric_error  = lod_data.geometric_error;
                // 切换阈值直接采用生成端按几何误差/像素预算算出的 screen_threshold
                // （generate_lod_levels 已保证严格单调递减、且 emit 的级 error>0 无死级）。
                // 防御性 clamp：异常值（<=0 或 >=1）夹回开区间，避免死级/恒选。
                {
                    float thr = lod_data.screen_threshold;
                    if (!(thr > 0.0f)) thr = 0.01f;
                    if (thr >= 1.0f)   thr = 0.99f;
                    lod_buf.screen_threshold = thr;
                }
                lod_buf.vertex_count     = static_cast<std::uint32_t>(lod_data.vertices.size());
                lod_buf.index_count      = static_cast<std::uint32_t>(lod_data.indices.size());
                lod_buf.max_index        = lod_buf.vertex_count > 0 ? lod_buf.vertex_count - 1u : 0u;
                lod_buf.source_lod_index = static_cast<std::uint32_t>(lod_idx);
                entry.levels.push_back(std::move(lod_buf));
            }

            std::unique_lock lock(impl_->lod_cache_mutex);
            impl_->lod_cache.insert_or_assign(lod_key, std::move(entry));
        }
    }
}

// ============================================================================
// reconcile_lod_residency（Step 3a：按需 LOD 驻留）
// ----------------------------------------------------------------------------
// 目标：每帧把每个 mesh 的 GPU 驻留集收敛到 {LOD0, 需求级 D}——只构建当前需要的
// 那一级，释放其余已构建的简化级，消除"上传所有 LOD ≈2× 显存"的浪费。
//
// 需求级 D：用与渲染端 select_render_buffers 完全一致的输入计算屏占比 + 选级——
//   world_center = transform.position；bounding_radius = 0.5·diag(Scene AABB)
//   （Scene AABB 即 import 时回填进 MechanicsDevice 的同一来源，故与渲染端等价）。
//   多相机取最高精度（最小级号），渲染端任一视角的需求 ≥ 此值，故经"向 LOD0 降级"
//   总能命中已构建的 D 或 LOD0，绝不会渲染到比需要更粗的网格。
//
// 线程安全：HardwareBuffer 为引用计数句柄。渲染线程经 select_render_buffers 在
//   shared_lock 内拷走句柄副本；此处在 unique_lock 内 reset 缓存中的句柄只是丢掉
//   一个引用，渲染线程本帧已拷走的副本经 refcount 存活到用完。故可直接释放，
//   无需延迟队列。GPU 缓冲构建（make_geometry_buffer）在锁外完成，仅写回时短暂加锁。
//
// 已知后续项（非本步）：物体停在某级阈值边界且持续微动时，D 会在相邻级间反复横跳
//   → 每帧重建/释放 GPU 缓冲（性能抖动，非正确性问题）。后续可加滞回/冷却帧缓解。
// ============================================================================
void GeometrySystem::reconcile_lod_residency() {
    auto& resource_manager = Resource::ResourceManager::get_instance();
    auto& hub = SharedDataHub::instance();
    auto& geom_storage = hub.geometry_storage();

    struct Lod0DeviceEviction {
        std::uintptr_t geometry_handle = 0;
        std::uintptr_t expected_model_resource_handle = 0;
        std::vector<std::uint32_t> mesh_indices;
    };
    std::vector<Lod0DeviceEviction> pending_lod0_evictions;

    // 空间淘汰恢复限速：防止相机快速移动时大量 actor 同时恢复 LOD 导致 GPU 构建洪峰
    size_t spatial_restored_this_frame = 0;

    // ---- 收集观察者 (世界位置, 角误差预算 epsilon)；无则不改动驻留 ----
    // 每个相机把「像素预算 + 自身 fov/分辨率」塌缩成单个角阈值 epsilon。选级判据
    // world_error/d ≤ epsilon 是纯球形量（与方向无关）→ 相机背后物体同样有定义，
    // 未来 GI 观察者（光源/探针）可用各自的固定 epsilon 复用同一路径与聚合逻辑。
    //
    // P0：像素预算由显存压力动态决定。VRAM 充裕时用默认 1.5px（视觉无损）；
    // VRAM 承压时逐步放宽 → 更多 mesh 自然选粗 LOD → GPU 缓冲缩小 → 显存回落，
    // 无需等到 90% 水位再暴力 evict 整个 actor。
    const float lod_pixel_budget = [this]() {
        const MemoryReport mr = compute_memory_report();
        if (mr.vram.budget_bytes == 0) return Impl::kLodDefaultPixelBudget;
        const float ratio = static_cast<float>(mr.vram.used_bytes)
                          / static_cast<float>(mr.vram.budget_bytes);
        return compute_pixel_budget_from_pressure(ratio);
    }();

    struct Viewer { ktm::fvec3 pos; float epsilon; };
    std::vector<Viewer> viewers;
    {
        auto& camera_storage = hub.camera_storage();
        for (auto sit = hub.scene_storage().cbegin(); sit != hub.scene_storage().cend(); ++sit) {
            for (std::uintptr_t cam_handle : sit->camera_handles) {
                if (auto cam = camera_storage.try_acquire_read_nowait(cam_handle)) {
                    const float eps = compute_angular_epsilon(
                        lod_pixel_budget, cam->fov,
                        static_cast<float>(cam->height));
                    viewers.push_back({cam->position, eps});
                }
            }
        }
    }
    if (viewers.empty()) return;

    // 从 viewers 提取纯相机位置（enforce_lod_budget 只需要位置做距离排序，
    // 不需要 epsilon），避免后续重复遍历 viewer 结构体。
    std::vector<ktm::fvec3> camera_positions;
    camera_positions.reserve(viewers.size());
    for (const auto& v : viewers) camera_positions.push_back(v.pos);

    // 释放冷却（方案 A）：每帧推进一次帧计数器，作为各级 last_demand_frame 的基准。
    // 仅在确有相机（会真正评估需求级）时推进，避免无相机帧空转计数。
    const std::uint64_t this_frame = ++impl_->lod_frame_counter;

    // 先结束 Geometry storage 的只读迭代，再执行下面可能需要的 Geometry 写锁。
    // ConstIterator 持有 storage 读锁；禁止在其作用域内升级同一 storage 的写锁。
    const auto geometry_handles = GeometryInternal::snapshot_storage_handles(geom_storage);

    // LOD 预算 entries 收集器。每个 (geometry, mesh) 产出 1 条，预分配为
    // geometry 数 × 8（覆盖多数多 mesh geometry 场景，减少 vector reallocation）。
    std::vector<GeometrySystem::LodBudgetEntry> lod_budget_entries;
    lod_budget_entries.reserve(geometry_handles.size() * 8);
    for (const auto geom_handle : geometry_handles) {
        std::uintptr_t model_resource_handle = 0;
        std::uintptr_t transform_handle = 0;
        std::uint32_t mesh_count = 0;
        {
            auto geom_read = geom_storage.try_acquire_read(geom_handle);
            if (!geom_read) continue;
            model_resource_handle = geom_read->model_resource_handle;
            transform_handle = geom_read->transform_handle;
            mesh_count = static_cast<std::uint32_t>(geom_read->mesh_handles.size());
        }
        if (!model_resource_handle) continue;

        std::uint64_t model_id = 0;
        if (auto model_res = hub.model_resource_storage().try_acquire_read(model_resource_handle)) {
            model_id = model_res->model_id;
        }
        if (model_id == 0) continue;

        // 取 actor transform 副本：完整矩阵把 mesh 局部 AABB 映到世界（求相机最近点距离），
        // scale_factor 把模型空间几何误差放大到世界误差。一次 acquire，下方各 mesh 复用。
        Corona::ModelTransform transform{};
        bool have_transform = false;
        float scale_factor = 1.0f;
        if (transform_handle) {
            if (auto tr = hub.model_transform_storage().try_acquire_read(transform_handle)) {
                transform = *tr;
                have_transform = true;
                scale_factor = std::max({std::abs(tr->scale.x),
                                         std::abs(tr->scale.y),
                                         std::abs(tr->scale.z)});
                if (!(scale_factor > 0.0f)) scale_factor = 1.0f;
            }
        }

        // Fix 1：不再每帧每 geom acquire_read<Scene>。半径从 lod_cache 条目读取（upload 已缓存），
        // Scene CPU 数据仅在确需构建某级时（need_build）才惰性取用。
        for (uint32_t mesh_idx = 0;
             mesh_idx < mesh_count; ++mesh_idx) {
            const uint64_t lod_key = Impl::make_lod_key(geom_handle, mesh_idx);
            ++impl_->diag_reconcile_mesh_visits;  // 诊断：处理的 (geom,mesh) 次数

            // ---- 快照：级数 + 各级世界误差 + mesh 局部 AABB ----
            // world_errors[i] = geometric_error[i]·scale_factor（世界单位）；level 0 误差恒 0。
            // 屏幕空间误差选级用：到 mesh 世界 AABB 最近点距离 d 处，角误差 = world_error/d，
            // 与相机角预算 epsilon 比较。local AABB 经完整 transform 映到世界。
            std::vector<float> world_errors;
            ktm::fvec3 local_min{0.0f, 0.0f, 0.0f};
            ktm::fvec3 local_max{0.0f, 0.0f, 0.0f};
            size_t level_count = 0;
            bool   have_error_data = false;
            bool   lod_spatially_evicted = false;
            {
                std::shared_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit == impl_->lod_cache.end() || cit->second.model_id != model_id) continue;
                level_count = cit->second.levels.size();
                lod_spatially_evicted = cit->second.lod_spatially_evicted;
                local_min = make_fvec3(cit->second.local_aabb_min[0],
                                       cit->second.local_aabb_min[1],
                                       cit->second.local_aabb_min[2]);
                local_max = make_fvec3(cit->second.local_aabb_max[0],
                                       cit->second.local_aabb_max[1],
                                       cit->second.local_aabb_max[2]);
                world_errors.reserve(level_count);
                for (size_t i = 0; i < level_count; ++i) {
                    const float ge = cit->second.levels[i].geometric_error;
                    world_errors.push_back(ge * scale_factor);
                    if (i >= 1 && ge > 0.0f) have_error_data = true;
                }
            }
            if (level_count <= 1) continue;  // 无简化级，无需 reconcile

            // ---- 需求级 D：屏幕空间误差选级，所有观察者取最高精度（最小级号）----
            // mesh 局部 AABB 经完整 transform（含 R/S/T）映到世界 → 对每个观察者求其到
            // 世界 AABB 最近点距离 d → select_lod_by_error 返回「角误差 world_error/d ≤
            // epsilon 的最粗一级」。多观察者取 min（任一视角的最高精度需求都要满足）。
            // 无几何误差数据（旧资源未含 geometric_error）→ 回退旧屏占比阈值路径。
            ktm::fvec3 world_min{0.0f, 0.0f, 0.0f};
            ktm::fvec3 world_max{0.0f, 0.0f, 0.0f};
            if (have_transform) {
                Spatial::AABB world_aabb;
                GeometryInternal::world_aabb_from_local_bounds(transform, local_min, local_max, world_aabb);
                world_min = world_aabb.min;
                world_max = world_aabb.max;
            } else {
                world_min = local_min;
                world_max = local_max;
            }

            // 计算 mesh 世界 AABB 中心，供 enforce_lod_budget 按距相机距离排序。
            // 使用 world_min/max 的算术平均——与 AABB::center() 语义一致，
            // 对细长/扁平物体也能给出合理的位置估计（不会被外接球高估距离）。
            const ktm::fvec3 world_center = GeometryInternal::make_fvec3(
                (world_min[0] + world_max[0]) * 0.5f,
                (world_min[1] + world_max[1]) * 0.5f,
                (world_min[2] + world_max[2]) * 0.5f);

            // 滞回带（替代旧 select_lod_with_hysteresis）：用收紧/放宽的 epsilon 各算一次
            // 聚合需求，构成死区。eps·(1+h) 偏粗（抗"变精细"），eps·(1-h) 偏精细（抗"变粗"）。
            // 仅当方向明确越过死区才切换，吸收相机微动时的边界抖动。
            auto aggregate_demand = [&](float eps_scale) -> int {
                int dem = static_cast<int>(level_count) - 1;
                for (const auto& v : viewers) {
                    const float d = distance_point_to_aabb(v.pos, world_min, world_max);
                    dem = std::min(dem, select_lod_by_error(d, world_errors, v.epsilon * eps_scale));
                }
                return dem;
            };

            // 读当前已提交级。
            int prev_committed = 0;
            {
                std::shared_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit == impl_->lod_cache.end() || cit->second.model_id != model_id) continue;
                prev_committed = cit->second.committed_demand;
            }

            int demand = prev_committed;

            // ---- 空间淘汰保持 / 回读恢复（在滞回之前短路）----
            bool skip_lod_demand = false;  // true = 强制 LOD0，跳过滞回+cap+阴影
            if (lod_spatially_evicted) {
                // 用本帧已算好的 world_center 判定距离
                float min_d = std::numeric_limits<float>::max();
                for (const auto& cp : camera_positions)
                    min_d = std::min(min_d, ktm::distance(world_center, cp));

                const float lod_evict_d = impl_->lod_evict_distance;

                if (min_d <= lod_evict_d) {
                    // 回到恢复区：清除标记（限速），让下方滞回正常重算 demand
                    if (spatial_restored_this_frame < Impl::kMaxLodSpatialRestorePerFrame) {
                        std::unique_lock ulock(impl_->lod_cache_mutex);
                        auto cit2 = impl_->lod_cache.find(lod_key);
                        if (cit2 != impl_->lod_cache.end() && cit2->second.model_id == model_id
                            && cit2->second.lod_spatially_evicted) {
                            cit2->second.lod_spatially_evicted = false;
                            ++spatial_restored_this_frame;
                        }
                    }
                    // 超限或已清除 → skip_lod_demand 保持 false → 滞回正常执行
                } else {
                    // 仍在淘汰区：强制 demand=0，跳过滞回+P2 cap+阴影
                    demand = 0;
                    skip_lod_demand = true;
                }
            }
            if (!skip_lod_demand) {
                if (!have_error_data) {
                    // 回退：无 geometric_error 的旧资源仍按距离选级（误差全 0 → 恒 LOD0），
                    // 实际通常 level_count<=1 已被上面拦截；此处只防御性保持 prev。
                    demand = prev_committed;
                } else {
                    const float h = Impl::kLodHysteresis;
                    const int demand_finer  = aggregate_demand(1.0f + h);
                    const int demand_coarser = aggregate_demand(1.0f - h);
                    if (demand_finer < prev_committed) {
                        demand = demand_finer;
                    } else if (demand_coarser > prev_committed) {
                        demand = demand_coarser;
                    } else {
                        demand = prev_committed;
                    }
                }
                if (demand < 0) demand = 0;
                if (static_cast<size_t>(demand) >= level_count) demand = static_cast<int>(level_count) - 1;

                // ============================================================
                // LOD 级 LRU — 应用上一帧的 budget cap + 收集本帧 entry
                // ============================================================
                // Cap 是上一帧 enforce_lod_budget 写入的"最小允许 demand"（下限），
                // 含义：demand 只能 >= cap（只能比 cap 更粗）。用 max 施加。
                //
                // 关键：必须附带 model_id 校验。若 lod_cache 条目已因 actor evict 而
                // 被 erase（slot 复用场景），同步清除陈旧 cap，防止新 actor 被旧 cap
                // 错误降级。
                {
                    auto cap_it = impl_->lod_budget_caps.find(lod_key);
                    if (cap_it != impl_->lod_budget_caps.end()) {
                        std::shared_lock cap_lock(impl_->lod_cache_mutex);
                        auto cache_it = impl_->lod_cache.find(lod_key);
                        if (cache_it == impl_->lod_cache.end()
                            || cache_it->second.model_id != model_id) {
                            impl_->lod_budget_caps.erase(cap_it);
                        } else {
                            demand = std::max(demand, cap_it->second);
                            if (static_cast<size_t>(demand) >= level_count)
                                demand = static_cast<int>(level_count) - 1;
                        }
                    }
                }
            }
            // 收集 entry 供本帧 enforce_lod_budget 使用。
            // 注意：此处记录的 demand 已是 cap 应用后的值（而非 cap 前的 natural demand）。
            // 这样 enforce_lod_budget 计算 savings 时基于"当前实际显示的级别"，
            // 而非"滞回自然需求的级别"，避免高估 savings、重复降级、过度牺牲画质。
            lod_budget_entries.push_back({lod_key, world_center, demand,
                static_cast<int>(level_count)});

            int shadow_demand = -1;
            int shadow_previous = -1;
            if (!skip_lod_demand) {
                std::unique_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id) {
                    if (this_frame > cit->second.shadow_last_request_frame &&
                        this_frame - cit->second.shadow_last_request_frame >
                            GeometryDetail::kShadowLodRequestTtlFrames) {
                        cit->second.shadow_committed_demand = -1;
                    }
                    shadow_demand = cit->second.shadow_committed_demand;
                    shadow_previous = cit->second.shadow_prev_committed;
                    if (shadow_demand >= 0 &&
                        static_cast<size_t>(shadow_demand) < cit->second.levels.size() &&
                        !cit->second.levels[shadow_demand].ready) {
                        cit->second.shadow_prev_committed =
                            cit->second.committed_demand;
                        cit->second.shadow_swap_in_progress = true;
                        shadow_previous = cit->second.shadow_prev_committed;
                    }
                }
            }

            // 限速反向（方案 D）：committed 直接跳到滞回目标，不再强制逐级移动。
            // 旧的"单帧最多移动一级"会逼着 committed 扫过每个中间级 → 每个中间级都被
            // 当成 demand 各 build 一次（哪怕只显示一帧）→ 相机平移时 GPU 上传抖动。
            // 直跳后只有"目标那一级"会触发 build，跳过所有中间级：
            //   - 接近（demand→0）：目标趋向恒驻的 LOD0，根本不 build；
            //   - 远离（demand→粗级）：仅目标粗级 build 一次，build 完成前渲染端
            //     select_render_buffers 的双向就近搜索临时显示更细的已驻级（无降级 pop、
            //     几何绝不比需要更粗），故跳级安全。
            // 代价：远离方向会从细级一次性 snap 到粗级（单次 pop），换取消除逐级 build 抖动。

            // ---- 变更 F：写回已提交级，并在需要时启动 swap 保活 ----
            // 稳态：committed_demand 对应唯一一套简化 GPU 缓冲（LOD0 永驻但只是引用计数副本）。
            // demand 跳到新级且新级未 ready → swap_in_progress=true，保留旧级供降级 fallback，
            // 直到 process_pending_lod_builds 检测到新级 ready 后立即释放旧级（swap 完成）。
            // 中途多次跳级：prev_committed 只记录首次切换时的旧级（最后一个真正显示的 ready 级），
            // 确保整个 build 窗口内渲染线程始终有一个有效缓冲可用。
            if (demand != prev_committed) {
                ++impl_->diag_demand_changes;
                CFW_LOG_NOTICE("[LOD] switch geom={} mesh={} level {} -> {} ({}) levels={}",
                               geom_handle, mesh_idx, prev_committed, demand,
                               (demand < prev_committed) ? "finer" : "coarser", level_count);
                std::unique_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id) {
                    const bool new_level_ready =
                        (static_cast<size_t>(demand) < cit->second.levels.size())
                        && cit->second.levels[demand].ready;
                    // 新级未就绪 → 启动 swap 保活（prev_committed 只设一次，保护最后一个 ready 的渲染级）
                    // 注：demand=0（LOD0 被卸载后重建）也需要保活，故去掉旧的 "demand > 0" 限制
                    if (!new_level_ready && !cit->second.swap_in_progress) {
                        cit->second.prev_committed   = prev_committed;
                        cit->second.swap_in_progress = true;
                    } else if (new_level_ready) {
                        // 新级立即就绪，无需 swap 等待
                        cit->second.swap_in_progress = false;
                        cit->second.prev_committed   = -1;
                    }
                    // else: swap 已在进行中，prev_committed 保持不变（保护最初的稳态级）
                    cit->second.committed_demand = demand;
                }
            }

            // ---- 快照：demand 是否需构建、哪些已就绪级需释放 ----
            // 变更 G：用 swap 保活守卫替代冷却帧检查（kLodReleaseCooldownFrames 已废弃）。
            // is_swap_guarded=true 时该级是正在被替换的旧级，保留供 select_render_buffers 降级；
            // 一旦新级 ready（process_pending_lod_builds 完成 swap），守卫解除，下一帧即释放。
            bool need_build = false;
            std::uint32_t demand_src_idx = 0;
            bool shadow_need_build = false;
            std::uint32_t shadow_src_idx = 0;
            std::vector<int> to_free;
            {
                std::shared_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit == impl_->lod_cache.end() || cit->second.model_id != model_id) continue;
                const auto& levels = cit->second.levels;
                const bool  in_swap = cit->second.swap_in_progress;
                const int   guarded = cit->second.prev_committed;
                if (shadow_demand > 0 && static_cast<size_t>(shadow_demand) < levels.size() &&
                    !levels[shadow_demand].ready) {
                    shadow_need_build = true;
                    shadow_src_idx = levels[shadow_demand].source_lod_index;
                }
                for (size_t i = 0; i < levels.size(); ++i) {  // i=0：LOD0 也参与 free 判断
                    if (static_cast<int>(i) == demand) {
                        if (!levels[i].ready) {
                            need_build     = true;
                            demand_src_idx = levels[i].source_lod_index;
                        }
                    } else if (levels[i].ready) {
                        // swap 保活：若该级是当前被替换的旧级，保留到新级 ready
                    const bool is_swap_guarded = in_swap && (static_cast<int>(i) == guarded);
                    const bool is_shadow_kept = static_cast<int>(i) == shadow_demand ||
                                                static_cast<int>(i) == shadow_previous;
                        if (!is_swap_guarded && !is_shadow_kept) {
                            to_free.push_back(static_cast<int>(i));
                        }
                    }
                }
            }

            // ---- 发起异步构建 demand 级（方案 C；demand>0 且未就绪）----
            if (need_build && demand > 0) {
                auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
                ++impl_->diag_scene_acquires;
                const Resource::Scene* scene_ptr = scene_read.valid() ? &(*scene_read) : nullptr;
                if (scene_ptr && mesh_idx < scene_ptr->data.meshes.size()) {
                    const auto& mesh = scene_ptr->data.meshes[mesh_idx];
                    if (demand_src_idx < mesh.lod_levels.size()) {
                    const auto& lod_data = mesh.lod_levels[demand_src_idx];
                    if (!lod_data.vertices.empty() && !lod_data.indices.empty()) {
                        std::uint64_t entry_epoch = 0;
                        {
                            std::shared_lock lk(impl_->lod_cache_mutex);
                            auto cit = impl_->lod_cache.find(lod_key);
                            if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id)
                                entry_epoch = cit->second.residency_epoch;
                        }
                        bool already = false;
                        auto pit = impl_->pending_lod_builds.find(lod_key);
                        if (pit != impl_->pending_lod_builds.end()) {
                            already = (pit->second.level == demand
                                       && pit->second.model_id == model_id
                                       && pit->second.residency_epoch == entry_epoch
                                       && pit->second.future.valid());
                            if (!already) impl_->pending_lod_builds.erase(pit);
                        }
                        if (!already && impl_->pending_lod_builds.size() < Impl::kMaxInflightLodBuilds) {
                            auto verts = lod_data.vertices;
                            auto inds  = lod_data.indices;
                            const std::size_t gpu_bytes =
                                2u * verts.size() * sizeof(Resource::Vertex) +
                                2u * inds.size() * sizeof(std::uint16_t);
                            auto promise = std::make_shared<std::promise<Impl::LODBuildResult>>();
                            impl_->pending_lod_builds[lod_key] =
                                Impl::PendingLodBuild{model_id, entry_epoch, demand, promise->get_future()};
                            impl_->lod_build_tasks.run(
                                [verts = std::move(verts), inds = std::move(inds), gpu_bytes, promise]() {
                                    Impl::LODBuildResult r;
                                    try {
                                        r.vertex_buffer = make_geometry_buffer(
                                            verts, Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Vertex, "geometry.lod_vertex");
                                        r.index_buffer = make_geometry_buffer(
                                            inds, Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Index, "geometry.lod_index");
                                        r.vertex_storage = make_geometry_buffer(
                                            verts, Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Storage, "geometry.lod_vertex_storage");
                                        r.index_storage = make_geometry_buffer(
                                            inds, Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Storage, "geometry.lod_index_storage");
                                        r.gpu_bytes = gpu_bytes;
                                        r.ok = static_cast<bool>(r.vertex_buffer)
                                               && static_cast<bool>(r.index_buffer);
                                    } catch (...) { r.ok = false; }
                                    promise->set_value(std::move(r));
                                });
                            ++impl_->diag_lod_build_launches;
                        }
                    }
                    }
                }
            }

            if (shadow_need_build && shadow_demand > 0) {
                auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
                ++impl_->diag_scene_acquires;
                const auto* scene_ptr = scene_read.valid() ? &(*scene_read) : nullptr;
                if (scene_ptr && mesh_idx < scene_ptr->data.meshes.size() &&
                    shadow_src_idx < scene_ptr->data.meshes[mesh_idx].lod_levels.size()) {
                    const auto& lod_data = scene_ptr->data.meshes[mesh_idx].lod_levels[shadow_src_idx];
                    if (!lod_data.vertices.empty() && !lod_data.indices.empty()) {
                        std::uint64_t epoch = 0;
                        { std::shared_lock lk(impl_->lod_cache_mutex); auto cit = impl_->lod_cache.find(lod_key);
                          if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id) epoch = cit->second.residency_epoch; }
                        auto pit = impl_->pending_shadow_lod_builds.find(lod_key);
                        bool already = pit != impl_->pending_shadow_lod_builds.end() &&
                            pit->second.level == shadow_demand && pit->second.model_id == model_id &&
                            pit->second.residency_epoch == epoch && pit->second.future.valid();
                        if (!already && pit != impl_->pending_shadow_lod_builds.end()) impl_->pending_shadow_lod_builds.erase(pit);
                        if (!already && impl_->pending_shadow_lod_builds.size() < Impl::kMaxInflightLodBuilds) {
                            auto verts = lod_data.vertices; auto inds = lod_data.indices;
                            const std::size_t bytes = 2u * verts.size() * sizeof(Resource::Vertex) +
                                                      2u * inds.size() * sizeof(std::uint16_t);
                            auto promise = std::make_shared<std::promise<Impl::LODBuildResult>>();
                            impl_->pending_shadow_lod_builds[lod_key] =
                                Impl::PendingLodBuild{model_id, epoch, shadow_demand, promise->get_future(), Impl::LodBuildPurpose::Shadow};
                            impl_->lod_build_tasks.run([verts=std::move(verts), inds=std::move(inds), bytes, promise]() {
                                Impl::LODBuildResult r; try {
                                    r.vertex_buffer=make_geometry_buffer(verts,Horizon::BufferUsage_TransferDst|Horizon::BufferUsage_Vertex,"geometry.shadow_lod_vertex");
                                    r.index_buffer=make_geometry_buffer(inds,Horizon::BufferUsage_TransferDst|Horizon::BufferUsage_Index,"geometry.shadow_lod_index");
                                    r.vertex_storage=make_geometry_buffer(verts,Horizon::BufferUsage_TransferSrc|Horizon::BufferUsage_TransferDst|Horizon::BufferUsage_Storage,"geometry.shadow_lod_vertex_storage");
                                    r.index_storage=make_geometry_buffer(inds,Horizon::BufferUsage_TransferSrc|Horizon::BufferUsage_TransferDst|Horizon::BufferUsage_Storage,"geometry.shadow_lod_index_storage");
                                    r.gpu_bytes=bytes; r.ok=static_cast<bool>(r.vertex_buffer)&&static_cast<bool>(r.index_buffer);
                                } catch (...) { r.ok=false; } promise->set_value(std::move(r));
                            });
                            ++impl_->diag_lod_build_launches;
                        }
                    }
                }
            }

            // ---- demand=0 且 LOD0 已被卸载 → 从 Scene CPU 数据重建 LOD0 GPU 缓冲 ----
            // LOD0 被卸载后（levels[0].ready=false），demand 回到 0 时走本路径重建。
            // 重建期间 swap 模型保活 prev_committed 级供渲染使用，不中断渲染。
            if (need_build && demand == 0) {
                auto scene_read = resource_manager.acquire_read<Resource::Scene>(model_id);
                ++impl_->diag_scene_acquires;
                const Resource::Scene* scene_ptr = scene_read.valid() ? &(*scene_read) : nullptr;
                if (scene_ptr && mesh_idx < scene_ptr->data.meshes.size()) {
                    const auto& mesh = scene_ptr->data.meshes[mesh_idx];
                    if (!mesh.vertices.empty() && !mesh.indices.empty()) {
                        std::uint64_t entry_epoch = 0;
                        {
                            std::shared_lock lk(impl_->lod_cache_mutex);
                            auto cit = impl_->lod_cache.find(lod_key);
                            if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id)
                                entry_epoch = cit->second.residency_epoch;
                        }
                        bool already = false;
                        auto pit = impl_->pending_lod_builds.find(lod_key);
                        if (pit != impl_->pending_lod_builds.end()) {
                            already = (pit->second.level == 0
                                       && pit->second.model_id == model_id
                                       && pit->second.residency_epoch == entry_epoch
                                       && pit->second.future.valid());
                            if (!already) impl_->pending_lod_builds.erase(pit);
                        }
                        if (!already && impl_->pending_lod_builds.size() < Impl::kMaxInflightLodBuilds) {
                            auto verts = mesh.vertices;  // 从 mesh 本体，非 lod_levels
                            auto inds  = mesh.indices;
                            const std::size_t gpu_bytes =
                                2u * verts.size() * sizeof(Resource::Vertex) +
                                2u * inds.size() * sizeof(std::uint16_t);
                            auto promise = std::make_shared<std::promise<Impl::LODBuildResult>>();
                            impl_->pending_lod_builds[lod_key] =
                                Impl::PendingLodBuild{model_id, entry_epoch, 0, promise->get_future()};
                            impl_->lod_build_tasks.run(
                                [verts = std::move(verts), inds = std::move(inds), gpu_bytes, promise]() {
                                    Impl::LODBuildResult r;
                                    try {
                                        r.vertex_buffer = make_geometry_buffer(
                                            verts, Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Vertex, "geometry.lod0_vertex");
                                        r.index_buffer = make_geometry_buffer(
                                            inds, Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Index, "geometry.lod0_index");
                                        r.vertex_storage = make_geometry_buffer(
                                            verts, Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Storage, "geometry.lod0_vertex_storage");
                                        r.index_storage = make_geometry_buffer(
                                            inds, Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Storage, "geometry.lod0_index_storage");
                                        r.gpu_bytes = gpu_bytes;
                                        r.ok = static_cast<bool>(r.vertex_buffer)
                                               && static_cast<bool>(r.index_buffer);
                                    } catch (...) { r.ok = false; }
                                    promise->set_value(std::move(r));
                                });
                            ++impl_->diag_lod_build_launches;
                            CFW_LOG_NOTICE("[LOD] rebuilding LOD0 from CPU data: geom={} mesh={}", geom_handle, mesh_idx);
                        }
                    }
                }
            }

            // ---- 释放多余的已就绪级（持锁；refcount 保证渲染端副本存活）----
            // 注：LOD0（lvl_idx=0）现在也参与释放判断。释放 lod_cache 引用后，
            // 延迟到整个 Geometry 只读扫描结束后再清空 mesh_dev 的独立句柄。
            bool freed_lod0 = false;
            if (!to_free.empty()) {
                std::unique_lock lock(impl_->lod_cache_mutex);
                auto cit = impl_->lod_cache.find(lod_key);
                if (cit != impl_->lod_cache.end() && cit->second.model_id == model_id) {
                    for (int lvl_idx : to_free) {
                        if (static_cast<size_t>(lvl_idx) >= cit->second.levels.size()) continue;
                        if (lvl_idx == demand) continue;  // 期间 demand 可能已变
                        auto& lvl = cit->second.levels[lvl_idx];
                        lvl.ready          = false;
                        ++impl_->diag_lod_frees;
                        lvl.vertex_buffer  = Horizon::HardwareBuffer{};
                        lvl.index_buffer   = Horizon::HardwareBuffer{};
                        lvl.vertex_storage = Horizon::HardwareBuffer{};
                        lvl.index_storage  = Horizon::HardwareBuffer{};
                        lvl.mesh_mem       = Corona::Memory::GpuMemToken{};
                        if (lvl_idx == 0) freed_lod0 = true;
                    }
                }
            }
            if (freed_lod0) {
                auto eviction = std::find_if(
                    pending_lod0_evictions.begin(), pending_lod0_evictions.end(),
                    [geom_handle](const Lod0DeviceEviction& candidate) {
                        return candidate.geometry_handle == geom_handle;
                    });
                if (eviction == pending_lod0_evictions.end()) {
                    pending_lod0_evictions.push_back(
                        Lod0DeviceEviction{geom_handle, model_resource_handle, {mesh_idx}});
                } else {
                    eviction->mesh_indices.push_back(mesh_idx);
                }
            }
        }
    }

    // All Geometry storage read iterators/accessors are out of scope here.
    // Only now may this function acquire Geometry storage write locks.
    for (const auto& eviction : pending_lod0_evictions) {
        auto geom_write = geom_storage.try_acquire_write(eviction.geometry_handle);
        if (!geom_write ||
            geom_write->model_resource_handle != eviction.expected_model_resource_handle) {
            continue;
        }
        for (const auto mesh_idx : eviction.mesh_indices) {
            if (mesh_idx >= geom_write->mesh_handles.size()) continue;
            auto& md = geom_write->mesh_handles[mesh_idx];
            md.vertexBuffer        = Horizon::HardwareBuffer{};
            md.indexBuffer         = Horizon::HardwareBuffer{};
            md.vertexStorageBuffer = Horizon::HardwareBuffer{};
            md.indexStorageBuffer  = Horizon::HardwareBuffer{};
            md.mesh_mem            = Corona::Memory::GpuMemToken{};
            CFW_LOG_NOTICE("[LOD] evicted LOD0 from VRAM: geom={} mesh={}",
                           eviction.geometry_handle, mesh_idx);
        }
    }

    // LOD 级 LRU — 帧末尾统一计算本帧 budget caps，下帧 reconcile 生效。
    // 调用必须在所有 mesh 循环结束后（entries 收集完整）、函数退出前。
    // 一帧延迟有意为之——无需拆分主循环；VRAM 压力变化缓慢（数百帧尺度）。
    enforce_lod_budget(camera_positions, lod_budget_entries);
}

// ============================================================================
// enforce_lod_budget（LOD 级 LRU — 延迟 Cap 机制）
// ============================================================================
// 在 reconcile_lod_residency 末尾调用。当 VRAM 超过 soft 水位（75% 预算）时，
// 按距相机距离排序（远者优先），对远处 mesh 施加 LOD 需求下限（cap），迫使其
// 选更粗 LOD 以释放显存。Cap 写入 lod_budget_caps，下帧 reconcile 生效。
//
// 设计要点：
//   1. soft_limit = mr.vram.budget_bytes * kLodBudgetSoftRatio(0.75)
//      （budget_bytes 取自 compute_memory_report，已做 min(物理, 用户设置) 封顶）
//   2. hard_limit = mr.vram.budget_bytes * kLodBudgetHardRatio(0.85)
//      超过时开启加速降级（每步 +2 级）
//   3. bytes_to_save 封顶为 mesh_bytes / 2（纹理无法通过 LOD 降级节省）
//   4. kMaxDegradedPerFrame = 64，防止单帧全场景同步 pop
// ============================================================================
void GeometrySystem::enforce_lod_budget(
    const std::vector<ktm::fvec3>& camera_positions,
    const std::vector<LodBudgetEntry>& entries) {

    // 清空上一帧的 caps，本帧重新计算
    impl_->lod_budget_caps.clear();

    // 获取当前 VRAM 状态，判定是否需要介入
    const MemoryReport mr = compute_memory_report();

    // 无预算 = 不限制，无需降级
    if (mr.vram.budget_bytes == 0) return;

    const std::size_t soft_limit =
        static_cast<std::size_t>(static_cast<double>(mr.vram.budget_bytes)
                                 * Impl::kLodBudgetSoftRatio);
    const std::size_t hard_limit =
        static_cast<std::size_t>(static_cast<double>(mr.vram.budget_bytes)
                                 * Impl::kLodBudgetHardRatio);

    // 当前用量未超软水位 → 无压力，直接返回
    if (mr.vram.used_bytes <= soft_limit) return;

    ++impl_->diag_lod_budget_checks;

    // 无相机或空 entries → 无法排序，直接返回
    if (camera_positions.empty() || entries.empty()) return;

    // ================================================================
    // 遍历 entries，构建候选项列表。
    // 每个候选项附带 per-level GPU bytes 预计算数组，供后续 savings
    // 计算使用。同时累加 total_estimated_bytes（诊断用）。
    // ================================================================
    struct Candidate {
        std::uint64_t lod_key;
        float         dist_sq;          // 到最近相机的距离平方（降序 = 远者优先）
        int           current_demand;   // 当前需求级（滞回后、clamp 后、cap 前）
        int           level_count;      // LOD 总级数
        // 预计算的各级 GPU 字节：下标 = LOD 级号，值 = 该级缓冲估算字节数。
        // LOD0 恒为 0（与 mesh_dev 共享缓冲，不占额外 VRAM）。
        std::vector<std::size_t> level_bytes;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(entries.size());
    std::size_t total_estimated_bytes = 0;

    {
        std::shared_lock lock(impl_->lod_cache_mutex);

        for (const auto& entry : entries) {
            // 无法降级的情况：只有 1 级或无更粗级可选
            if (entry.level_count <= 1) continue;
            // 已是该 mesh 的最粗级 → 无法再降
            if (entry.current_demand >= entry.level_count - 1) continue;
            // LOD0 降级会导致净 VRAM 增加（LOD1..N 需新建缓冲），跳过
            if (entry.current_demand < 1) continue;

            // ABA 校验：entry 在收集后可能被 evict
            auto cache_it = impl_->lod_cache.find(entry.lod_key);
            if (cache_it == impl_->lod_cache.end()) continue;
            const auto& levels = cache_it->second.levels;
            // level_count 变化（模型重导入等）→ 跳过
            if (levels.size() != static_cast<size_t>(entry.level_count)) continue;

            // 预计算各级 GPU 字节
            std::vector<std::size_t> lvl_bytes;
            lvl_bytes.reserve(levels.size());
            for (size_t lvl = 0; lvl < levels.size(); ++lvl) {
                if (lvl == 0) {
                    // LOD0 = mesh_dev 缓冲引用，不占额外 VRAM
                    lvl_bytes.push_back(0);
                } else if (levels[lvl].ready) {
                    // 已构建级：直接取账本字节
                    lvl_bytes.push_back(levels[lvl].mesh_mem.bytes());
                } else {
                    // 未构建级：根据顶点/索引数估算
                    // 公式：2 套缓冲（buffer + storage）×（顶点 + 索引）字节
                    const std::size_t est =
                        2u * levels[lvl].vertex_count * sizeof(Resource::Vertex)
                        + 2u * levels[lvl].index_count * sizeof(std::uint16_t);
                    lvl_bytes.push_back(est);
                }
            }

            // 当前级字节（当前显示的级 = committed_demand，即 entry.current_demand）
            const std::size_t current_bytes =
                (static_cast<size_t>(entry.current_demand) < lvl_bytes.size())
                    ? lvl_bytes[static_cast<size_t>(entry.current_demand)]
                    : 0;
            total_estimated_bytes += current_bytes;

            // 距最近相机距离（平方）
            const float dist_sq = nearest_camera_dist2(entry.world_center, camera_positions);

            candidates.push_back({entry.lod_key, dist_sq,
                                  entry.current_demand, entry.level_count,
                                  std::move(lvl_bytes)});
        }
    }  // shared_lock 释放

    if (candidates.empty()) return;

    impl_->diag_lod_budget_entries += candidates.size();
    impl_->diag_lod_budget_est_vram = total_estimated_bytes;

    // ================================================================
    // 按距离降序排序——远者先降级（近处保留精细度）
    // ================================================================
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.dist_sq > b.dist_sq;  // 远→近
              });

    // ================================================================
    // 计算需要节省的字节数，封顶到 mesh_bytes / 2。
    // 纹理无法通过 LOD 降级节省——若超预算全因纹理膨胀，无限降级 mesh
    // 只会白耗画质而于事无补。
    // ================================================================
    std::size_t bytes_to_save = mr.vram.used_bytes - soft_limit;
    // 封顶：至多节省 mesh 总量的一半
    const std::size_t max_mesh_savings = mr.vram.mesh_bytes / 2;
    if (bytes_to_save > max_mesh_savings)
        bytes_to_save = max_mesh_savings;

    // 加速模式：超过 hard_limit 时每步降 2 级以加速释放
    const bool accelerated = (mr.vram.used_bytes > hard_limit);

    // ================================================================
    // 遍历候选项，逐步降级直到满足目标或达到单帧上限
    // ================================================================
    std::size_t bytes_saved = 0;
    std::size_t degraded     = 0;

    for (const auto& cand : candidates) {
        if (bytes_saved >= bytes_to_save) break;
        if (degraded >= Impl::kMaxDegradedPerFrame) break;

        const int cur_demand = cand.current_demand;
        if (cur_demand < 0
            || static_cast<size_t>(cur_demand) >= cand.level_bytes.size())
            continue;

        // 加速模式：目标 = cur + 2；正常模式：cur + 1
        int target_demand = accelerated
            ? std::min(cur_demand + 2, cand.level_count - 1)
            : cur_demand + 1;

        // 确保 target 在有效范围内且确实比当前更粗
        if (target_demand <= cur_demand) continue;
        if (static_cast<size_t>(target_demand) >= cand.level_bytes.size()) continue;

        // 计算 savings：当前级字节 - 目标级字节
        const std::size_t cur_bytes = cand.level_bytes[static_cast<size_t>(cur_demand)];
        const std::size_t tgt_bytes = cand.level_bytes[static_cast<size_t>(target_demand)];
        // 如果目标级 > 当前级，目标级数据更小（更少的顶点），则 cur_bytes > tgt_bytes
        // savings = 释放的 - 新分配的 = cur - tgt
        const std::size_t savings =
            (cur_bytes > tgt_bytes) ? (cur_bytes - tgt_bytes) : 0;

        // 写入 cap：该 lod_key 的 demand 至少为 target_demand
        impl_->lod_budget_caps[cand.lod_key] = target_demand;
        bytes_saved += savings;
        ++degraded;
    }

    impl_->diag_lod_budget_degraded += degraded;

    // ================================================================
    // 诊断日志（仅在确实发生了降级且有诊断开关时输出）
    // ================================================================
    if (geometry_diagnostics_enabled() && degraded > 0) {
        CFW_LOG_NOTICE("[LODBudget] frame={} used={}KB budget={}KB "
                       "soft={}KB hard={}KB need={}KB "
                       "candidates={} degraded={} saved={}KB est_vram={}KB "
                       "mode={}",
                       impl_->lod_frame_counter,
                       mr.vram.used_bytes / 1024,
                       mr.vram.budget_bytes / 1024,
                       soft_limit / 1024,
                       hard_limit / 1024,
                       bytes_to_save / 1024,
                       candidates.size(),
                       degraded,
                       bytes_saved / 1024,
                       total_estimated_bytes / 1024,
                       accelerated ? "fast" : "normal");
    }
}


// ============================================================================
// process_pending_lod_builds（方案 C：轮询回写异步构建结果）
// ----------------------------------------------------------------------------
// 在 update() 中 reconcile 之前调用。遍历在途任务，对已就绪的 future 取结果并回写
// 进 lod_cache。回写前做 ABA 重校验（条目在 + model_id + residency_epoch + 级存在 +
// 仍未就绪），任一不符即丢弃——结果 RAII 自动释放 worker 已建的 GPU 缓冲，零泄漏。
// GpuMemToken 在此（几何线程）构建，回避 worker 侧 ledger 线程安全问题。
// 仅几何线程访问 pending_lod_builds，无需加锁。
// ============================================================================
void GeometrySystem::process_pending_lod_builds() {
    if (impl_->pending_lod_builds.empty()) return;

    std::vector<uint64_t> done;
    for (auto& [key, task] : impl_->pending_lod_builds) {
        if (task.future.valid() &&
            task.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            done.push_back(key);
        }
    }

    for (uint64_t key : done) {
        Impl::PendingLodBuild task = std::move(impl_->pending_lod_builds[key]);
        impl_->pending_lod_builds.erase(key);

        Impl::LODBuildResult r = task.future.get();
        if (!r.ok) { ++impl_->diag_lod_build_discards; continue; }

        // 令牌在几何线程构建（worker 只产出缓冲 + 字节数）
        Corona::Memory::GpuMemToken tok(Corona::Memory::ResKind::Mesh, r.gpu_bytes);

        std::unique_lock lock(impl_->lod_cache_mutex);
        auto cit = impl_->lod_cache.find(key);
        if (cit == impl_->lod_cache.end()
            || cit->second.model_id != task.model_id
            || cit->second.residency_epoch != task.residency_epoch
            || static_cast<size_t>(task.level) >= cit->second.levels.size()
            || cit->second.levels[task.level].ready) {
            ++impl_->diag_lod_build_discards;  // 条目已变/已就绪 → 丢弃（r 析构释放 GPU）
            continue;
        }
        auto& lvl = cit->second.levels[task.level];
        lvl.vertex_buffer  = std::move(r.vertex_buffer);
        lvl.index_buffer   = std::move(r.index_buffer);
        lvl.vertex_storage = std::move(r.vertex_storage);
        lvl.index_storage  = std::move(r.index_storage);
        lvl.mesh_mem       = std::move(tok);
        lvl.ready          = true;
        lvl.last_demand_frame = impl_->lod_frame_counter;
        ++impl_->diag_lod_builds;

        // ---- 变更 H：swap 完成检测 ----
        // 新级 ready 后立即释放被保活的旧级（prev_committed），最小化"2层共存"窗口。
        // 不等下一帧 reconcile：本帧 build 回写后即刻在同一 unique_lock 内完成 swap。
        const bool shadow_task = task.purpose == Impl::LodBuildPurpose::Shadow;
        if (shadow_task ? cit->second.shadow_swap_in_progress : cit->second.swap_in_progress) {
            const int new_d   = shadow_task ? cit->second.shadow_committed_demand : cit->second.committed_demand;
            const int old_lvl = shadow_task ? cit->second.shadow_prev_committed : cit->second.prev_committed;
            // 确认：刚回写的级正是 committed_demand（避免中途跳级时把错误级当新级）
            if (task.level == new_d && lvl.ready) {
                if (shadow_task) {
                    cit->second.shadow_swap_in_progress = false;
                    cit->second.shadow_prev_committed = -1;
                } else {
                    cit->second.swap_in_progress = false;
                    cit->second.prev_committed = -1;
                }
                CFW_LOG_NOTICE("[LOD] swap complete: freed old level {} -> active level {}",
                               old_lvl, new_d);
                // 释放旧保活级（old_lvl >= 1：永不从 lod_cache 移除 LOD0 的引用计数副本）
                if (old_lvl >= 1
                    && old_lvl != new_d
                    && static_cast<size_t>(old_lvl) < cit->second.levels.size()) {
                    auto& old = cit->second.levels[old_lvl];
                    old.ready          = false;
                    old.vertex_buffer  = Horizon::HardwareBuffer{};
                    old.index_buffer   = Horizon::HardwareBuffer{};
                    old.vertex_storage = Horizon::HardwareBuffer{};
                    old.index_storage  = Horizon::HardwareBuffer{};
                    old.mesh_mem       = Corona::Memory::GpuMemToken{};
                    ++impl_->diag_lod_frees;
                }
            }
        }
    }

    if (!impl_->pending_shadow_lod_builds.empty()) {
        auto main_pending = std::move(impl_->pending_lod_builds);
        impl_->pending_lod_builds = std::move(impl_->pending_shadow_lod_builds);
        process_pending_lod_builds();
        impl_->pending_shadow_lod_builds = std::move(impl_->pending_lod_builds);
        impl_->pending_lod_builds = std::move(main_pending);
    }
}


}  // namespace Corona::Systems

namespace Corona::Systems {

// ============================================================================
// P0：mesh/texture 资源内存账本（CPU 侧登记 + 对账）与预算报告
// ============================================================================

// ============================================================================
// reconcile_cpu_residency（RAM 3层窗口管理 + LOD 磁盘缓存换入换出）
// ----------------------------------------------------------------------------
// 每 kCpuWindowEvalInterval 帧调用一次（update() → reconcile_lod_residency 之后）。
// 将每个 model 的 CPU LODLevel 数据限制在固定3层窗口内：
//   { lod_levels[0],  lod_levels[demand_median-1],  lod_levels[N-1] }
//     最精细简化级           中位数需求级                最粗级
// LOD0（MeshData::vertices/indices）永远保留，不在此管理。
//
// 窗口外的级不再直接丢弃：数据 std::move 进异步写盘队列，由后台 worker 线程
// 序列化后写入两级 LRU 磁盘缓存（几何线程只付出移动指针 + 入队的开销）。
// 窗口内数据缺失的级则反向操作：查磁盘缓存 → 反序列化回填 lod_levels，
// 命中后 reconcile_lod_residency 才能为该级重建 GPU 缓冲；未命中则该级
// 保持为空，GPU 侧继续使用已驻留的级（通常 LOD0）渲染。
// ============================================================================
void GeometrySystem::reconcile_cpu_residency() {
    auto& resource_manager = Resource::ResourceManager::get_instance();

    // 1. 按 model_id 收集所有实例的 committed_demand + LOD元数据
    struct ModelInfo {
        std::vector<int> demands;   // 各实例 committed_demand
        int coarsest_src = -1;      // lod_cache 中最粗级的 source_lod_index
        int level_count  = 0;       // lod_cache 中的级数（含LOD0）
    };
    std::unordered_map<uint64_t, ModelInfo> model_info;
    {
        std::shared_lock lk(impl_->lod_cache_mutex);
        for (const auto& [key, entry] : impl_->lod_cache) {
            if (entry.model_id == 0 || entry.levels.size() <= 1) continue;
            auto& mi = model_info[entry.model_id];
            mi.demands.push_back(entry.committed_demand);
            if (mi.level_count == 0) {
                mi.level_count  = static_cast<int>(entry.levels.size());
                mi.coarsest_src = entry.levels.back().source_lod_index;
            }
        }
    }
    if (model_info.empty()) return;

    // 延迟初始化磁盘缓存 + 启动 worker 线程（call_once，首次以后为空操作）。
    // 放在函数入口而非 evict 现场：目录创建 + 线程启动在任何 Scene 写锁之外
    // 完成，避免首次 evict 时在 acquire_write<Scene> 持有期间做文件系统操作。
    impl_->ensure_lod_disk_cache();

    for (auto& [model_id, mi] : model_info) {
        if (mi.demands.empty() || mi.coarsest_src < 0) continue;

        // 2. 计算中位数 committed_demand（整数排序取中间值）
        std::sort(mi.demands.begin(), mi.demands.end());
        const int D_med = mi.demands[mi.demands.size() / 2];

        // 3. 确定 CPU 驻留窗口（lod_levels[] 下标）
        // demand_lod_src = D_med - 1：cache 级 D_med 对应 lod_levels[D_med-1]
        // D_med=0（近距离，直接用 mesh_dev）→ demand_lod_src=0（保留 lod_levels[0]）
        const int demand_lod_src = (D_med >= 1) ? (D_med - 1) : 0;

        std::unordered_set<int> cpu_window;
        cpu_window.insert(0);                // 最精细简化级，始终保留
        cpu_window.insert(demand_lod_src);   // 中位数需求级
        cpu_window.insert(mi.coarsest_src);  // 最粗级，始终保留

        // 更新诊断信息
        {
            std::lock_guard<std::mutex> lk(impl_->cpu_window_mutex);
            impl_->model_cpu_windows[model_id] = {0, demand_lod_src, mi.coarsest_src};
        }

        // 4. acquire_write<Scene>，对每个 mesh 的每个 lod_level 做双向处理：
        //    窗口内 + 数据缺失 → 查磁盘缓存恢复（反序列化回填，供 GPU 侧重建该级）
        //    窗口外 + 数据仍在 → std::move 移交异步写盘队列（几何线程不做序列化/IO）
        // 写锁持有期间的开销：evict 侧仅移动指针 + 入队（微秒级）；restore 侧
        // 命中内存级缓存为一次 memcpy，仅在数据已被内存级 LRU 刷盘时才同步读盘
        // （小概率路径，128MB 内存级容量足以覆盖正常的换出→换入时间窗口）。
        auto scene_write = resource_manager.acquire_write<Resource::Scene>(model_id);
        if (!scene_write.valid()) continue;

        int freed_count    = 0;  // 本周期移交写盘队列的级数1
        int restored_count = 0;  // 本周期从磁盘恢复的级数
        for (std::uint32_t mesh_idx = 0;
             mesh_idx < static_cast<std::uint32_t>(scene_write->data.meshes.size());
             ++mesh_idx) {
            auto& mesh = scene_write->data.meshes[mesh_idx];
            for (int j = 0; j < static_cast<int>(mesh.lod_levels.size()); ++j) {
                auto& lod = mesh.lod_levels[j];

                if (cpu_window.count(j) > 0) {
                    // ---- 窗口内：保留；数据缺失时尝试恢复 ----
                    // 数据为空说明此级此前被移出窗口写过盘（或写盘失败被丢弃）。
                    // 恢复顺序：
                    //   ① 异步写盘队列——该级刚被 evict、blob 尚未落盘时数据仍在
                    //     队列中，直接 move 回填。既消除落盘前的 miss，也杜绝命中
                    //     同 key 陈旧磁盘 blob（此前 put 失败/超大跳过时遗留）的竞态。
                    //   ② worker 正在写该 key → 本周期跳过，下周期从磁盘命中。
                    //   ③ 磁盘缓存 get → 反序列化回填。
                    // 全部未命中则保持为空——该级无法重建（无运行时 meshopt 路径），
                    // 渲染继续使用已驻留的级（通常 LOD0），直到模型重新导入。
                    if (lod.vertices.empty() && impl_->lod_disk_cache) {
                        const auto key = make_lod_disk_key(model_id, mesh_idx, j);

                        // ①② 查队列/在写标记（仅锁 3，未嵌套锁 2，符合锁层次）
                        bool handled = false;
                        {
                            std::lock_guard qlock(impl_->lod_disk_write_mutex);
                            auto qit = std::find_if(
                                impl_->pending_lod_disk_writes.begin(),
                                impl_->pending_lod_disk_writes.end(),
                                [&key](const LodDiskWriteTask& t) { return t.key == key; });
                            if (qit != impl_->pending_lod_disk_writes.end()) {
                                lod.vertices         = std::move(qit->record.vertices);
                                lod.indices          = std::move(qit->record.indices);
                                lod.bone_weights     = std::move(qit->record.bone_weights);
                                lod.error            = qit->record.error;
                                lod.screen_threshold = qit->record.screen_threshold;
                                lod.geometric_error  = qit->record.geometric_error;
                                impl_->pending_lod_disk_writes.erase(qit);
                                ++restored_count;
                                handled = true;
                            } else if (impl_->lod_disk_write_inflight_key == key) {
                                handled = true;  // 正在落盘：本周期跳过
                            }
                        }
                        if (handled) {
                            // 队列回填成功后顺带清理可能残留的同 key 旧磁盘 blob；
                            // 在写跳过时 vertices 仍空，不清理（以免与 put 竞争）
                            if (!lod.vertices.empty())
                                impl_->lod_disk_cache->erase(key);
                            continue;
                        }

                        // ③ 磁盘缓存回读
                        auto cached = impl_->lod_disk_cache->get(key);
                        if (cached) {
                            auto record = deserialize_lod_record(cached->data);
                            if (record) {
                                lod.vertices         = std::move(record->vertices);
                                lod.indices          = std::move(record->indices);
                                lod.bone_weights     = std::move(record->bone_weights);
                                lod.error            = record->error;
                                lod.screen_threshold = record->screen_threshold;
                                lod.geometric_error  = record->geometric_error;
                                // 数据已回到 RAM，删除磁盘副本避免双份占用；
                                // 下次移出窗口时会重新入队写盘
                                impl_->lod_disk_cache->erase(key);
                                ++restored_count;
                            }
                            // 反序列化失败（版本不符/数据损坏）→ 保持为空，
                            // 该级同样不再可恢复；损坏条目留在缓存中会被 LRU
                            // 自然淘汰，无需主动清理
                        }
                    }
                    continue;
                }

                // ---- 窗口外：数据移交异步写盘队列 ----
                if (lod.vertices.empty()) continue;  // 已清空：跳过

                // 背压检查必须在 std::move 之前完成——一旦 move，本级数据即空，
                // 队列满时无法回退。队列满则跳过本级，数据原样保留在 lod_levels
                // 中，下个评估周期（kCpuWindowEvalInterval 帧后）重试。
                bool queued = false;
                {
                    std::lock_guard qlock(impl_->lod_disk_write_mutex);
                    if (impl_->pending_lod_disk_writes.size() < Impl::kMaxPendingLodDiskWrites) {
                        LodDiskWriteTask task;
                        task.key                     = make_lod_disk_key(model_id, mesh_idx, j);
                        task.record.vertices         = std::move(lod.vertices);
                        task.record.indices          = std::move(lod.indices);
                        task.record.bone_weights     = std::move(lod.bone_weights);
                        task.record.error            = lod.error;
                        task.record.screen_threshold = lod.screen_threshold;
                        task.record.geometric_error  = lod.geometric_error;
                        task.model_id   = model_id;
                        task.mesh_index = mesh_idx;
                        task.lod_level  = j;
                        impl_->pending_lod_disk_writes.push_back(std::move(task));
                        queued = true;
                    }
                }

                if (queued) {
                    // notify 放在队列锁外，避免 worker 被唤醒后立刻阻塞在锁上
                    impl_->lod_disk_write_cv.notify_one();
                    // moved-from vector 标准上仅保证 valid-but-unspecified；后续
                    // evict/restore 逻辑依赖 empty() 判断，显式 clear 守规
                    //（所有主流实现移动后即为空，clear 为零开销）
                    lod.vertices.clear();
                    lod.indices.clear();
                    lod.bone_weights.clear();
                    ++freed_count;
                }
            }
        }
        if (freed_count > 0 || restored_count > 0) {
            CFW_LOG_NOTICE("[LOD-CPU] model={:#x} window={{0, {}, {}}}: "
                           "移交写盘队列 {} 级, 恢复 {} 级（队列/磁盘）",
                           model_id, demand_lod_src, mi.coarsest_src,
                           freed_count, restored_count);
        }
    }
}

// ============================================================================
// query_mesh_slots — 跨系统统一 mesh GPU 状态快照接口
// ----------------------------------------------------------------------------
// 内部持写锁（texture.storeSampledDescriptor 需非 const），单次锁内完成所有
// mesh 的 LOD 路由 + 材质拷贝，返回 refcount 值副本组成的 vector。
// 调用方不得持有同一 geometry_handle 的锁（避免重入死锁）。
// ============================================================================

namespace {

[[nodiscard]] bool render_mesh_buffers_valid(const GeometrySystem::RenderMeshBuffers& geo) {
    return static_cast<bool>(geo.vertex) &&
           static_cast<bool>(geo.index) &&
           static_cast<bool>(geo.vertex_storage) &&
           static_cast<bool>(geo.index_storage);
}

void log_invalid_mesh_slot_once(std::uintptr_t geometry_handle,
                                std::uint32_t mesh_index,
                                const GeometrySystem::RenderMeshBuffers& geo,
                                const Horizon::HardwareImage& texture) {
    const std::string key = std::to_string(geometry_handle) + ":" + std::to_string(mesh_index);
    {
        std::lock_guard lock(g_invalid_mesh_slot_log_mutex);
        if (!g_invalid_mesh_slot_logs.insert(key).second) return;
    }

    CFW_LOG_WARNING("GeometrySystem: invalid mesh slot skipped "
                    "(geometry={}, mesh={}, vertex={}, index={}, vertex_storage={}, index_storage={}, texture={}, vertices={}, indices={}, max_index={})",
                    geometry_handle, mesh_index,
                    static_cast<bool>(geo.vertex),
                    static_cast<bool>(geo.index),
                    static_cast<bool>(geo.vertex_storage),
                    static_cast<bool>(geo.index_storage),
                    static_cast<bool>(texture),
                    geo.vertex_count,
                    geo.index_count,
                    geo.max_index);
}

/// 内部辅助：共享 LOD 路由 + MeshSlot 构建逻辑（有/无相机两条路径共用）
[[nodiscard]] std::vector<GeometrySystem::MeshSlot>
build_mesh_slots(const GeometrySystem*        gs,
                 std::uintptr_t               geometry_handle,
                 const GeometryDevice&        geom,
                 bool                         use_camera,
                 const ktm::fvec3&            camera_pos,
                 float                        camera_fov_deg,
                 const ktm::fvec3&            world_center,
                 float                        bounding_radius,
                 bool                         use_shadow_lod = false,
                 float                        shadow_texel = 0.0f,
                 float                        shadow_scale = 1.0f) {
    std::vector<GeometrySystem::MeshSlot> result;
    result.reserve(geom.mesh_handles.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(geom.mesh_handles.size()); ++i) {
        const auto& m = geom.mesh_handles[i];

        // LOD 路由：构建 fallback，然后经 GeometrySystem 解析当前常驻级
        GeometrySystem::RenderMeshBuffers fallback{
            m.vertexBuffer, m.indexBuffer,
            m.vertexStorageBuffer, m.indexStorageBuffer,
            m.vertex_count, m.index_count, m.max_index};

        GeometrySystem::RenderMeshBuffers geo = use_shadow_lod
            ? gs->select_shadow_render_buffers(geometry_handle, i, shadow_texel,
                                                shadow_scale, fallback)
            : use_camera
            ? gs->select_render_buffers(
                  geometry_handle, i,
                  camera_pos, camera_fov_deg, world_center, bounding_radius,
                  fallback)
            : gs->resident_render_buffers(geometry_handle, i, fallback);

        GeometrySystem::MeshSlot slot;
        slot.mesh_index     = i;
        slot.geo            = geo;
        slot.texture        = m.textureBuffer;        // 引用计数值拷贝
        slot.material_color = m.materialColor;
        slot.texture_ready  = static_cast<bool>(slot.texture);
        slot.vertex_count   = slot.geo.vertex_count;
        slot.index_count    = slot.geo.index_count;
        slot.max_index      = slot.geo.max_index;
        slot.valid          = render_mesh_buffers_valid(slot.geo) &&
                              static_cast<bool>(slot.texture);
        if (!slot.valid) {
            log_invalid_mesh_slot_once(geometry_handle, i, slot.geo, slot.texture);
        }
        result.push_back(std::move(slot));
    }
    return result;
}

}  // namespace

std::vector<GeometrySystem::MeshSlot>
GeometrySystem::query_mesh_slots(std::uintptr_t geometry_handle) const {
    auto& geom_storage = SharedDataHub::instance().geometry_storage();
    // 读锁即可：query_mesh_slots 只复制句柄值，不修改 GeometryDevice 状态。
    // storeSampledDescriptor 由调用方在拿到 MeshSlot 后自行调用（对本地副本操作，
    // 无需持有 geometry 锁）。
    auto geom = geom_storage.try_acquire_read(geometry_handle);
    if (!geom || geom->mesh_handles.empty()) return {};

    return build_mesh_slots(this, geometry_handle, *geom,
                            false, {}, 0.f, {}, 0.f);
}

std::vector<GeometrySystem::MeshSlot>
GeometrySystem::query_mesh_slots(std::uintptr_t    geometry_handle,
                                 const ktm::fvec3& camera_pos,
                                 float             camera_fov_deg,
                                 const ktm::fvec3& world_center,
                                 float             bounding_radius) const {
    auto& geom_storage = SharedDataHub::instance().geometry_storage();
    auto geom = geom_storage.try_acquire_read(geometry_handle);
    if (!geom || geom->mesh_handles.empty()) return {};

    return build_mesh_slots(this, geometry_handle, *geom,
                            true, camera_pos, camera_fov_deg,
                            world_center, bounding_radius);
}

void GeometrySystem::request_shadow_lod(std::uintptr_t geometry_handle,
                                        uint32_t mesh_index,
                                        float world_units_per_texel,
                                        float max_abs_scale,
                                        std::uint64_t frame) const {
    if (!(world_units_per_texel > 0.0f) || !std::isfinite(world_units_per_texel) ||
        !std::isfinite(max_abs_scale)) return;
    std::unique_lock lock(impl_->lod_cache_mutex);
    auto it = impl_->lod_cache.find(Impl::make_lod_key(geometry_handle, mesh_index));
    if (it == impl_->lod_cache.end() || it->second.levels.size() <= 1) return;
    std::array<float, 8> errors{};
    const int count = static_cast<int>(std::min<size_t>(it->second.levels.size(), errors.size()));
    for (int i = 0; i < count; ++i) errors[static_cast<size_t>(i)] = it->second.levels[i].geometric_error;
    const int target = GeometryDetail::choose_shadow_target(
        errors, count, max_abs_scale, world_units_per_texel, 0);
    if (it->second.shadow_last_request_frame != frame) {
        it->second.shadow_committed_demand = target;
    } else if (it->second.shadow_committed_demand < 0) {
        it->second.shadow_committed_demand = target;
    } else {
        it->second.shadow_committed_demand = std::min(it->second.shadow_committed_demand, target);
    }
    it->second.shadow_last_request_frame = frame;
}

std::vector<GeometrySystem::MeshSlot>
GeometrySystem::query_shadow_mesh_slots(std::uintptr_t geometry_handle,
                                        float world_units_per_texel,
                                        float max_abs_scale,
                                        std::uint64_t frame) const {
    auto& geom_storage = SharedDataHub::instance().geometry_storage();
    auto geom = geom_storage.try_acquire_read(geometry_handle);
    if (!geom || geom->mesh_handles.empty()) return {};

    for (uint32_t mesh_index = 0;
         mesh_index < static_cast<uint32_t>(geom->mesh_handles.size());
         ++mesh_index) {
        request_shadow_lod(geometry_handle, mesh_index, world_units_per_texel,
                           max_abs_scale, frame);
    }
    return build_mesh_slots(this, geometry_handle, *geom,
                            false, {}, 0.f, {}, 0.f,
                            true, world_units_per_texel, max_abs_scale);
}

std::vector<std::vector<GeometrySystem::ShadowMeshSlot>>
GeometrySystem::query_shadow_mesh_slots_batch(
    std::uintptr_t geometry_handle,
    std::span<const ShadowLodQuery> cascades,
    float max_abs_scale,
    std::uint64_t frame) const {
    std::vector<std::vector<ShadowMeshSlot>> result(cascades.size());
    if (cascades.empty()) return result;

    auto& geom_storage = SharedDataHub::instance().geometry_storage();
    auto geom = geom_storage.try_acquire_read(geometry_handle);
    if (!geom || geom->mesh_handles.empty()) return result;

    std::vector<GeometryDetail::ShadowLodQueryInput> query_inputs;
    query_inputs.reserve(cascades.size());
    for (const auto& cascade : cascades) {
        query_inputs.push_back({cascade.enabled, cascade.world_units_per_texel});
    }
    std::vector<int> targets(cascades.size(), -1);
    for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade) {
        if (cascades[cascade].enabled) {
            result[cascade].reserve(geom->mesh_handles.size());
        }
    }

    std::unique_lock lod_lock(impl_->lod_cache_mutex);
    for (uint32_t mesh_index = 0;
         mesh_index < static_cast<uint32_t>(geom->mesh_handles.size());
         ++mesh_index) {
        const auto& mesh = geom->mesh_handles[mesh_index];
        const RenderMeshBuffers fallback{
            mesh.vertexBuffer, mesh.indexBuffer,
            mesh.vertexStorageBuffer, mesh.indexStorageBuffer,
            mesh.vertex_count, mesh.index_count, mesh.max_index};

        auto cache_it = impl_->lod_cache.find(Impl::make_lod_key(geometry_handle, mesh_index));
        const bool has_levels = cache_it != impl_->lod_cache.end() &&
                                !cache_it->second.levels.empty();
        int main_target = 0;
        GeometryDetail::ShadowLodBatchDecision decision;
        if (has_levels) {
            auto& entry = cache_it->second;
            main_target = std::clamp(entry.committed_demand, 0,
                                     static_cast<int>(entry.levels.size()) - 1);
            std::array<float, 8> errors{};
            const int level_count = static_cast<int>(
                std::min<std::size_t>(entry.levels.size(), errors.size()));
            for (int level = 0; level < level_count; ++level) {
                errors[static_cast<std::size_t>(level)] =
                    entry.levels[static_cast<std::size_t>(level)].geometric_error;
            }
            decision = GeometryDetail::choose_shadow_targets(
                errors, level_count, max_abs_scale, query_inputs, targets, main_target);
            if (entry.levels.size() > 1 && decision.aggregated_demand >= 0) {
                if (entry.shadow_last_request_frame != frame ||
                    entry.shadow_committed_demand < 0) {
                    entry.shadow_committed_demand = decision.aggregated_demand;
                } else {
                    entry.shadow_committed_demand = std::min(
                        entry.shadow_committed_demand, decision.aggregated_demand);
                }
                entry.shadow_last_request_frame = frame;
            }
        } else {
            for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade) {
                targets[cascade] = cascades[cascade].enabled ? main_target : -1;
            }
        }

        for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade) {
            if (!cascades[cascade].enabled) continue;
            RenderMeshBuffers selected = fallback;
            if (has_levels) {
                const auto& levels = cache_it->second.levels;
                const int target = std::clamp(targets[cascade], 0,
                                              static_cast<int>(levels.size()) - 1);
                for (int level = target; level >= 0; --level) {
                    const auto& candidate = levels[static_cast<std::size_t>(level)];
                    if (!candidate.ready || !candidate.vertex_buffer ||
                        !candidate.index_buffer) {
                        continue;
                    }
                    selected = RenderMeshBuffers{
                        candidate.vertex_buffer,
                        candidate.index_buffer,
                        candidate.vertex_storage ? candidate.vertex_storage
                                                 : fallback.vertex_storage,
                        candidate.index_storage ? candidate.index_storage
                                                : fallback.index_storage,
                        candidate.vertex_count,
                        candidate.index_count,
                        candidate.max_index};
                    break;
                }
            }

            ShadowMeshSlot slot;
            slot.mesh_index = mesh_index;
            slot.geo = std::move(selected);
            slot.vertex_count = slot.geo.vertex_count;
            slot.index_count = slot.geo.index_count;
            slot.max_index = slot.geo.max_index;
            slot.valid = render_mesh_buffers_valid(slot.geo);
            result[cascade].push_back(std::move(slot));
        }
    }
    return result;
}

void GeometrySystem::update_cpu_resource_ledger() {
    auto& hub = SharedDataHub::instance();
    auto& rm  = Resource::ResourceManager::get_instance();
    auto& geom_storage = hub.geometry_storage();

    // ---- 阶段 1：收集当前驻留的 model_id（Scene rid）----
    std::unordered_set<std::uint64_t> live_model_ids;
    for (auto it = geom_storage.cbegin(); it != geom_storage.cend(); ++it) {
        if (!it->model_resource_handle) continue;
        std::uint64_t model_id = 0;
        if (auto mr = hub.model_resource_storage().try_acquire_read(it->model_resource_handle))
            model_id = mr->model_id;
        if (model_id != 0) live_model_ids.insert(model_id);
    }

    // ---- 阶段 2：登记新出现 model_id 的 mesh CPU + 其纹理 texture CPU ----
    // 字节计算在 ledger 锁外完成（持 Scene/Image 读锁），算完再短暂加锁写入。
    for (std::uint64_t model_id : live_model_ids) {
        {
            std::lock_guard lk(impl_->cpu_ledger_mutex);
            if (impl_->cpu_ledger.count(model_id)) continue;  // 已登记
        }
        auto scene = rm.acquire_read<Resource::Scene>(model_id);
        if (!scene.valid()) continue;

        // mesh CPU：所有 mesh 的顶点/索引 + 各级 LOD
        std::size_t mesh_bytes = 0;
        for (const auto& mesh : scene->data.meshes) {
            mesh_bytes += mesh.vertices.size() * sizeof(Resource::Vertex);
            mesh_bytes += mesh.indices.size()  * sizeof(std::uint16_t);
            for (const auto& lod : mesh.lod_levels) {
                mesh_bytes += lod.vertices.size() * sizeof(Resource::Vertex);
                mesh_bytes += lod.indices.size()  * sizeof(std::uint16_t);
            }
        }

        // texture CPU：收集材质引用的全部纹理 rid（去重），按解码/压缩态算字节
        std::vector<std::pair<std::uint64_t, std::size_t>> tex_entries;
        auto collect_tex = [&](std::uint64_t tid) {
            if (tid == Resource::InvalidTextureId || tid == 0) return;
            for (const auto& te : tex_entries) if (te.first == tid) return;  // 本 Scene 内去重
            std::size_t tbytes = 0;
            if (auto img = rm.acquire_read<Resource::Image>(tid)) {
                if (img->is_compressed())
                    tbytes = img->get_compressed_data().data.size();
                else
                    tbytes = static_cast<std::size_t>(img->get_width()) *
                             static_cast<std::size_t>(img->get_height()) *
                             static_cast<std::size_t>(img->get_channels());
            }
            if (tbytes > 0) tex_entries.emplace_back(tid, tbytes);
        };
        for (const auto& mat : scene->data.materials) {
            collect_tex(mat.albedo_texture);
            collect_tex(mat.normal_texture);
            collect_tex(mat.metallic_texture);
            collect_tex(mat.roughness_texture);
            collect_tex(mat.opacity_texture);
        }

        std::lock_guard lk(impl_->cpu_ledger_mutex);
        impl_->cpu_ledger.try_emplace(
            model_id, Impl::CpuResEntry{Corona::Memory::ResKind::Mesh, mesh_bytes});
        for (const auto& [tid, tbytes] : tex_entries)
            impl_->cpu_ledger.try_emplace(
                tid, Impl::CpuResEntry{Corona::Memory::ResKind::Texture, tbytes});
    }

    // ---- 阶段 3：对账 —— 删除已被 ResourceManager 驱逐的 rid（liveness 减计）----
    {
        auto entries = rm.list_entries();
        std::unordered_set<std::uint64_t> live_rids;
        live_rids.reserve(entries.size());
        for (const auto& e : entries) live_rids.insert(e.rid);

        std::lock_guard lk(impl_->cpu_ledger_mutex);
        for (auto it = impl_->cpu_ledger.begin(); it != impl_->cpu_ledger.end();) {
            if (!live_rids.count(it->first))
                it = impl_->cpu_ledger.erase(it);
            else
                ++it;
        }
    }
}

MemoryReport GeometrySystem::compute_memory_report() const {
    MemoryReport r;

    // ---- VRAM ----
    // mesh/texture 与 used 全部来自我们自己的 RAII 令牌账本（自行统计用量，不依赖 Horizon）。
    auto& led = Corona::Memory::gpu_ledger();
    r.vram.mesh_bytes    = led.mesh_bytes();
    r.vram.texture_bytes = led.texture_bytes();
    r.vram.used_bytes    = r.vram.mesh_bytes + r.vram.texture_bytes;
    r.vram_mesh_peak     = led.mesh_peak();
    r.vram_texture_peak  = led.texture_peak();

    // 容量（仅大小）来自 Horizon：DEVICE_LOCAL 显存总量；可被手动 vram_budget 下调。
    // NOTE: query_device_memory_size() 已在新版 Horizon 中移除
    // 使用默认值 8GB，可通过 vram_budget_bytes 手动配置
    std::size_t vram_cap = 8ULL * 1024 * 1024 * 1024; // 8 GB default
    if (impl_->vram_budget_bytes > 0)
        vram_cap = std::min(vram_cap, impl_->vram_budget_bytes);

    // ---- RAM ----
    // used = 我们追踪的 mesh+texture CPU（按 rid 去重）；容量 = SDL 系统物理内存总量。
    {
        std::lock_guard lk(impl_->cpu_ledger_mutex);
        for (const auto& [rid, e] : impl_->cpu_ledger) {
            if (e.kind == Corona::Memory::ResKind::Mesh)
                r.ram.mesh_bytes += e.bytes;
            else
                r.ram.texture_bytes += e.bytes;
        }
    }
    r.ram.used_bytes = r.ram.mesh_bytes + r.ram.texture_bytes;
    const std::size_t ram_cap =
        static_cast<std::size_t>(Corona::Memory::system_ram_bytes().load(std::memory_order_relaxed));

    // ---- 水位 + over/need_free（high=90%、low=80% 容量）----
    const double hi = static_cast<double>(impl_->evict_high_ratio);
    const double lo = static_cast<double>(impl_->evict_low_ratio);
    auto fill = [hi, lo](MemoryPoolReport& p, std::size_t cap) {
        p.budget_bytes = cap;
        if (cap > 0) {
            p.high_bytes = static_cast<std::size_t>(static_cast<double>(cap) * hi);
            p.low_bytes  = static_cast<std::size_t>(static_cast<double>(cap) * lo);
            if (p.used_bytes > p.high_bytes) {
                p.over_bytes      = p.used_bytes - p.high_bytes;
                p.need_free_bytes = p.used_bytes - p.low_bytes;
                p.pressured       = true;
            }
        }
    };
    fill(r.vram, vram_cap);
    fill(r.ram, ram_cap);
    return r;
}

MemoryReport GeometrySystem::memory_report() const {
    return compute_memory_report();
}

void GeometrySystem::estimate_actor_memory(std::uintptr_t actor,
                                           std::size_t& out_gpu_bytes,
                                           std::size_t& out_cpu_bytes) const {
    out_gpu_bytes = 0;
    out_cpu_bytes = 0;
    auto& hub = SharedDataHub::instance();

    auto actor_read = hub.actor_storage().try_acquire_read(actor);
    if (!actor_read.valid()) return;

    std::unordered_set<std::uintptr_t> visited_geoms;
    std::unordered_set<std::uint64_t>  visited_models;

    for (auto profile_handle : actor_read->profile_handles) {
        auto profile = hub.profile_storage().try_acquire_read(profile_handle);
        if (!profile) continue;

        std::vector<std::uintptr_t> geom_handles;
        if (profile->geometry_handle) geom_handles.push_back(profile->geometry_handle);
        if (profile->optics_handle) {
            if (auto o = hub.optics_storage().try_acquire_read(profile->optics_handle))
                if (o->geometry_handle) geom_handles.push_back(o->geometry_handle);
        }
        if (profile->mechanics_handle) {
            if (auto m = hub.mechanics_storage().try_acquire_read(profile->mechanics_handle))
                if (m->geometry_handle) geom_handles.push_back(m->geometry_handle);
        }
        if (profile->acoustics_handle) {
            if (auto a = hub.acoustics_storage().try_acquire_read(profile->acoustics_handle))
                if (a->geometry_handle) geom_handles.push_back(a->geometry_handle);
        }

        for (auto geom_handle : geom_handles) {
            if (!visited_geoms.insert(geom_handle).second) continue;

            uint32_t      mesh_count = 0;
            std::uint64_t model_id   = 0;
            {
                auto geom = hub.geometry_storage().try_acquire_read(geom_handle);
                if (!geom) continue;
                mesh_count = static_cast<uint32_t>(geom->mesh_handles.size());
                for (const auto& md : geom->mesh_handles)
                    out_gpu_bytes += md.mesh_mem.bytes() + md.tex_mem.bytes();
                if (geom->model_resource_handle) {
                    if (auto mr = hub.model_resource_storage().try_acquire_read(geom->model_resource_handle))
                        model_id = mr->model_id;
                }
            }
            // LOD 缓存的 GPU 字节（LOD0 计 0，LOD1..N 各级令牌）
            {
                std::shared_lock lod_lock(impl_->lod_cache_mutex);
                for (uint32_t mi = 0; mi < mesh_count; ++mi) {
                    auto it = impl_->lod_cache.find(Impl::make_lod_key(geom_handle, mi));
                    if (it == impl_->lod_cache.end()) continue;
                    for (const auto& lvl : it->second.levels)
                        out_gpu_bytes += lvl.mesh_mem.bytes();
                }
            }
            // CPU 字节：该 model_id 的 mesh CPU（cpu_ledger，按 model_id 去重）
            if (model_id != 0 && visited_models.insert(model_id).second) {
                std::lock_guard lk(impl_->cpu_ledger_mutex);
                auto it = impl_->cpu_ledger.find(model_id);
                if (it != impl_->cpu_ledger.end()) out_cpu_bytes += it->second.bytes;
            }
        }
    }
}

void GeometrySystem::evict_under_memory_pressure() {
    const MemoryReport rep = compute_memory_report();
    if (!rep.vram.pressured && !rep.ram.pressured) return;

    // 目标释放量：不超过我们追踪的可淘汰总量（VRAM 实测压力可能含我们无法释放的
    // 渲染目标等，封顶到 mesh+tex 账本，避免空转过度淘汰）。
    std::size_t vram_need = std::min(rep.vram.need_free_bytes,
                                     rep.vram.mesh_bytes + rep.vram.texture_bytes);
    std::size_t ram_need  = std::min(rep.ram.need_free_bytes,
                                     rep.ram.mesh_bytes + rep.ram.texture_bytes);
    if (vram_need == 0 && ram_need == 0) return;

    auto& hub = SharedDataHub::instance();
    const std::vector<ktm::fvec3> cameras = collect_camera_positions();

    struct Cand {
        std::uintptr_t scene;
        std::uintptr_t actor;
        float          dist;
    };
    std::vector<Cand> cands;
    {
        std::shared_lock lock(impl_->mtx);
        for (auto& [scene_handle, st] : impl_->scenes) {
            for (const auto& [actor, state] : st.actor_load_states) {
                if (state != ActorLoadState::Loaded) continue;
                if (st.loading_tasks.count(actor) || st.unloading_tasks.count(actor)) continue;
                if (auto a = hub.actor_storage().try_acquire_read(actor)) {
                    if (a->pinned) continue;
                } else {
                    continue;
                }
                float dist = 0.0f;
                if (auto eit = st.actor_to_entry.find(actor);
                    eit != st.actor_to_entry.end() && !cameras.empty()) {
                    const ktm::fvec3 c = eit->second.center();
                    dist = std::numeric_limits<float>::max();
                    for (const auto& cam : cameras)
                        dist = std::min(dist, ktm::distance(c, cam));
                }
                cands.push_back({scene_handle, actor, dist});
            }
        }
    }
    if (cands.empty()) return;

    // 最冷优先：距相机远者先淘汰
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.dist > b.dist;
    });

    // CPU 级联（清 Scene/Image）只允许在 RAM 真正承压时发生。
    // 仅 VRAM 承压 → gpu_only=true：释放 GPU、保留 Scene CPU（恢复时从 CPU 快速重建，
    // 不必磁盘重导）。这是"GPU 压力绝不误伤 CPU"的核心保证。
    const bool gpu_only = !rep.ram.pressured;

    constexpr std::size_t kMaxEvictionsPerPass = 64;
    std::vector<Events::ActorEvictRequestedEvent> to_evict;
    for (const auto& c : cands) {
        if (to_evict.size() >= kMaxEvictionsPerPass) break;
        if (vram_need == 0 && ram_need == 0) break;
        std::size_t g = 0, cpu = 0;
        estimate_actor_memory(c.actor, g, cpu);
        if (g == 0 && cpu == 0) continue;  // 无可释放，跳过
        to_evict.push_back({c.scene, c.actor, gpu_only});
        vram_need -= std::min(vram_need, g);
        // gpu_only 不清 CPU，故不计入 ram_need 的削减（避免误判已满足 RAM 目标）
        if (!gpu_only) ram_need -= std::min(ram_need, cpu);
    }
    if (to_evict.empty()) return;

    CFW_LOG_NOTICE("[GeometrySystem] Memory pressure: evicting {} cold actor(s), gpu_only={} "
                   "(VRAM {}MB/{}MB, RAM {}MB/{}MB)",
                   to_evict.size(), gpu_only,
                   rep.vram.used_bytes / (1024 * 1024), rep.vram.budget_bytes / (1024 * 1024),
                   rep.ram.used_bytes / (1024 * 1024), rep.ram.budget_bytes / (1024 * 1024));
    for (const auto& evt : to_evict) {
        if (impl_->ctx && impl_->ctx->event_bus())
            impl_->ctx->event_bus()->publish(evt);
    }
}

}  // namespace Corona::Systems


