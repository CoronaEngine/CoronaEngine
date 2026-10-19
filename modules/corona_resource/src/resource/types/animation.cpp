#include "corona/resource/types/animation.h"
#include "corona/resource/resource_manager.h"
#include <mutex>
#include <map>

namespace Corona::Resource {

std::shared_future<AnimationSources> ResourceManager::import_animations_async(std::vector<std::filesystem::path> paths) {
    // Jobs retain the cache through shutdown. Function-static maps inside a
    // worker could be destroyed before ResourceManager joins outstanding jobs.
    using Key = std::tuple<std::filesystem::path, std::filesystem::file_time_type, std::uintmax_t>;
    struct Cache {
        std::mutex mutex;
        std::map<Key, std::weak_ptr<const AnimationSource>> entries;
    };
    static const auto cache = std::make_shared<Cache>();
    auto promise = std::make_shared<std::promise<AnimationSources>>();
    auto result = promise->get_future().share();
    import_arena_.execute([this, paths = std::move(paths), promise, cache = cache]() {
        async_tasks_.run([paths, promise, cache]() {
            try {
                // Weak entries retain no unused animation data. Timestamp/size
                // changes create a new version while existing players pin the old one.
                AnimationSources sources;
                for (const auto& path : paths) {
                    const auto canonical = std::filesystem::canonical(path);
                    const Key key{canonical, std::filesystem::last_write_time(canonical), std::filesystem::file_size(canonical)};
                    std::shared_ptr<const AnimationSource> source;
                    {
                        std::lock_guard lock(cache->mutex);
                        auto found = cache->entries.find(key);
                        if (found != cache->entries.end()) source = found->second.lock();
                    }
                    if (!source) {
                        source = read_animation_source(canonical);
                        std::lock_guard lock(cache->mutex);
                        std::erase_if(cache->entries, [](const auto& item) { return item.second.expired(); });
                        auto existing = cache->entries[key].lock();
                        if (existing) source = std::move(existing);
                        else cache->entries[key] = source;
                    }
                    sources.push_back(std::move(source));
                }
                promise->set_value(std::move(sources));
            } catch (...) { promise->set_exception(std::current_exception()); }
        });
    });
    return result;
}

Animation::Animation(const std::filesystem::path& path) : IResource(path) {}

AnimationParser::AnimationParser() {
    // register_extension(".anim");
    // register_extension(".clip");
}

}  // namespace Corona::Resource
