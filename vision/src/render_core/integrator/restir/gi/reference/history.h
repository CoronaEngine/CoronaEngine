#pragma once

#include "device.h"

namespace vision::restir_gi {

// Native temporal history precision, plus the Corona light replay seed.
// Five images use 48 logical bytes per GI cell. The tracing origin stays FP32.
class HistoryImages {
    Texture2D origins_;
    Texture2D offsets_;
    Texture2D radiance_;
    Texture2D normals_;
    Texture2D metadata_;

public:
    void allocate(Device &device,uint2 extent,const std::string &name) {
        origins_=device.create_texture2d(extent,PixelStorage::FLOAT4,name+" origin");
        offsets_=device.create_texture2d(extent,PixelStorage::HALF4,name+" offset and distance");
        radiance_=device.create_texture2d(extent,PixelStorage::HALF4,name+" radiance");
        normals_=device.create_texture2d(extent,PixelStorage::HALF4,name+" normal and cosine");
        metadata_=device.create_texture2d(extent,PixelStorage::UINT2,name+" seed and reservoir");
    }

    [[nodiscard]] SampleVar read(const Uint2 &pixel) const {
        Float4 origin=origins_.read_native<float4>(pixel);
        Float4 offset=offsets_.read_native<float4>(pixel);
        Float4 radiance=radiance_.read_native<float4>(pixel);
        Float4 normal=normals_.read_native<float4>(pixel);
        Uint2 metadata=metadata_.read_native<uint2>(pixel);
        Float2 mw=unpack_image_half2(metadata.y);
        SampleVar sample;
        sample.hit_distance=make_float4(origin.xyz()+offset.xyz(),offset.w);
        sample.origin_cosine=make_float4(origin.xyz(),normal.w);
        sample.radiance_count=make_float4(radiance.xyz(),mw.x);
        sample.normal_weight=make_float4(normal.xyz()*2.f-1.f,mw.y);
        sample.light_seed=metadata.x;
        return sample;
    }

    void write(const Uint2 &pixel,const SampleVar &sample) const {
        origins_.write_native(pixel,make_float4(sample.origin_cosine.xyz(),0.f));
        offsets_.write_native(pixel,make_float4(sample.hit_distance.xyz()-sample.origin_cosine.xyz(),sample.hit_distance.w));
        radiance_.write_native(pixel,make_float4(sample.radiance_count.xyz(),0.f));
        normals_.write_native(pixel,make_float4(sample.normal_weight.xyz()*.5f+.5f,sample.origin_cosine.w));
        metadata_.write_native(pixel,make_uint2(sample.light_seed,
            pack_image_half2(make_float2(sample.radiance_count.w,sample.normal_weight.w))));
    }
};

} // namespace vision::restir_gi
