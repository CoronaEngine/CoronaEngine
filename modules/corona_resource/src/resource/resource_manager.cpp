#include "corona/resource/resource_manager.h"

#include "horizon/core/logging.h"

#include <unordered_set>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
/**
 * @brief 将 std::filesystem::path 转换为 UTF-8 编码的字符串
 * 用于日志输出，确保中文路径正确显示
 */
inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& wstr = path.native();
    if (wstr.empty()) {
        return {};
    }
    int const size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                         static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return path.string();
    }
    std::string utf8_str(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                        static_cast<int>(wstr.size()), utf8_str.data(), size, nullptr, nullptr);
    return utf8_str;
#else
    return path.string();
#endif
}
}  // namespace

namespace Corona::Resource {

ResourceManager& ResourceManager::get_instance() {
    static ResourceManager instance;
    return instance;
}

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager() {
    // 取消所有正在进行的异步任务并等待它们完成
    async_tasks_.cancel();
    async_tasks_.wait();
    parser_registry_.clear();
    resource_cache_.clear();
}

TResourceID ResourceManager::import_sync(const std::filesystem::path& path) {
    return load_internal(path);
}

std::future<TResourceID> ResourceManager::import_async(const std::filesystem::path& path) {
    auto promise = std::make_shared<std::promise<TResourceID>>();
    auto future = promise->get_future();

    // 将加载任务提交到任务组
    import_arena_.execute([this, path, promise]() {
        async_tasks_.run([this, path, promise]() {
            try {
                promise->set_value(load_internal(path));
            } catch (...) {
                promise->set_value(IResource::INVALID_UID);
            }
        });
    });

    return future;
}

bool ResourceManager::export_sync(TResourceID rid, const std::filesystem::path& path) {
    auto const parser = parser_registry_.find_export_parser(path);
    if (!parser) {
        return false;
    }

    if (auto const handle = resource_cache_.acquire_read(rid)) {
        return parser->export_to(*handle, path);
    }
    return false;
}

std::future<bool> ResourceManager::export_async(TResourceID rid, const std::filesystem::path& path) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();

    async_tasks_.run([this, rid, path, promise]() {
        promise->set_value(export_sync(rid, path));
    });
    return future;
}

bool ResourceManager::remove_cache(TResourceID rid) {
    return resource_cache_.remove_entry(rid);
}

std::future<bool> ResourceManager::remove_cache_async(TResourceID rid) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    async_tasks_.run([this, rid, promise]() {
        promise->set_value(try_evict(rid).success);
    });
    return future;
}

bool ResourceManager::add_resource(TResourceID const rid, std::shared_ptr<IResource> resource,
                                    std::size_t estimated_bytes) {
    return resource_cache_.add_resource(rid, std::move(resource), estimated_bytes);
}

// ============================================================================
// P1 扩展：pin / touch / budget / evict
// ============================================================================

bool ResourceManager::pin(TResourceID rid) {
    return resource_cache_.pin(rid);
}

bool ResourceManager::unpin(TResourceID rid) {
    return resource_cache_.unpin(rid);
}

bool ResourceManager::touch(TResourceID rid) {
    return resource_cache_.touch(rid);
}

std::optional<ResourceEntryInfo> ResourceManager::entry_info(TResourceID rid) const {
    return resource_cache_.entry_info(rid);
}

std::vector<ResourceEntryInfo> ResourceManager::list_entries() const {
    return resource_cache_.list_entries();
}

void ResourceManager::set_memory_budget(std::size_t bytes) {
    resource_cache_.set_memory_budget(bytes);
}

std::size_t ResourceManager::used_memory_bytes() const {
    return resource_cache_.used_memory_bytes();
}

std::size_t ResourceManager::memory_budget() const {
    return resource_cache_.memory_budget();
}

EvictResult ResourceManager::try_evict(TResourceID rid) {
    auto info = resource_cache_.entry_info(rid);
    if (!info) return {false, rid, 0};

    EvictResult result;
    result.rid = rid;

    if (info->pinned) {
        CFW_LOG_DEBUG("[ResourceManager] try_evict: resource {} is pinned", rid);
        return result;
    }
    if (info->ref_count > 0) {
        CFW_LOG_DEBUG("[ResourceManager] try_evict: resource {} has {} active references",
                      rid, info->ref_count);
        return result;
    }

    result.bytes_freed = info->estimated_bytes;
    result.success = resource_cache_.remove_entry(rid);
    if (result.success) {
        CFW_LOG_DEBUG("[ResourceManager] Evicted resource {} ({} bytes freed)", rid, result.bytes_freed);
    }
    return result;
}

EvictResult ResourceManager::evict_until_under_budget() {
    std::size_t budget = resource_cache_.memory_budget();
    if (budget == 0) return {};  // 不限制

    EvictResult last_result;
    constexpr int kMaxIterations = 1000;
    int evicted_count = 0;

    for (int i = 0; i < kMaxIterations; ++i) {
        if (resource_cache_.used_memory_bytes() <= budget) break;

        // 查找最旧的未 pin 且无引用的条目（LRU 策略：last_access 最早者）
        auto entries = resource_cache_.list_entries();
        TResourceID oldest_rid = 0;
        auto oldest_time = std::chrono::system_clock::time_point::max();

        for (const auto& e : entries) {
            if (e.pinned || e.ref_count > 0) continue;
            if (e.state != LoadState::Ready) continue;  // 只淘汰已就绪的资源
            if (e.last_access < oldest_time) {
                oldest_time = e.last_access;
                oldest_rid = e.rid;
            }
        }

        if (oldest_rid == 0) {
            CFW_LOG_WARNING("[ResourceManager] all {} entries pinned or in use, budget eviction stalled", entries.size());
            break;
        }

        last_result = try_evict(oldest_rid);
        if (!last_result.success) break;
        evicted_count++;
    }

    if (evicted_count > 0) {
        CFW_LOG_NOTICE("[ResourceManager] budget eviction freed {} resources, {}MB used / {}MB budget",
                       evicted_count, resource_cache_.used_memory_bytes() / (1024*1024),
                       budget / (1024*1024));
    }
    return last_result;
}

TResourceID ResourceManager::load_internal(const std::filesystem::path& path) {
    // 1. 检查路径是否为空
    if (path.empty()) {
        CFW_LOG_ERROR("[ResourceManager] Cannot load resource: path is empty");
        return IResource::INVALID_UID;
    }

    // 2. 规范化路径（处理相对路径、冗余分隔符等）
    std::filesystem::path normalized_path;
    try {
        // 如果是相对路径，转换为绝对路径
        if (path.is_relative()) {
            normalized_path = std::filesystem::absolute(path);
        } else {
            normalized_path = path;
        }
        // 规范化路径（移除 "." 和 ".." 等）
        normalized_path = std::filesystem::weakly_canonical(normalized_path);
    } catch (const std::filesystem::filesystem_error& e) {
        CFW_LOG_ERROR("[ResourceManager] Invalid path '{}': {}", path_to_utf8(path), e.what());
        return IResource::INVALID_UID;
    }

    // 3. 检查路径是否存在
    std::error_code ec;
    if (!std::filesystem::exists(normalized_path, ec)) {
        if (ec) {
            CFW_LOG_ERROR("[ResourceManager] Cannot access path '{}': {}", path_to_utf8(normalized_path), ec.message());
        } else {
            CFW_LOG_ERROR("[ResourceManager] File not found: '{}'", path_to_utf8(normalized_path));
        }
        return IResource::INVALID_UID;
    }

    // 4. 检查是否为常规文件（而非目录）
    if (!std::filesystem::is_regular_file(normalized_path, ec)) {
        CFW_LOG_ERROR("[ResourceManager] Path is not a regular file: '{}'", path_to_utf8(normalized_path));
        return IResource::INVALID_UID;
    }

    auto const rid = IResource::generate_uid(normalized_path);

    // 防止递归导入自死锁：parser 在解析资源时可能间接调用 import_sync(X) →
    // load_internal(X) 回到同一资源，此时本线程已持有 Loading 状态，再进入会
    // cv.wait() 永久阻塞等自己完成。用 thread_local 集合追踪并提前中断循环。
    thread_local std::unordered_set<TResourceID> tls_loading_resources;
    if (!tls_loading_resources.insert(rid).second) {
        CFW_LOG_ERROR("[ResourceManager] Circular import detected for resource: '{}'",
                      path_to_utf8(normalized_path));
        return IResource::INVALID_UID;
    }
    struct TlsGuard {
        std::unordered_set<TResourceID>* set;
        TResourceID rid;
        ~TlsGuard() { set->erase(rid); }
    } tls_guard{&tls_loading_resources, rid};

    if (auto [entry, is_creator] = resource_cache_.get_or_create_entry(rid); is_creator) {
        std::shared_ptr<IResource> resource = nullptr;
        bool success = false;

        try {
            if (const auto parser = parser_registry_.find_parser(normalized_path)) {
                resource = parser->import_from(normalized_path, this->resource_cache_);
                if (resource) {
                    success = true;
                    CFW_LOG_DEBUG("[ResourceManager] Successfully loaded resource: '{}'", path_to_utf8(normalized_path));
                } else {
                    CFW_LOG_ERROR("[ResourceManager] Parser returned null for: '{}'", path_to_utf8(normalized_path));
                }
            } else {
                CFW_LOG_ERROR("[ResourceManager] No parser found for file type: '{}'", normalized_path.extension().string());
            }
        } catch (const std::exception& e) {
            CFW_LOG_ERROR("[ResourceManager] Exception while loading '{}': {}", path_to_utf8(normalized_path), e.what());
        } catch (...) {
            CFW_LOG_ERROR("[ResourceManager] Unknown exception while loading '{}'", path_to_utf8(normalized_path));
        }

        std::unique_lock lock(entry->mutex);
        if (success) {
            entry->resource = resource;
            entry->state = LoadState::Ready;
            // 用文件大小作为内存占用的初始估算值
            // 实际内存占用通常接近文件大小（纹理/模型等压缩资源加载后会膨胀，
            // 但作为 LRU 淘汰优先级排序的依据，文件大小是一个合理的一阶近似）
            {
                std::error_code ec;
                auto fsize = std::filesystem::file_size(normalized_path, ec);
                if (!ec && fsize > 0) {
                    entry->estimated_bytes = static_cast<std::size_t>(fsize);
                }
            }
            lock.unlock();
            entry->cv.notify_all();  // 通知等待的线程加载完成
            return rid;
        } else {
            entry->state = LoadState::Failed;
            resource_cache_.remove_entry(rid);  // 先移除再 notify，防止 pin 竞争
            lock.unlock();
            entry->cv.notify_all();
            return IResource::INVALID_UID;
        }
    } else {
        // 如果不是创建者，等待资源加载完成
        std::shared_lock lock(entry->mutex);
        entry->cv.wait(lock, [&entry] { return entry->state != LoadState::Loading; });
        if (entry->state == LoadState::Ready) {
            return rid;
        }
        return IResource::INVALID_UID;
    }
}
}  // namespace Corona::Resource
