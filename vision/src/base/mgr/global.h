//
// Created by Zero on 2023/6/14.
//

#pragma once

#include "rhi/context.h"
#include "image_pool.h"
#include "rhi/common.h"
#include "base/using.h"

namespace vision {
class Pipeline;
class Spectrum;
class Global {
    OC_MAKE_INSTANCE_CONSTRUCTOR(Global, s_global)
    ~Global();

private:
    struct SceneGpuContext {
        BindlessArray *bindless_array{};
        Device *device{};
    };

    weak_ptr<Pipeline> pipeline_{};
    Device *device_{nullptr};
    SceneGpuContext scene_gpu_context_{};
    fs::path scene_path_;

public:
    class SceneGpuContextScope {
    private:
        Global &global_;
        SceneGpuContext previous_{};

    public:
        SceneGpuContextScope(BindlessArray &bindless_array, Device &device) noexcept;
        SceneGpuContextScope(const SceneGpuContextScope &) = delete;
        SceneGpuContextScope &operator=(const SceneGpuContextScope &) = delete;
        ~SceneGpuContextScope();
    };

    OC_MAKE_INSTANCE_FUNC_DECL(Global)
    void set_pipeline(SP<Pipeline> pipeline);
    [[nodiscard]] SP<Pipeline> pipeline_shared();
    [[nodiscard]] Pipeline *pipeline();
    [[nodiscard]] ImagePool &image_pool() {
        return ImagePool::instance();
    }
    [[nodiscard]] Device &device() noexcept { return *device_; }
    void set_device(Device *val) noexcept { device_ = val; }
    [[nodiscard]] BindlessArray &bindless_array();
    [[nodiscard]] BindlessArray *scene_bindless_array() noexcept {
        return scene_gpu_context_.bindless_array;
    }
    [[nodiscard]] Device *scene_device() noexcept {
        return scene_gpu_context_.device;
    }
    [[nodiscard]] SceneGpuContext push_scene_gpu_context(BindlessArray &bindless_array,
                                                        Device &device) noexcept;
    void restore_scene_gpu_context(SceneGpuContext context) noexcept;
    void set_scene_path(const fs::path &sp) noexcept;
    [[nodiscard]] fs::path scene_path() const noexcept;
    [[nodiscard]] static decltype(auto) context() {
        return RHIContext::instance();
    }
};

class FrameBuffer;

class Spectrum;
template<typename impl_t, typename desc_t>
class TObject;
using TSpectrum = TObject<Spectrum, SpectrumDesc>;
class Scene;
class Renderer;
class Geometry;
class GeometryData;

class Toolkit {
protected:
    Toolkit() = default;

public:
    [[nodiscard]] static Device &device() noexcept;
    [[nodiscard]] static Stream &stream() noexcept;
    [[nodiscard]] static Pipeline *pipeline() noexcept;
    [[nodiscard]] static Scene &scene() noexcept;
    [[nodiscard]] static Renderer &renderer() noexcept;
    [[nodiscard]] static FrameBuffer &frame_buffer() noexcept;
    [[nodiscard]] static TSpectrum &spectrum() noexcept;
    [[nodiscard]] static Geometry &geometry() noexcept;
    [[nodiscard]] static GeometryData *geometry_data() noexcept;
};

}// namespace vision
