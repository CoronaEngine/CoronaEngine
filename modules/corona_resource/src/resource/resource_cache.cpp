#include "corona/resource/resource_cache.h"

#include "horizon/core/logging.h"

namespace Corona::Resource {

namespace {

std::chrono::system_clock::time_point now_time_point() {
    return std::chrono::system_clock::now();
}

}  // namespace

ResourceCache::~ResourceCache() {
    clear();
}

std::pair<std::shared_ptr<ResourceEntry>, bool> ResourceCache::get_or_create_entry(TResourceID rid) {
    std::unique_lock lock(mutex_);
    const auto [it, inserted] = resources_.try_emplace(rid);
    if (inserted) {
        auto entry = std::make_shared<ResourceEntry>();
        entry->state.store(LoadState::Loading, std::memory_order_relaxed);
        entry->last_access.store(now_time_point(), std::memory_order_relaxed);
        it->second = std::move(entry);
        return {it->second, true};
    }
    return {it->second, false};
}

std::shared_ptr<ResourceEntry> ResourceCache::get_entry(TResourceID rid) {
    std::shared_lock lock(mutex_);
    const auto it = resources_.find(rid);
    return it == resources_.end() ? nullptr : it->second;
}

bool ResourceCache::remove_entry(TResourceID rid) {
    std::unique_lock lock(mutex_);
    const auto it = resources_.find(rid);
    if (it == resources_.end()) {
        return false;
    }
    if (it->second->ref_count.load(std::memory_order_acquire) > 0) {
        CFW_LOG_DEBUG("[ResourceCache] Resource {} has {} active references, delay remove",
                      rid, it->second->ref_count.load(std::memory_order_relaxed));
        return false;
    }
    if (it->second->pinned.load(std::memory_order_acquire)) {
        CFW_LOG_DEBUG("[ResourceCache] Resource {} is pinned, delay remove", rid);
        return false;
    }
    resources_.erase(it);
    return true;
}

void ResourceCache::clear() {
    std::unique_lock lock(mutex_);
    resources_.clear();
}

bool ResourceCache::add_resource(TResourceID rid, std::shared_ptr<IResource> resource,
                                 std::size_t estimated_bytes) {
    if (!resource) return false;

    std::unique_lock lock(mutex_);
    const auto [it, inserted] = resources_.try_emplace(rid);
    if (!inserted) {
        return false;
    }

    auto entry = std::make_shared<ResourceEntry>();
    entry->resource = std::move(resource);
    entry->state.store(LoadState::Ready, std::memory_order_relaxed);
    entry->estimated_bytes.store(estimated_bytes, std::memory_order_relaxed);
    entry->last_access.store(now_time_point(), std::memory_order_relaxed);
    it->second = std::move(entry);
    return true;
}

// ============================================================================
// ResourceCache: pin / touch / budget
// ============================================================================

bool ResourceCache::pin(TResourceID rid) {
    std::shared_lock lock(mutex_);
    const auto it = resources_.find(rid);
    if (it == resources_.end()) return false;
    it->second->pinned.store(true, std::memory_order_release);
    return true;
}

bool ResourceCache::unpin(TResourceID rid) {
    std::shared_lock lock(mutex_);
    const auto it = resources_.find(rid);
    if (it == resources_.end()) return false;
    it->second->pinned.store(false, std::memory_order_release);
    return true;
}

bool ResourceCache::touch(TResourceID rid) {
    std::shared_lock lock(mutex_);
    const auto it = resources_.find(rid);
    if (it == resources_.end()) return false;
    it->second->last_access.store(now_time_point(), std::memory_order_relaxed);
    return true;
}

std::optional<ResourceEntryInfo> ResourceCache::entry_info(TResourceID rid) const {
    std::shared_lock lock(mutex_);
    const auto it = resources_.find(rid);
    if (it == resources_.end()) return std::nullopt;

    ResourceEntryInfo info;
    info.rid             = rid;
    info.estimated_bytes = it->second->estimated_bytes.load(std::memory_order_relaxed);
    info.ref_count       = it->second->ref_count.load(std::memory_order_relaxed);
    info.pinned          = it->second->pinned.load(std::memory_order_relaxed);
    info.state           = it->second->state.load(std::memory_order_relaxed);
    info.last_access     = it->second->last_access.load(std::memory_order_relaxed);
    return info;
}

std::vector<ResourceEntryInfo> ResourceCache::list_entries() const {
    std::shared_lock lock(mutex_);
    std::vector<ResourceEntryInfo> result;
    result.reserve(resources_.size());
    for (const auto& [rid, entry] : resources_) {
        ResourceEntryInfo info;
        info.rid             = rid;
        info.estimated_bytes = entry->estimated_bytes.load(std::memory_order_relaxed);
        info.ref_count       = entry->ref_count.load(std::memory_order_relaxed);
        info.pinned          = entry->pinned.load(std::memory_order_relaxed);
        info.state           = entry->state.load(std::memory_order_relaxed);
        info.last_access     = entry->last_access.load(std::memory_order_relaxed);
        result.push_back(info);
    }
    return result;
}

void ResourceCache::set_memory_budget(std::size_t bytes) {
    memory_budget_.store(bytes, std::memory_order_relaxed);
}

std::size_t ResourceCache::used_memory_bytes() const {
    std::shared_lock lock(mutex_);
    std::size_t total = 0;
    for (const auto& [rid, entry] : resources_) {
        total += entry->estimated_bytes.load(std::memory_order_relaxed);
    }
    return total;
}

std::size_t ResourceCache::memory_budget() const {
    return memory_budget_.load(std::memory_order_relaxed);
}

}  // namespace Corona::Resource
