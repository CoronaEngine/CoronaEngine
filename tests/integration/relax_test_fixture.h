#pragma once
#include "render_core/denoiser/ReLAX/filter.h"
#include <cmath>
#include <stdexcept>
#include <vector>

namespace relax_test {
using namespace ocarina;
inline void expect(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
struct Fixture {
    vision::relax::Filter filter;
    Device &device;
    Stream &stream;
    uint2 size;
    uint n;
    Buffer<float4> diffuse, specular, output_d, output_s;
    std::vector<vision::relax::Guide> guides;
    std::vector<float4> d, s, got_d, got_s, prepass_d, prepass_s;
    vision::relax::FilterParam param;

    Fixture(Device &dev, Stream &str, uint2 dimensions, uint layers = 1u)
        : device(dev), stream(str), size(dimensions), n(size.x * size.y),
          diffuse(dev.create_buffer<float4>(n, "RELAX fixture diffuse")),
          specular(dev.create_buffer<float4>(n, "RELAX fixture specular")),
          output_d(dev.create_buffer<float4>(n, "RELAX fixture filtered diffuse")),
          output_s(dev.create_buffer<float4>(n, "RELAX fixture filtered specular")),
          guides(n), d(n, make_float4(.2f, .6f, 1.f, 7.f)), s(n, make_float4(1.f, .3f, .1f, 5.f)),
          got_d(n), got_s(n), prepass_d(n), prepass_s(n) {
        filter.prepare(device, size, layers);
        filter.compile(device);
        for (uint y = 0; y < size.y; ++y) for (uint x = 0; x < size.x; ++x) {
            auto &g = guides[y * size.x + x];
            g.position = make_float4((float(x) + .5f - size.x * .5f) * .05f,
                                    (float(y) + .5f - size.y * .5f) * .05f, 2.f, 2.f);
            g.normal_roughness = make_float4(0.f, 0.f, -1.f, .4f);
            g.diffuse_factor = g.specular_factor = make_float4(1.f);
            g.identity = make_uint4(1u, 2u, 3u, 1u);
        }
        param.raw_diffuse = diffuse.descriptor(); param.raw_specular = specular.descriptor();
        param.output_diffuse = output_d.descriptor(); param.output_specular = output_s.descriptor();
        param.unproject = .025f;
        param.history_valid = 1u;
        param.world_to_raster = make_float4x4(make_float4(20.f, 0.f, 0.f, 0.f), make_float4(0.f, 20.f, 0.f, 0.f),
            make_float4(0.f, 0.f, 1.f, 0.f), make_float4(size.x * .5f, size.y * .5f, 0.f, 1.f));
        param.prev_world_to_raster = param.world_to_raster;
    }
    void run(uint frame, uint layer = 0u) {
        param.frame = frame;
        stream << filter.guides().upload(guides.data()) << diffuse.upload(d.data()) << specular.upload(s.data());
        stream << filter.dispatch(param, layer) << output_d.download(got_d.data()) << output_s.download(got_s.data())
               << filter.prepass_diffuse().download(prepass_d.data()) << filter.prepass_specular().download(prepass_s.data())
               << synchronize() << commit();
    }
};
}

void check_relax_prepass(ocarina::Device &, ocarina::Stream &);
void check_relax_temporal(ocarina::Device &, ocarina::Stream &);
