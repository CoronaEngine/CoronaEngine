//
// Created by Zero on 2023/5/30.
//
// NOTE: Offline denoise has been disabled. 
// Normal/albedo buffers are now computed on-the-fly from visibility buffer,
// which is not compatible with OIDN's offline workflow.
// This file is kept for potential future re-implementation.
//
#include "base/denoiser.h"

#if 0  // OIDN offline denoise disabled

#include "OpenImageDenoise/oidn.hpp"

namespace vision {

using namespace ocarina;

class OIDN : public Denoiser {
private:
    oidn::DeviceRef device_{};

private:
    [[nodiscard]] oidn::DeviceRef create_device() const noexcept {
        return oidn::DeviceRef(oidn::newDevice(oidn::DeviceType::CPU));
    }
    [[nodiscard]] oidn::FilterRef create_filter() const noexcept {
        switch (mode_) {
            case RT:
                return device_.newFilter("RT");
            case RTLightmap:
                return device_.newFilter("RTLightmap");
            default:
                break;
        }
        return nullptr;
    }

public:
    explicit OIDN(const DenoiserDesc &desc)
        : Denoiser(desc),
          device_{create_device()} {
        device_.commit();
    }
    VS_MAKE_PLUGIN_NAME_FUNC

    void apply(uint2 res, float4 *output, float4 *color,
               float4 *normal, float4 *albedo) noexcept {
        TIMER(oidn_denoise)
        oidn::FilterRef filter = create_filter();
        filter.setImage("output", output, oidn::Format::Float3,
                        res.x, res.y, 0, sizeof(float4));
        filter.setImage("color", color, oidn::Format::Float3,
                        res.x, res.y, 0, sizeof(float4));
        if (normal && albedo) {
            filter.setImage("normal", normal, oidn::Format::Float3,
                            res.x, res.y, 0, sizeof(float4));
            filter.setImage("albedo", albedo, oidn::Format::Float3,
                            res.x, res.y, 0, sizeof(float4));
        }
        // color image is HDR
        filter.set("hdr", true);
        filter.commit();
        filter.execute();
        device_.sync();

        const char *errorMessage;
        if (device_.getError(errorMessage) != oidn::Error::None) {
            OC_ERROR_FORMAT("oidn error: {}", errorMessage)
        }
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR(vision::OIDN)

#endif  // OIDN offline denoise disabled