#include <corona/resource/cache/lru_cache.h>

#include <horizon/core/logging.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <system_error>

namespace Corona::Cache {

// ============================================================================
// CacheRecord
// ============================================================================

CacheRecord::CacheRecord()
    : data_size(0)
    , last_access(std::chrono::system_clock::now()) {}

CacheRecord::CacheRecord(std::string k, const char* d, size_t sz)
    : key(std::move(k))
    , data(d, d + sz)
    , data_size(sz)
    , last_access(std::chrono::system_clock::now()) {}

// ============================================================================
// MemoryCache
// ============================================================================

MemoryCache::MemoryCache(size_t capacity_bytes)
    : capacity_(capacity_bytes) {}

MemoryCache::~MemoryCache() = default;

bool MemoryCache::put(const std::string& key, const char* data, size_t size) {
    // 更新已存在的 key
    auto it = map_.find(key);
    if (it != map_.end()) {
        auto& item = *(it->second);
        used_ -= item.data_size;
        item.data.assign(data, data + size);
        item.data_size = size;
        item.last_access = std::chrono::system_clock::now();
        used_ += size;
        // 提升到头部
        list_.splice(list_.begin(), list_, it->second);
        return true;
    }

    // 新项：检查容量
    if (used_ + size > capacity_ && !list_.empty()) {
        return false;  // 调用方应先 evict_lru()
    }

    // 插入到头部
    CacheRecord item(key, data, size);
    used_ += size;
    list_.push_front(std::move(item));
    map_[key] = list_.begin();
    return true;
}

std::optional<CacheRecord> MemoryCache::get(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;

    auto& item = *(it->second);
    item.last_access = std::chrono::system_clock::now();
    // 提升到头部
    list_.splice(list_.begin(), list_, it->second);
    // 显式构造返回值（CacheRecord 为 move-only，不可隐式拷贝），
    // 避免引用包装在后续 evict 中悬空。
    CacheRecord result(item.key, item.data.data(), item.data.size());
    result.last_access = item.last_access;
    return result;
}

void MemoryCache::erase(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return;
    used_ -= it->second->data_size;
    list_.erase(it->second);
    map_.erase(it);
}

EvictResult MemoryCache::evict_lru() {
    if (list_.empty()) return {EvictStatus::CacheEmpty};

    // 从尾部向头部查找第一个非 pinned 项
    auto it = list_.end();
    while (it != list_.begin()) {
        --it;
        if (it->pinned) continue;

        EvictResult result;
        result.status      = EvictStatus::Success;
        result.key         = it->key;
        result.bytes_freed = it->data_size;

        CacheRecord evicted = std::move(*it);
        map_.erase(evicted.key);
        used_ -= evicted.data_size;
        list_.erase(it);
        result.item = std::move(evicted);
        CFW_LOG_DEBUG("[LRU Memory] evicted key={} size={}KB used={}/{}KB",
                      result.key, result.bytes_freed / 1024,
                      used_ / 1024, capacity_ / 1024);
        return result;
    }
    return {EvictStatus::AllPinned};
}

bool MemoryCache::contains(const std::string& key) const {
    return map_.find(key) != map_.end();
}

bool MemoryCache::touch(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    auto& item = *(it->second);
    item.last_access = std::chrono::system_clock::now();
    list_.splice(list_.begin(), list_, it->second);
    return true;
}

bool MemoryCache::pin(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second->pinned = true;
    return true;
}

bool MemoryCache::unpin(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second->pinned = false;
    return true;
}

// ============================================================================
// DiskCache
// ============================================================================

DiskCache::DiskCache(size_t capacity_bytes, std::filesystem::path directory)
    : capacity_(capacity_bytes)
    , dir_(std::move(directory)) {
    // 创建缓存目录
    std::error_code ec;
    if (!std::filesystem::exists(dir_)) {
        std::filesystem::create_directories(dir_, ec);
        if (ec) {
            CFW_LOG_ERROR("[DiskCache] Failed to create directory {}: {}",
                          dir_.string(), ec.message());
            return;
        }
    }

    // 初始化：扫描已有文件
    size_t total = 0;
    for (auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.size() < 6 || filename.substr(filename.size() - 6) != ".cache") continue;
        // 文件名去掉 .cache 后缀即为 key 的哈希
        std::string stored_key = filename.substr(0, filename.size() - 6);

        auto fsize = entry.file_size(ec);
        if (ec) { ec.clear(); continue; }
        total += fsize;

        CacheRecord meta;
        meta.key = stored_key;
        meta.data_size = fsize;
        meta.last_access = std::chrono::system_clock::now();
        list_.push_back(stored_key);
        map_[stored_key] = {std::move(meta), std::prev(list_.end())};
    }
    CFW_LOG_NOTICE("[DiskCache] Initialized {} ({} files, {} bytes used)",
                   dir_.string(), map_.size(), total);
    used_ = total;  // 增量计数器初始化：等于初次扫描统计的真实磁盘用量。
}

DiskCache::~DiskCache() = default;

std::filesystem::path DiskCache::safe_path(const std::string& key) const {
    // 1. 拒绝空 key
    if (key.empty()) {
        throw std::invalid_argument("DiskCache: key is empty");
    }

    // 2. 拒绝路径穿越（".." 组件）
    if (key.find("..") != std::string::npos) {
        throw std::invalid_argument("DiskCache: key contains '..': " + key);
    }

    // 3. 拒绝绝对路径（Unix '/' / Windows 盘符 'C:' / UNC '\\\\'）
    if (key[0] == '/' || key[0] == '\\') {
        throw std::invalid_argument("DiskCache: key is absolute path: " + key);
    }
    if (key.size() >= 3 && std::isalpha(static_cast<unsigned char>(key[0]))
        && key[1] == ':' && (key[2] == '/' || key[2] == '\\')) {
        throw std::invalid_argument("DiskCache: key is absolute path: " + key);
    }

    // —— 字符清洗 ————————————————————————————————
    // 4. 将非法文件名字符替换为 '_'
    std::string safe;
    safe.reserve(key.size());
    for (char c : key) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            safe += '_';
        } else {
            safe += c;
        }
    }
    return dir_ / (safe + ".cache");
}

std::optional<CacheRecord> DiskCache::read_file(const std::string& key) const {
    std::filesystem::path path;
    try {
        path = safe_path(key);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    std::error_code ec;

    // 拒绝符号链接（防止通过 symlink 读取缓存目录外文件）
    if (std::filesystem::is_symlink(path, ec)) return std::nullopt;

    if (!std::filesystem::exists(path, ec)) return std::nullopt;

    auto fsize = std::filesystem::file_size(path, ec);
    if (ec || fsize == 0) return std::nullopt;

    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;

    CacheRecord item;
    item.key = key;
    item.data.resize(fsize);
    file.read(item.data.data(), static_cast<std::streamsize>(fsize));
    item.data_size = fsize;
    item.last_access = std::chrono::system_clock::now();
    return item;
}

bool DiskCache::write_file(const CacheRecord& item) const {
    std::filesystem::path path;
    try {
        path = safe_path(item.key);
    } catch (const std::invalid_argument&) {
        return false;
    }
    // 确保父目录存在（无符号链接）
    auto parent = path.parent_path();
    std::error_code ec;
    if (std::filesystem::is_symlink(parent, ec)) {
        CFW_LOG_ERROR("[DiskCache] Refusing to write to symlinked directory: {}",
                      parent.string());
        return false;
    }
    if (!std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }

    // 确保文件路径本身非符号链接（防止通过 symlink 写入任意位置）
    if (std::filesystem::is_symlink(path, ec)) {
        CFW_LOG_ERROR("[DiskCache] Refusing to write to symlinked file: {}",
                      path.string());
        return false;
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        CFW_LOG_ERROR("[DiskCache] Failed to open {} for writing", path.string());
        return false;
    }
    file.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    return file.good();
}

bool DiskCache::put(CacheRecord item) {
    auto key = item.key;
    size_t item_size = item.data.size();
    bool is_new_entry = false;

    // ====================================================================
    // Phase 1: Lock — 索引操作（无磁盘 IO）
    // ====================================================================
    {
        std::lock_guard lock(mtx_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            // ——— UPDATE 路径 ———
            // 如果已 pin，拒绝覆盖
            if (it->second.first.pinned) return false;
            // 删除旧文件（unlink 很快，远快于 write，安全在锁内执行）
            std::error_code ec;
            std::filesystem::remove(safe_path(key), ec);
            // 增量计数器：先减去旧大小，写盘成功后再加新大小（Phase 2 后处理）
            used_ -= it->second.first.data_size;
            // 逐字段更新元数据，保留 item 完整用于 Phase 2 write_file
            it->second.first.key = item.key;
            it->second.first.data.clear();
            it->second.first.data_size = item.data_size;
            it->second.first.last_access = item.last_access;
            it->second.first.pinned = item.pinned;
            // 提升到 LRU 头部
            list_.splice(list_.begin(), list_, it->second.second);
        } else {
            // ——— INSERT 路径 ———
            // 增量计数器替代 calc_directory_size() 全目录扫描：O(1) 容量检查。
            while (used_ + item_size > capacity_) {
                if (!evict_one(key).ok()) break;
            }
            if (used_ + item_size > capacity_) {
                return false;
            }

            CacheRecord meta;
            meta.key = key;
            meta.data_size = item_size;
            meta.last_access = std::chrono::system_clock::now();
            list_.push_front(key);
            map_[key] = {std::move(meta), list_.begin()};
            used_ += item_size;  // 新条目已计入，write 失败时回滚
            is_new_entry = true;
        }
    }  // ——— 锁释放 ———

    // ====================================================================
    // Phase 2: Unlocked — 磁盘 write（重量级 IO，不阻塞其他索引操作）
    // ====================================================================
    if (!write_file(item)) {
        CFW_LOG_ERROR("[LRU Disk] write failed key={} size={}KB", key, item_size / 1024);
        std::lock_guard lock(mtx_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            // 无论 INSERT 还是 UPDATE：写盘失败意味着磁盘上已无有效文件
            // （INSERT 从未写成功；UPDATE 旧文件在 Phase 1 已删、新文件写失败）。
            // 统一移除索引项以保持 used_ 与磁盘一致、避免悬挂条目指向已删文件。
            // used_ 在 Phase 1 已扣减（UPDATE 减旧 / INSERT 加新后此处减新），
            // 这里仅对 INSERT 还原其已加的 item_size；UPDATE 不需再动（旧已减、新未加）。
            if (is_new_entry) used_ -= item_size;
            list_.erase(it->second.second);
            map_.erase(it);
        }
        return false;
    }

    // ====================================================================
    // Phase 3: UPDATE 路径 — 写盘成功，加上新大小
    // ====================================================================
    if (!is_new_entry) {
        std::lock_guard lock(mtx_);
        used_ += item_size;
    }

    CFW_LOG_DEBUG("[LRU Disk] stored key={} size={}KB files={}", key, item_size / 1024, map_.size());
    return true;
}

std::optional<CacheRecord> DiskCache::get(const std::string& key) {
    // ====================================================================
    // Phase 1: Lock — 检查 key 是否存在（无磁盘 IO）
    // ====================================================================
    {
        std::lock_guard lock(mtx_);
        if (map_.find(key) == map_.end()) return std::nullopt;
    }

    // ====================================================================
    // Phase 2: Unlocked — 磁盘 read（重量级 IO）
    // ====================================================================
    auto data = read_file(key);
    if (!data) {
        std::lock_guard lock(mtx_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            used_ -= it->second.first.data_size;  // 文件已不可读，同步扣减增量计数器
            list_.erase(it->second.second);
            map_.erase(it);
        }
        return std::nullopt;
    }

    // ====================================================================
    // Phase 3: Lock — 提升到 LRU 头部 + 更新访问时间（无磁盘 IO）
    // ====================================================================
    {
        std::lock_guard lock(mtx_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.splice(list_.begin(), list_, it->second.second);
            it->second.first.last_access = std::chrono::system_clock::now();
        }
    }

    return data;
}

void DiskCache::erase(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end()) return;

    std::error_code ec;
    std::filesystem::remove(safe_path(key), ec);
    used_ -= it->second.first.data_size;  // 增量计数器：删除时同步扣减
    list_.erase(it->second.second);
    map_.erase(it);
}

bool DiskCache::contains(const std::string& key) const {
    std::lock_guard lock(mtx_);
    return map_.find(key) != map_.end();
}

bool DiskCache::touch(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second.first.last_access = std::chrono::system_clock::now();
    list_.splice(list_.begin(), list_, it->second.second);
    return true;
}

bool DiskCache::pin(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second.first.pinned = true;
    return true;
}

bool DiskCache::unpin(const std::string& key) {
    std::lock_guard lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    it->second.first.pinned = false;
    return true;
}

bool DiskCache::is_pinned(const std::string& key) const {
    std::lock_guard lock(mtx_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    return it->second.first.pinned;
}

size_t DiskCache::used_bytes() {
    std::lock_guard lock(mtx_);
    return used_;  // O(1)：直接返回增量计数器，无需全目录 stat 扫描。
}

EvictResult DiskCache::evict_one(const std::string& skip_key) {
    // 调用方（put）已持有 mtx_，此处不加锁
    // 从尾部（最旧）向头部查找可淘汰项（跳过 skip_key 和 pinned 项）。
    // 注意：在找到 victim 后立即 return，因此反向迭代器不会在被 erase 后继续使用。
    for (auto rit = list_.rbegin(); rit != list_.rend(); ++rit) {
        if (*rit == skip_key) continue;
        auto map_it = map_.find(*rit);
        if (map_it == map_.end()) continue;
        if (map_it->second.first.pinned) continue;  // 不可淘汰

        // 保存 forward iterator（比存 rit 更安全）
        auto forward_it = map_it->second.second;
        std::string victim_key = *rit;
        size_t victim_bytes = map_it->second.first.data_size;

        std::error_code ec;
        std::filesystem::remove(safe_path(victim_key), ec);
        list_.erase(forward_it);
        map_.erase(map_it);
        used_ -= victim_bytes;  // 增量计数器：淘汰时同步扣减

        return {EvictStatus::Success, victim_key, victim_bytes, std::nullopt};
    }
    return {EvictStatus::AllPinned};
}

// ============================================================================
// CacheManager
// ============================================================================

CacheManager::CacheManager(size_t mem_capacity, size_t disk_capacity,
                           std::filesystem::path disk_dir)
    : mem_(mem_capacity) {
    if (disk_capacity > 0) {
        disk_.emplace(disk_capacity, std::move(disk_dir));
    }
}

bool CacheManager::put(const std::string& key, const char* data, size_t size) {
    // 如果单条数据超过内存总容量，直接拒绝（永不驱逐命中率判死刑的数据）
    if (size > mem_.capacity_bytes()) return false;

    constexpr int kMaxFlushIterations = 1000;

    for (int attempt = 0; attempt < kMaxFlushIterations; ++attempt) {
        // Phase 1: Lock — 尝试内存插入
        {
            std::lock_guard lock(mtx_);
            if (mem_.put(key, data, size)) return true;
        }

        // Phase 2: Lock — 淘汰一个 victim
        EvictResult evict_res;
        {
            std::lock_guard lock(mtx_);
            evict_res = mem_.evict_lru();
        }

        if (!evict_res.ok()) return false;  // 全部 pinned 或缓存空

        // Phase 3: Unlocked — 将 victim 刷盘（DiskCache 用自己的内部锁）
        auto& victim = evict_res.item;
        // 刷盘前保留回滚副本：std::move 后 victim->data 为空，无法用于回滚
        const std::string rollback_key   = victim->key;
        const std::vector<char> rollback_data = victim->data;
        const size_t rollback_size       = victim->data_size;

        if (disk_) {
            CacheRecord item;
            item.key         = rollback_key;
            item.data        = std::move(victim->data);
            item.data_size   = rollback_size;
            item.last_access = victim->last_access;

            if (!disk_->put(std::move(item))) {
                std::lock_guard lock(mtx_);
                if (!mem_.put(rollback_key, rollback_data.data(), rollback_size)) {
                    if (evict_cb_) evict_cb_(rollback_key, rollback_data);
                }
                return false;
            }
        } else if (evict_cb_) {
            evict_cb_(victim->key, victim->data);
        }
        // 循环继续：尝试再次插入（可能还需淘汰更多项）
    }

    return false;  // 超出最大迭代次数
}

std::optional<CacheRecord> CacheManager::get(const std::string& key) {
    // Phase 1: Lock — 查内存
    {
        std::lock_guard lock(mtx_);
        auto mem_result = mem_.get(key);
        if (mem_result) return mem_result;
    }

    // Phase 2: Unlocked — 查磁盘（DiskCache 用自己的内部锁）
    if (!disk_) return std::nullopt;
    auto disk_result = disk_->get(key);
    if (!disk_result) return std::nullopt;

    // Phase 3: 提升到内存（可能需 evict + 刷盘腾空间）
    auto& d = disk_result->data;
    for (int attempt = 0; attempt < 2; ++attempt) {
        EvictResult evict_res;
        {
            std::lock_guard lock(mtx_);
            if (mem_.used_bytes() + d.size() <= mem_.capacity_bytes()) {
                mem_.put(key, d.data(), d.size());
                if (disk_ && disk_->is_pinned(key)) {
                    mem_.pin(key);
                }
                return disk_result;
            }
            evict_res = mem_.evict_lru();
        }

        if (!evict_res.ok()) break;  // 全部 pinned 或缓存空，放弃提升

        // 将 victim 刷盘（DiskCache 锁独立，不阻塞 mem_）
        auto& victim = evict_res.item;
        if (disk_) {
            const std::string rollback_key   = victim->key;
            const std::vector<char> rollback_data = victim->data;
            const size_t rollback_size       = victim->data_size;
            CacheRecord item;
            item.key        = rollback_key;
            item.data       = std::move(victim->data);
            item.data_size  = rollback_size;
            item.last_access = victim->last_access;
            if (!disk_->put(std::move(item))) {
                CFW_LOG_ERROR("[LRU CacheMgr] promotion: failed to flush {} to disk, rolling back",
                             rollback_key);
                std::lock_guard lock(mtx_);
                if (!mem_.put(rollback_key, rollback_data.data(), rollback_size)) {
                    if (evict_cb_) evict_cb_(rollback_key, rollback_data);
                }
                break;  // 放弃本次提升，返回磁盘数据
            }
        } else if (evict_cb_) {
            evict_cb_(victim->key, victim->data);
        }
    }

    // 无法提升到内存（内存全 pinned）— 仍返回磁盘数据
    return disk_result;
}

void CacheManager::erase(const std::string& key) {
    std::lock_guard lock(mtx_);
    mem_.erase(key);
    if (disk_) disk_->erase(key);
}

bool CacheManager::contains(const std::string& key) {
    std::lock_guard lock(mtx_);
    if (mem_.contains(key)) return true;
    if (disk_ && disk_->contains(key)) return true;
    return false;
}

bool CacheManager::touch(const std::string& key) {
    std::lock_guard lock(mtx_);
    if (mem_.touch(key)) return true;
    if (disk_ && disk_->touch(key)) return true;
    return false;
}

bool CacheManager::pin(const std::string& key) {
    std::lock_guard lock(mtx_);
    bool found = false;
    if (mem_.pin(key)) found = true;
    if (disk_ && disk_->pin(key)) found = true;
    return found;
}

bool CacheManager::unpin(const std::string& key) {
    std::lock_guard lock(mtx_);
    bool found = false;
    if (mem_.unpin(key)) found = true;
    if (disk_ && disk_->unpin(key)) found = true;
    return found;
}

size_t CacheManager::memory_used() const {
    std::lock_guard lock(mtx_);
    return mem_.used_bytes();
}

size_t CacheManager::memory_capacity() const {
    return mem_.capacity_bytes();
}

size_t CacheManager::disk_used() {
    std::lock_guard lock(mtx_);
    return disk_ ? disk_->used_bytes() : 0;
}

size_t CacheManager::disk_capacity() const {
    return disk_ ? disk_->capacity_bytes() : 0;
}

void CacheManager::set_evict_callback(EvictCallback cb) {
    std::lock_guard lock(mtx_);
    evict_cb_ = std::move(cb);
}

}  // namespace Corona::Cache
