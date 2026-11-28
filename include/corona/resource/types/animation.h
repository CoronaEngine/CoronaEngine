#pragma once
#include "corona/resource/resource.h"
#include "corona/resource/types/scene.h"

namespace Corona::Resource {

// Animation-only imports never allocate mesh/material/GPU resources and do not
// occupy the Scene cache entry for the same filename.
struct AnimationSource {
    SkeletonData skeleton;
    std::vector<AnimationClip> clips;
};
std::shared_ptr<const AnimationSource> read_animation_source(const std::filesystem::path& path);

class Animation : public IResource {
   public:
    explicit Animation(const std::filesystem::path& path);
    ~Animation() override = default;
};

class AnimationParser : public IParser {
   public:
    AnimationParser();
    ~AnimationParser() override = default;

   protected:
};

}  // namespace Corona::Resource
