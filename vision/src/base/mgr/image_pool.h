//
// Created by Zero on 26/09/2022.
//

#pragma once

#include "rhi/common.h"
#include "base/import/node_desc.h"
#include "core/image/image.h"
#include "base/using.h"

namespace vision {
class ImagePool {
public:
    // Source-scoped CPU images. Descriptor hashes include decoding options;
    // separate source revisions use separate caches even for identical paths.
    using SourceCache = map<uint64_t, SP<const Image>>;

private:
    map<uint64_t, RegistrableTexture3D> textures_;
    SP<SourceCache> source_cache_;
    static ImagePool *s_image_pool;
    ImagePool(const ImagePool &) = delete;
    ImagePool(ImagePool &&) = delete;
    ImagePool operator=(const ImagePool &) = delete;
    ImagePool operator=(ImagePool &&) = delete;

public:
    ImagePool() = default;
    void set_source_cache(SP<SourceCache> cache) { source_cache_ = ocarina::move(cache); }
    static ImagePool &instance();
    static void destroy_instance();
    [[nodiscard]] RegistrableTexture3D load_texture(const ShaderNodeDesc &desc,
                                                    BindlessArray &bindless_array,
                                                    Device &device) noexcept;
    [[nodiscard]] RegistrableTexture3D &obtain_texture(const ShaderNodeDesc &desc,
                                                       BindlessArray &bindless_array,
                                                       Device &device) noexcept;
    void prepare(Stream &stream) noexcept;
    [[nodiscard]] bool is_contain(uint64_t hash) const noexcept { return textures_.contains(hash); }
    [[nodiscard]] size_t size() const noexcept { return textures_.size(); }
};

}// namespace vision
