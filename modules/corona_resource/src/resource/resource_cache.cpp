#include "corona/resource/resource_cache.h"

#include "horizon/core/logging.h"

namespace Corona::Resource {

ResourceCache::~ResourceCache() {
    clear();
}

std::pair<std::shared_ptr<ResourceEntry>, bool> ResourceCache::get_or_create_entry(TResourceID rid) {
    typename decltype(resources_)::accessor accessor;
    if (resources_.insert(accessor, rid)) {
        auto entry = std::make_shared<ResourceEntry>();
        entry->state = LoadState::Loading;
        entry->last_access = std::chrono::system_clock::now();
        accessor->second = entry;
        return {entry, true};
    }
    return {accessor->second, false};
}

std::shared_ptr<ResourceEntry> ResourceCache::get_entry(TResourceID rid) {
    typename decltype(resources_)::const_accessor accessor;
    if (resources_.find(accessor, rid)) {
        return accessor->second;
    }
    return nullptr;
}

bool ResourceCache::remove_entry(TResourceID rid) {
    typename decltype(resources_)::accessor accessor;
    if (resources_.find(accessor, rid)) {
        if (accessor->second->ref_count > 0) {
            CFW_LOG_DEBUG("[ResourceCache] Resource {} has {} active references, delay remove",
                         rid, accessor->second->ref_count.load());
            return false;
        }
        if (accessor->second->pinned) {
            CFW_LOG_DEBUG("[ResourceCache] Resource {} is pinned, delay remove", rid);
            return false;
        }
        return resources_.erase(accessor);
    }
    return false;
}

void ResourceCache::clear() {
    resources_.clear();
}

bool ResourceCache::add_resource(TResourceID rid, std::shared_ptr<IResource> resource,
                                  std::size_t estimated_bytes) {
    if (!resource) return false;

    typename decltype(resources_)::accessor accessor;
    if (resources_.insert(accessor, rid)) {
        auto entry = std::make_shared<ResourceEntry>();
        entry->resource = std::move(resource);
        entry->state = LoadState::Ready;
        entry->estimated_bytes = estimated_bytes;
        entry->last_access = std::chrono::system_clock::now();
        accessor->second = entry;
        return true;
    }

    return false;
}

// ============================================================================
// ResourceCache: pin / touch / budget
// ============================================================================

bool ResourceCache::pin(TResourceID rid) {
    typename decltype(resources_)::accessor accessor;
    if (!resources_.find(accessor, rid)) return false;
    accessor->second->pinned = true;
    return true;
}

bool ResourceCache::unpin(TResourceID rid) {
    typename decltype(resources_)::accessor accessor;
    if (!resources_.find(accessor, rid)) return false;
    accessor->second->pinned = false;
    return true;
}

bool ResourceCache::touch(TResourceID rid) {
    typename decltype(resources_)::accessor accessor;
    if (!resources_.find(accessor, rid)) return false;
    accessor->second->last_access = std::chrono::system_clock::now();
    return true;
}

std::optional<ResourceEntryInfo> ResourceCache::entry_info(TResourceID rid) const {
    typename decltype(resources_)::const_accessor accessor;
    if (!resources_.find(accessor, rid)) return std::nullopt;

    ResourceEntryInfo info;
    info.rid             = rid;
    info.estimated_bytes = accessor->second->estimated_bytes;
    info.ref_count       = accessor->second->ref_count;
    info.pinned          = accessor->second->pinned;
    info.state           = accessor->second->state;
    info.last_access     = accessor->second->last_access;
    return info;
}

std::vector<ResourceEntryInfo> ResourceCache::list_entries() const {
    std::vector<ResourceEntryInfo> result;
    result.reserve(resources_.size());
    for (auto it = resources_.begin(); it != resources_.end(); ++it) {
        ResourceEntryInfo info;
        info.rid             = it->first;
        auto& entry           = it->second;
        info.estimated_bytes  = entry->estimated_bytes;
        info.ref_count        = entry->ref_count;
        info.pinned           = entry->pinned;
        info.state            = entry->state;
        info.last_access      = entry->last_access;
        result.push_back(info);
    }
    return result;
}

void ResourceCache::set_memory_budget(std::size_t bytes) {
    memory_budget_ = bytes;
}

std::size_t ResourceCache::used_memory_bytes() const {
    std::size_t total = 0;
    for (auto it = resources_.begin(); it != resources_.end(); ++it) {
        total += it->second->estimated_bytes.load();
    }
    return total;
}

std::size_t ResourceCache::memory_budget() const {
    return memory_budget_;
}

}  // namespace Corona::Resource
