#pragma once

// Included inside vision::reblur::real_conversion by the canonical fixture.
// The fixed NRD OUT textures come from the same actual frame64 capture.
inline PassShader compile_common_backend(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Ulong block_bytes = p.external.size.x.cast<ulong>() * p.external.size.y.cast<ulong>() * 16ull;
        Ulong offset = index.cast<ulong>() * 16ull;
        Float view_z = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, offset).w;
        $if(as<uint>(view_z) == 0x7f7fffffu) {
            p.external.output_diffuse.write(index, make_float4(0.f));
            p.external.output_specular.write(index, make_float4(0.f));
            $return();
        };
        Float roughness = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, block_bytes + offset).w;
        Float3 diffuse_factor = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, block_bytes * 2ull + offset).xyz();
        Float3 specular_factor = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, block_bytes * 3ull + offset).xyz();
        Float4 diffuse = Image<float4, Format::RGBA16Float>{p.inputs.t0}.read_in_bounds(make_int2(pixel));
        Float4 specular = Image<float4, Format::RGBA16Float>{p.inputs.t1}.read_in_bounds(make_int2(pixel));
        p.external.output_diffuse.write(index, make_float4(UnpackRadiance(diffuse) * diffuse_factor,
            diffuse.w * HitDistanceNormalization(view_z, p.constants.gHitDistParams, 1.f)));
        p.external.output_specular.write(index, make_float4(UnpackRadiance(specular) * specular_factor,
            specular.w * HitDistanceNormalization(view_z, p.constants.gHitDistParams, roughness)));
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Unpack");
}
