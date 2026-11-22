#pragma once

#include "base/mgr/pipeline.h"
#include "radiance_storage.h"

namespace vision {

class DIRadiance final : private Toolkit {
private:
    SP<ScreenBuffer> radiance_{make_shared<ScreenBuffer>("DI radiance")};
    mutable RegistrableBuffer<uint2> raw_radiance_{pipeline()->bindless_array()};
    bool compact_{};
    bool prepared_{};

    void allocate_compact() {
        const uint count = pipeline()->pixel_num();
        raw_radiance_.super() = device().create_buffer<uint2>(count, radiance_->name() + " rgb21");
        raw_radiance_.register_self();
        vector<uint2> zero(count, make_uint2(0u));
        raw_radiance_.upload_immediately(zero.data());
    }

public:
    [[nodiscard]] const SP<ScreenBuffer> &radiance() const noexcept { return radiance_; }
    [[nodiscard]] bool compact_radiance() const noexcept { return compact_; }
    [[nodiscard]] const RegistrableBuffer<uint2> &raw_radiance() const noexcept { return raw_radiance_; }

    void prepare(bool compact = false) noexcept { set_compact_radiance(compact); }

    void set_compact_radiance(bool compact) noexcept {
        if (prepared_ && compact == compact_) return;
        if (prepared_) {
            // Keep descriptor indices stable for the kernels that captured them.
            ByteBufferView empty{handle_ty{0u}, 0u};
            if (compact_) {
                pipeline()->bindless_array().set_buffer(raw_radiance_.index().hv(), empty);
                raw_radiance_.super().destroy();
            } else {
                frame_buffer().unregister(radiance_);
                pipeline()->bindless_array().set_buffer(radiance_->index().hv(), empty);
                radiance_->clear_all();
            }
        }
        compact_ = compact;
        if (compact_) {
            allocate_compact();
        } else {
            frame_buffer().prepare_screen_buffer(radiance_);
            vector<float4> zero(pipeline()->pixel_num(), make_float4(0.f));
            radiance_->device_buffer().upload_immediately(zero.data());
        }
        prepared_ = true;
    }

    void update_resolution(uint2) noexcept {
        // The framebuffer resizes its registered full-precision screen buffers.
        if (compact_) allocate_compact();
    }

    [[nodiscard]] Float4 read_radiance(const Uint &index) const noexcept {
        return compact_ ? unpack_restir_radiance(raw_radiance_.read(index)) : radiance_->read(index);
    }

    void write_radiance(const Uint &index, const Float4 &value) const noexcept {
        if (compact_) raw_radiance_.write(index, pack_restir_radiance(value));
        else radiance_->write(index, value);
    }

    [[nodiscard]] vector<float4> download_radiance() const noexcept {
        vector<float4> result(pipeline()->pixel_num());
        if (compact_) {
            vector<uint2> stored(result.size());
            stream() << raw_radiance_.view().download(stored.data()) << synchronize() << ocarina::commit();
            for (size_t i = 0u; i < result.size(); ++i) result[i] = unpack_restir_radiance_host(stored[i]);
        } else {
            stream() << radiance_->view().download(result.data()) << synchronize() << ocarina::commit();
        }
        return result;
    }
};

}// namespace vision
