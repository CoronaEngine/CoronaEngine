#include "rtxpt_gi_lights.h"
#include "rtxpt_gi_lighting_dsl.h"
#include "indirect.h"
#include "dsl/data/shared_array.h"
#include <bit>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace vision::rtxpt_gi {
using namespace ocarina;
namespace {
constexpr uint cube_dim = 2048u;
constexpr uint cube_mip_count = 3u; // Native diffuse-miss offset is two.
constexpr uint env_dim = 1024u;
constexpr uint env_nodes = 4096u;
constexpr uint native_max_lights = 1024u * 1024u - 2u;
// Matched LightsBaker.hlsl fixed execution constants.
constexpr uint light_build_threads = 128u;
constexpr uint light_power_block = 32u;
constexpr uint proxies_per_task = 32u;

template<typename T>
Buffer<T> upload(const vector<T> &values, const char *name) {
    auto result = Toolkit::device().create_buffer<T>(std::max<size_t>(1u, values.size()), name);
    if (!values.empty()) result.upload_immediately(values.data());
    return result;
}

uint mip_offset(uint dim, uint mip, uint faces = 1u) noexcept {
    uint offset = 0u;
    for (uint i = 0u; i < mip; ++i) { offset += dim * dim * faces; dim >>= 1u; }
    return offset;
}

Float3 oct_decode(const Float2 &uv) noexcept {
    return lighting_dsl::oct_decode(uv);
}
Float2 oct_encode(const Float3 &dir) noexcept {
    return lighting_dsl::oct_encode(dir);
}
Float3 cube_direction(const Uint &face, const Float2 &uv) noexcept {
    return lighting_dsl::cube_direction(face, uv);
}
Float3 cube_coordinate(const Float3 &dir) noexcept {
    return lighting_dsl::cube_coordinate(dir);
}
Float4 decode_half(const Uint2 &bits) noexcept {
    return lighting_dsl::decode_half(bits);
}
Uint2 encode_half(const Float4 &value) noexcept {
    return lighting_dsl::encode_half(value);
}
Float4 half_round(const Float4 &value) noexcept {
    return lighting_dsl::half_round(value);
}

Float3 cube_sample(const Buffer<uint2> &cube, const Float3 &dir, uint mip) noexcept {
    const uint dim = cube_dim >> mip;
    const uint base = mip_offset(cube_dim, mip, 6u);
    const Float3 coordinate = cube_coordinate(dir);
    const Uint face = cast<uint>(coordinate.z);
    const Float2 xy = coordinate.xy() * float(dim) - .5f;
    const Int2 lower = cast<int2>(floor(xy));
    const Float2 t = xy - floor(xy);
    const auto tap = [&](const Int2 &pixel) {
        // Seam taps are remapped to the neighboring cube face in direction space.
        const Float3 sample_dir = cube_direction(face, (make_float2(pixel) + .5f) / float(dim));
        const Float3 sample_coord = cube_coordinate(sample_dir);
        const Uint sample_face = cast<uint>(sample_coord.z);
        const Int2 sample_xy = clamp(cast<int2>(floor(sample_coord.xy() * float(dim))),
                                     make_int2(0), make_int2(int(dim) - 1));
        const Uint index = base + sample_face * dim * dim + cast<uint>(sample_xy.y) * dim + cast<uint>(sample_xy.x);
        return decode_half(cube.read(index, false)).xyz();
    };
    const Float3 v00 = tap(lower), v10 = tap(lower + make_int2(1, 0));
    const Float3 v01 = tap(lower + make_int2(0, 1)), v11 = tap(lower + make_int2(1, 1));
    return lerp(t.y, lerp(t.x, v00, v10), lerp(t.x, v01, v11));
}

struct EnvironmentNode { uint x{}, y{}, dim{}; };

uint packed_node_weight(const vector<float4> &mips, EnvironmentNode node,
                        uint index, uint depth_limit) noexcept {
    if (node.dim >= env_dim) return index;
    const uint level = 10u - uint(std::countr_zero(node.dim));
    const uint offset = mip_offset(env_dim, level);
    const float area_mul = float(1u << (level * 2u));
    float weight = area_mul * mips[offset + node.y * node.dim + node.x].w;
    weight = std::max(.0001f * float(level), weight);
    weight *= float(level > depth_limit);
    // Matched Utils.hlsli::FastSqrt, including its approximation before the
    // integer priority packing. Replacing this by sqrt changes split ordering.
    const float fast_sqrt = std::bit_cast<float>(0x1fbd1df5u + (std::bit_cast<uint>(weight) >> 1u));
    const uint quantized = std::min(uint(fast_sqrt * 100.f + .5f), 0x000fffffu);
    return (quantized << 12u) | index;
}

void subdivide(vector<EnvironmentNode> &nodes, const vector<float4> &mips,
               uint count, uint depth_limit) {
    for (uint split = 0u; split < count; ++split) {
        uint maximum = 0u;
        for (uint i = 0u; i < nodes.size(); ++i)
            maximum = std::max(maximum, packed_node_weight(mips, nodes[i], i, depth_limit));
        const uint chosen = maximum & 4095u;
        auto parent = nodes[chosen];
        if (parent.dim >= env_dim)
            throw std::runtime_error("RTXPT GI environment quadtree attempted to subdivide a finest texel");
        for (uint i = 0u; i < 4u; ++i) {
            const EnvironmentNode child{parent.x * 2u + i % 2u, parent.y * 2u + i / 2u, parent.dim * 2u};
            if (i == 0u) nodes[chosen] = child;
            else nodes.push_back(child);
        }
    }
}
}// namespace

void NativeLightSampler::prepare() {
    if (spectrum()->impl_type() != "srgb")
        throw std::runtime_error("RTXPT GI native lighting currently requires the RGB spectrum");
    if (const char *value = std::getenv("VISION_RTXPT_GI_LIGHT_FEEDBACK"))
        feedback_enabled_ = std::atoi(value) != 0;
    extent_ = pipeline()->resolution();
    has_environment_ = scene().light_manager().env_light() != nullptr;
    vector<uint4> primitives;
    vector<uint> bases(scene().instances().size(), InvalidUI32);
    for (uint i = 0u; i < scene().instances().size(); ++i) {
        const auto &shape = scene().instances()[i];
        if (!shape->has_emission()) continue;
        const auto *light = shape->emission().get();
        if (light->impl_type() != "area" || light->source_desc()["two_sided"].as_bool(false))
            throw std::runtime_error("RTXPT GI native light table requires one-sided triangle emitters");
        bases[i] = env_nodes + uint(primitives.size());
        for (uint p = 0u; p < shape->mesh()->triangles().size(); ++p)
            primitives.push_back(make_uint4(i, p, shape->handle().light_id, 0u));
    }
    for (const auto &light : scene().light_manager().lights())
        if (!light->match(LightType::Area) && !light->match(LightType::Infinite))
            throw std::runtime_error("RTXPT GI native light table does not yet support analytic lights");
    triangle_count_ = uint(primitives.size());
    light_count_ = env_nodes + triangle_count_;
    if (light_count_ >= native_max_lights)
        throw std::runtime_error("RTXPT GI native light count exceeds the matched native light table limit");
    primitive_ids_ = upload(primitives, "RTXPT GI native emitter primitive IDs");
    instance_bases_ = upload(bases, "RTXPT GI native instance light bases");
    lights_ = device().create_buffer<uint4>(size_t(light_count_) * 2u, "RTXPT GI native PolymorphicLightInfo32");
    environment_nodes_ = device().create_buffer<uint4>(env_nodes + env_dim * env_dim, "RTXPT GI native environment quads and direction lookup");
    const uint env_texels = mip_offset(env_dim, 11u);
    environment_mips_ = device().create_buffer<float4>(env_texels, "RTXPT GI native equal-area radiance/importance mips");
    environment_cube_ = device().create_buffer<uint2>(mip_offset(cube_dim, cube_mip_count, 6u),
                                                     "RTXPT GI native 2048 cube half4 mip0-2");
    const uint proxy_capacity = 8u * std::max(light_count_, native_max_lights / 20u) + light_count_;
    proxy_indices_ = device().create_buffer<uint>(proxy_capacity, "RTXPT GI native global proxy indices");
    proxy_counts_ = device().create_buffer<uint>(light_count_, "RTXPT GI native global proxy counts");
    proxy_offsets_ = device().create_buffer<uint>(light_count_, "RTXPT GI native global proxy offsets");
    weights_ = device().create_buffer<float>(light_count_, "RTXPT GI native global light power");
    const uint weight_groups = (light_count_ + light_build_threads * light_power_block - 1u) /
                               (light_build_threads * light_power_block);
    const uint proxy_groups = (light_count_ + light_build_threads - 1u) / light_build_threads;
    weight_block_sums_ = device().create_buffer<float>(weight_groups, "RTXPT GI native light power block sums");
    proxy_group_sums_ = device().create_buffer<uint>(proxy_groups, "RTXPT GI native proxy group sums");
    proxy_group_bases_ = device().create_buffer<uint>(proxy_groups, "RTXPT GI native proxy group bases");
    const uint task_capacity = light_count_ + (proxy_capacity + proxies_per_task - 1u) / proxies_per_task;
    proxy_tasks_ = device().create_buffer<uint4>(task_capacity, "RTXPT GI native 32-proxy build tasks");
    control_ = upload(vector<uint>(8u, 0u), "RTXPT GI native global control");
    feedback_counts_ = device().create_buffer<uint>(light_count_ + 1u, "RTXPT GI native global feedback counts");
    update_resolution(extent_);
    prepared_ = true;
    initialized_ = false;
    OC_INFO_FORMAT("RTXPT GI native lights: {} env quad slots, {} emitter triangles; NEE 1/1 global proxies; feedback {}", env_nodes, triangle_count_, feedback_enabled_ ? "ON" : "OFF");
}

void NativeLightSampler::update_resolution(uint2 resolution) noexcept {
    extent_ = resolution;
    feedback_.super() = device().create_buffer<uint2>(size_t(resolution.x) * resolution.y * 2u,
                                                      "RTXPT GI native direct and indirect feedback reservoirs");
    feedback_.register_self();
    vector<uint2> empty(size_t(resolution.x) * resolution.y * 2u, make_uint2(InvalidUI32, 0u));
    feedback_.upload_immediately(empty.data());
    if (prepared_ || light_count_ != 0u) {
        vector<uint> control(8u, 0u);
        control[2] = resolution.x; control[3] = resolution.y;
        control[7] = uint(feedback_enabled_);
        control_.upload_immediately(control.data());
    }
    previous_frame_ = InvalidUI32;
}

void NativeLightSampler::compile_environment() {
    auto &sampler = renderer().sampler();
    const auto *environment = scene().light_manager().env_light();
    Kernel cube = [&] {
        const Uint index = dispatch_id();
        constexpr uint dim = cube_dim / 2u;
        $if(index >= dim * dim * 6u) { $return(); };
        const Uint face = index / (dim * dim);
        const Uint2 xy = make_uint2(index % dim, (index / dim) % dim) * 2u;
        Float4 total = make_float4(0.f);
        Float sum = 0.f;
        // Native BaseLayerCS generates four full-precision samples, stores
        // mip0 as half, and computes mip1 before that storage quantization.
        for (uint x = 0u; x < 2u; ++x) for (uint y = 0u; y < 2u; ++y) {
            const Uint2 pixel = xy + make_uint2(x, y);
            Float3 radiance = make_float3(0.f);
            if (environment) {
                const Float3 direction = cube_direction(face, (make_float2(pixel) + .5f) / float(cube_dim));
                const auto swl = spectrum()->sample_wavelength(sampler);
                const LightSampleContext receiver{make_float3(0.f), make_float3(0.f, 0.f, 1.f), make_float3(0.f, 0.f, 1.f)};
                const LightEvalContext point{direction, -direction, make_float2(0.f), 0.f};
                radiance = environment->evaluate_wi(receiver, point, swl, LightEvalMode::L).L.vec3();
            }
            const Float4 value = make_float4(clamp(radiance, 0.f, 65504.f), 1.f);
            const Float weight = lighting_dsl::cube_area(Float(float(cube_dim)), pixel);
            environment_cube_.write(face * cube_dim * cube_dim + pixel.y * cube_dim + pixel.x, encode_half(value), false);
            total += value * weight;
            sum += weight;
        }
        environment_cube_.write(mip_offset(cube_dim, 1u, 6u) + index, encode_half(total / sum), false);
    };
    cube.function()->set_block_dim(64u);
    bake_cube_ = device().compile(cube, "RTXPT GI native environment cube bake");
    cube_reduce_.clear();
    for (uint mip = 2u; mip < cube_mip_count; ++mip) {
        const uint dim = cube_dim >> mip, src_dim = dim * 2u;
        const uint dst_base = mip_offset(cube_dim, mip, 6u), src_base = mip_offset(cube_dim, mip - 1u, 6u);
        Kernel reduce = [&] {
            const Uint index = dispatch_id();
            $if(index >= dim * dim * 6u) { $return(); };
            const Uint face = index / (dim * dim);
            const Uint2 xy = make_uint2(index % dim, (index / dim) % dim);
            Float4 total = make_float4(0.f);
            Float sum = 0.f;
            for (uint x = 0u; x < 2u; ++x) for (uint y = 0u; y < 2u; ++y) {
                const Uint2 src = xy * 2u + make_uint2(x, y);
                const Float weight = lighting_dsl::cube_area(Float(float(src_dim)), src);
                const Float4 value = decode_half(environment_cube_.read(src_base + face * src_dim * src_dim + src.y * src_dim + src.x, false));
                total += value * weight;
                sum += weight;
            }
            environment_cube_.write(dst_base + index, encode_half(total / sum), false);
        };
        reduce.function()->set_block_dim(64u);
        cube_reduce_.push_back(device().compile(reduce, "RTXPT GI native environment cube mip"));
    }
    Kernel bake = [&] {
        const Uint index = dispatch_id();
        $if(index >= env_dim * env_dim) { $return(); };
        const Uint2 xy = make_uint2(index % env_dim, index / env_dim);
        Float3 radiance = make_float3(0.f);
        Float importance = 0.f;
        for (uint y = 0u; y < 4u; ++y) for (uint x = 0u; x < 4u; ++x) {
            const Float2 uv = (make_float2(xy * 4u + make_uint2(x, y)) + .5f) / float(env_dim * 4u);
            const Float3 value = cube_sample(environment_cube_, oct_decode(uv), 0u);
            radiance += value;
            importance += (dot(value, make_float3(.2126f, .7152f, .0722f)) + (value.x + value.y + value.z) / 3.f) * .5f;
        }
        const Float4 rounded = half_round(make_float4(radiance / 16.f, importance / 16.f));
        environment_mips_.write(index, rounded, false);
    };
    bake.function()->set_block_dim(64u);
    bake_environment_ = device().compile(bake, "RTXPT GI native equal-area importance bake");
    environment_reduce_.clear();
    for (uint mip = 1u; mip <= 10u; ++mip) {
        const uint dim = env_dim >> mip, src_dim = dim * 2u;
        const uint dst_base = mip_offset(env_dim, mip), src_base = mip_offset(env_dim, mip - 1u);
        Kernel reduce = [&] {
            const Uint index = dispatch_id();
            $if(index >= dim * dim) { $return(); };
            const Uint2 xy = make_uint2(index % dim, index / dim) * 2u;
            const Float4 a = environment_mips_.read(src_base + xy.y * src_dim + xy.x, false);
            const Float4 b = environment_mips_.read(src_base + xy.y * src_dim + xy.x + 1u, false);
            const Float4 c = environment_mips_.read(src_base + (xy.y + 1u) * src_dim + xy.x, false);
            const Float4 d = environment_mips_.read(src_base + (xy.y + 1u) * src_dim + xy.x + 1u, false);
            const Float4 average = (a + b + c + d) * .25f;
            environment_mips_.write(dst_base + index, half_round(average), false);
        };
        reduce.function()->set_block_dim(64u);
        environment_reduce_.push_back(device().compile(reduce, "RTXPT GI native equal-area importance mip"));
    }
}

void NativeLightSampler::build_environment_nodes() {
    vector<float4> mips(mip_offset(env_dim, 11u));
    pipeline()->stream() << environment_mips_.download(mips.data()) << synchronize() << commit();
    vector<EnvironmentNode> seeds;
    for (uint i = 0u; i < 16u; ++i) seeds.push_back({i / 4u, i % 4u, 4u});
    vector<uint4> nodes;
    if (mips.back().w > 0.f) {
        subdivide(seeds, mips, 16u, 3u);
        for (const auto &seed : seeds) {
            vector<EnvironmentNode> group{seed};
            subdivide(group, mips, 21u, 0u);
            for (const auto &node : group) nodes.push_back(make_uint4(node.x, node.y, node.dim, 0u));
        }
    } else {
        for (uint i = 0u; i < env_nodes; ++i) nodes.push_back(make_uint4(i % 64u, i / 64u, 64u, 0u));
    }
    nodes.resize(env_nodes + env_dim * env_dim);
    for (uint light = 0u; light < env_nodes; ++light) {
        const auto node = nodes[light];
        const uint scale = env_dim / node.z;
        for (uint y = node.y * scale; y < (node.y + 1u) * scale; ++y)
            for (uint x = node.x * scale; x < (node.x + 1u) * scale; ++x)
                nodes[env_nodes + y * env_dim + x] = make_uint4(light, 0u, 0u, 0u);
    }
    environment_nodes_.upload_immediately(nodes.data());
}

void NativeLightSampler::compile_triangles() {
    auto &sampler = renderer().sampler();
    Kernel bake = [&] {
        const Uint index = dispatch_id();
        $if(index >= triangle_count_) { $return(); };
        const Uint4 primitive = primitive_ids_.read(index, false);
        const auto a = geometry().compute_light_eval_context(primitive.x, primitive.y, make_float2(1.f, 0.f));
        const auto b = geometry().compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f, 1.f));
        const auto c = geometry().compute_light_eval_context(primitive.x, primitive.y, make_float2(0.f));
        const auto center = geometry().compute_light_eval_context(primitive.x, primitive.y, make_float2(1.f / 3.f));
        const auto swl = spectrum()->sample_wavelength(sampler);
        Float3 radiance = make_float3(0.f);
        const LightSampleContext receiver{center.pos + center.ng, center.ng, center.ng};
        renderer().light_sampler()->dispatch_light(primitive.z, [&](const Light *light) {
            if (light->match(LightType::Area))
                radiance = light->evaluate_wi(receiver, center, swl, LightEvalMode::L).L.vec3();
        });
        radiance = max(radiance, make_float3(0.f));
        $if(max(max(radiance.x, radiance.y), radiance.z) < 1e-7f) { radiance = make_float3(0.f); };
        // Native only compensates negative instance transform determinants.
        // Corona's emissive ng can be flipped toward authored vertex normals,
        // so it cannot determine the native triangle light winding.
        const auto instance = geometry().data()->instances().read(primitive.x);
        Transform transform{instance->o2w()};
        const Float3 tx = transform.apply_vector(make_float3(1.f, 0.f, 0.f));
        const Float3 ty = transform.apply_vector(make_float3(0.f, 1.f, 0.f));
        const Float3 tz = transform.apply_vector(make_float3(0.f, 0.f, 1.f));
        const Bool mirrored = dot(cross(tx, ty), tz) < 0.f;
        Float3 b_pos = b.pos, c_pos = c.pos;
        $if(mirrored) { b_pos = c.pos; c_pos = b.pos; };
        const auto packed = lighting_dsl::pack_triangle(a.pos, b_pos, c_pos, radiance);
        lights_.write((env_nodes + index) * 2u, packed.lo, false);
        lights_.write((env_nodes + index) * 2u + 1u, packed.hi, false);
    };
    bake.function()->set_block_dim(32u);
    bake_triangles_ = device().compile(bake, "RTXPT GI native triangle light bake");
}

void NativeLightSampler::compile_environment_lights() {
    Kernel bake = [&] {
        const Uint index = dispatch_id();
        $if(index >= env_nodes) { $return(); };
        const Uint4 node = environment_nodes_.read(index, false);
        Uint level = 0u, dim = node.z;
        $while(dim < env_dim) { dim *= 2u; level += 1u; };
        Uint offset = 0u, mip_dim = env_dim;
        $for(i, level) { offset += mip_dim * mip_dim; mip_dim >>= 1u; };
        const Float4 radiance = environment_mips_.read(offset + node.y * node.z + node.x, false);
        const Float weight = cast<float>(1u << (level * 2u)) * max(0.f, radiance.w * .0002f);
        const Uint3 native_node = node.xyz();
        const Float3 native_radiance = radiance.xyz();
        const auto packed = lighting_dsl::pack_environment(native_node, native_radiance, weight);
        lights_.write(index * 2u, packed.lo, false);
        lights_.write(index * 2u + 1u, packed.hi, false);
    };
    bake.function()->set_block_dim(64u);
    bake_environment_lights_ = device().compile(bake, "RTXPT GI native environment quad bake");
}

void NativeLightSampler::compile_proxies() {
    const uint weight_groups = (light_count_ + light_build_threads * light_power_block - 1u) /
                               (light_build_threads * light_power_block);
    const uint proxy_groups = (light_count_ + light_build_threads - 1u) / light_build_threads;
    // Native ComputeWeights: 32 consecutive lights per lane, then one 128-lane
    // group sum. No padded lane returns before the group barrier.
    Kernel power = [&] {
        const Uint thread = dispatch_id(), lane = thread % light_build_threads;
        const Uint from = thread * light_power_block;
        const Uint to = min(from + light_power_block, light_count_);
        Float local_sum = 0.f;
        $for(light, from, to) {
            const Float power = lighting_dsl::power(lights_.read(light * 2u, false), lights_.read(light * 2u + 1u, false));
            const Float weight = select(power < 1e-7f, 0.f, power);
            weights_.write(light, weight, false);
            local_sum += weight;
        };
        SharedArray<float, light_build_threads> sums;
        sums.write(lane, local_sum);
        synchronize_block();
        $if(lane == 0u) {
            Float total = 0.f;
            $for(i, light_build_threads) { total += sums.read(i); };
            weight_block_sums_.write(thread / light_build_threads, total, false);
        };
    };
    power.function()->set_block_dim(light_build_threads);
    compute_weights_ = device().compile(power, "RTXPT GI native parallel light powers");

    // Native's inter-group CAS additions may arrive in any order. A fixed
    // group order is one valid native order and makes this adapter repeatable.
    // This folds only ceil(N/4096) floats (7 for the Kitchen), not N lights.
    Kernel total = [&] {
        Float sum = 0.f;
        $for(group, weight_groups) { sum += weight_block_sums_.read(group, false); };
        control_.write(1u, as<uint>(sum), false);
        control_.write(6u, 0u, false); // Native ProxyBuildTaskCount.
    };
    total.function()->set_block_dim(1u);
    reduce_weight_sum_ = device().compile(total, "RTXPT GI native light power total");

    Kernel counts = [&] {
        const Uint light = dispatch_id(), lane = light % light_build_threads;
        const Uint group = light / light_build_threads;
        const Float sum = as<float>(control_.read(1u, false));
        Uint valid_feedback = 0u;
        Float feedback_ratio = 0.f;
        if (feedback_enabled_) {
            const Uint total_feedback = control_.read(2u, false) * control_.read(3u, false) * 2u;
            const Uint invalid_feedback = feedback_counts_.read(light_count_, false);
            const Bool available = control_.read(4u, false) != 0u;
            valid_feedback = select(available, total_feedback - invalid_feedback, 0u);
            feedback_ratio = select(available, .65f, 0.f);
        }
        Uint count = 0u;
        $if(light < light_count_) {
            Float weight = weights_.read(light, false);
            if (feedback_enabled_) {
                weight = lighting_dsl::feedback_blend(weight, sum, feedback_counts_.read(light, false),
                                                       valid_feedback, feedback_ratio);
            }
            const uint budget = 8u * std::max(light_count_, native_max_lights / 20u);
            count = lighting_dsl::proxy_count(weight, sum, Uint{budget}, Uint{light_count_});
            proxy_counts_.write(light, count, false);
        };
        SharedArray<uint, light_build_threads> group_counts;
        group_counts.write(lane, count);
        synchronize_block();
        $if(lane == 0u) {
            Uint running = 0u;
            const Uint begin = group * light_build_threads;
            const Uint size = min(Uint{light_build_threads}, Uint{light_count_} - begin);
            $for(i, size) {
                proxy_offsets_.write(begin + i, running, false);
                running += group_counts.read(i);
            };
            proxy_group_sums_.write(group, running, false);
            $if(group == 0u) { control_.write(5u, valid_feedback, false); };
        };
    };
    counts.function()->set_block_dim(light_build_threads);
    build_proxies_ = device().compile(counts, "RTXPT GI native parallel proxy counts");

    // A stable exclusive scan of integer group totals. Each lane owns a
    // consecutive group range; the shared scan preserves ascending light IDs.
    Kernel prefix = [&] {
        const Uint lane = dispatch_id();
        constexpr uint scan_lanes = light_build_threads;
        const uint span = (proxy_groups + scan_lanes - 1u) / scan_lanes;
        const Uint begin = lane * span, end = min(begin + span, proxy_groups);
        Uint local = 0u;
        $for(group, begin, end) { local += proxy_group_sums_.read(group, false); };
        SharedArray<uint, scan_lanes> lane_bases;
        lane_bases.write(lane, local);
        synchronize_block();
        $if(lane == 0u) {
            Uint running = 0u;
            $for(i, scan_lanes) {
                const Uint count = lane_bases.read(i);
                lane_bases.write(i, running);
                running += count;
            };
            control_.write(0u, running, false);
        };
        synchronize_block();
        Uint running = lane_bases.read(lane);
        $for(group, begin, end) {
            proxy_group_bases_.write(group, running, false);
            running += proxy_group_sums_.read(group, false);
        };
    };
    prefix.function()->set_block_dim(light_build_threads);
    prefix_proxies_ = device().compile(prefix, "RTXPT GI native stable proxy prefix");

    // Native CreateProxyJobs partitions every light into at most 32 stores per
    // task. Job order may vary; their final proxy indices remain stable.
    Kernel create = [&] {
        const Uint light = dispatch_id();
        $if(light >= light_count_) { $return(); };
        const Uint begin = proxy_offsets_.read(light, false) +
                           proxy_group_bases_.read(light / light_build_threads, false);
        proxy_offsets_.write(light, begin, false);
        const Uint count = proxy_counts_.read(light, false);
        const Uint tasks = (count + proxies_per_task - 1u) / proxies_per_task;
        const Uint task_base = control_.atomic(Uint{6u}).fetch_add(tasks);
        $for(i, tasks) {
            const Uint from = begin + i * proxies_per_task;
            const Uint to = begin + min((i + 1u) * proxies_per_task, count);
            proxy_tasks_.write(task_base + i, make_uint4(light, begin, from, to), false);
        };
    };
    create.function()->set_block_dim(light_build_threads);
    create_proxy_jobs_ = device().compile(create, "RTXPT GI native create proxy jobs");

    Kernel fill = [&] {
        const Uint index = dispatch_id();
        $if(index >= control_.read(6u, false)) { $return(); };
        const Uint4 task = proxy_tasks_.read(index, false);
        $for(proxy, Uint{task.z}, Uint{task.w}) { proxy_indices_.write(proxy, Uint{task.x}, false); };
    };
    fill.function()->set_block_dim(light_build_threads);
    fill_proxies_ = device().compile(fill, "RTXPT GI native execute proxy jobs");
}

void NativeLightSampler::compile_feedback() {
    Kernel reset = [&] {
        const Uint index = dispatch_id();
        $if(index <= light_count_) { feedback_counts_.write(index, 0u, false); };
    };
    reset.function()->set_block_dim(64u);
    reset_feedback_counts_ = device().compile(reset, "RTXPT GI native reset global feedback counts");
    Kernel process = [&](Uint2 resolution, Uint update) {
        const Uint2 pixel = dispatch_idx().xy();
        $if(pixel.x >= resolution.x || pixel.y >= resolution.y * 2u) { $return(); };
        const Bool indirect = pixel.y >= resolution.y;
        const Int2 local = make_int2(cast<int>(pixel.x), cast<int>(pixel.y % resolution.y));
        const Uint layer_offset = select(indirect, resolution.x * resolution.y, 0u);
        const auto read = [&](const Int2 &coord) {
            Uint2 result = make_uint2(InvalidUI32, 0u);
            $if(all(coord >= make_int2(0)) && all(coord < make_int2(resolution))) {
                const Uint index = layer_offset + cast<uint>(coord.y) * resolution.x + cast<uint>(coord.x);
                result = feedback_.read(index);
            };
            return result;
        };
        const Uint2 center = read(local), left = read(local + make_int2(-1, 0));
        const Uint2 right = read(local + make_int2(1, 0)), up = read(local + make_int2(0, -1));
        const Uint2 down = read(local + make_int2(0, 1));
        const Uint2 native_pixel = cast<uint2>(local);
        const Uint light = lighting_dsl::feedback_flood(center, left, right, up, down, native_pixel, update);
        // Wave aggregation in native only coalesces these identical integer
        // additions. Scalar atomics produce the same histogram.
        const Uint target = min(light, light_count_);
        (void)feedback_counts_.atomic(target).fetch_add(1u);
    };
    process.function()->set_block_dim(16u, 16u);
    process_feedback_ = device().compile(process, "RTXPT GI native flood global feedback");
    Kernel clear = [&](Uint2 resolution, Uint unused) {
        const Uint2 pixel = dispatch_idx().xy();
        $if(pixel.x < resolution.x && pixel.y < resolution.y * 2u) {
            feedback_.write(pixel.y * resolution.x + pixel.x, make_uint2(InvalidUI32, 0u));
        };
    };
    clear.function()->set_block_dim(16u, 16u);
    clear_feedback_ = device().compile(clear, "RTXPT GI native clear light feedback");
    Kernel state = [&](Uint history) { control_.write(4u, history, false); };
    state.function()->set_block_dim(1u);
    update_feedback_state_ = device().compile(state, "RTXPT GI native light feedback state");
}

void NativeLightSampler::compile() {
    if (!prepared_) prepare();
    compile_environment();
    compile_triangles();
    compile_environment_lights();
    compile_proxies();
    if (feedback_enabled_) compile_feedback();
    // Immutable environment preparation is performed once per compiled scene.
    // Runtime triangle/proxy preparation is returned by dispatch and timed.
    pipeline()->stream() << bake_cube_().dispatch((cube_dim / 2u) * (cube_dim / 2u) * 6u);
    for (uint mip = 2u; mip < cube_mip_count; ++mip)
        pipeline()->stream() << cube_reduce_[mip - 2u]().dispatch((cube_dim >> mip) * (cube_dim >> mip) * 6u);
    pipeline()->stream() << bake_environment_().dispatch(env_dim * env_dim);
    for (uint mip = 1u; mip <= 10u; ++mip)
        pipeline()->stream() << environment_reduce_[mip - 1u]().dispatch((env_dim >> mip) * (env_dim >> mip));
    pipeline()->stream() << synchronize() << commit();
    build_environment_nodes();
    pipeline()->stream() << bake_environment_lights_().dispatch(env_nodes) << synchronize() << commit();
    initialized_ = true;
}

CommandBatch NativeLightSampler::dispatch(uint frame) {
    OC_ASSERT(initialized_);
    CommandBatch batch;
    const bool history = frame != 0u && previous_frame_ != InvalidUI32 && previous_frame_ + 1u == frame;
    if (feedback_enabled_) {
        batch << update_feedback_state_(uint(history)).dispatch(1u);
        batch << reset_feedback_counts_().dispatch(light_count_ + 1u);
        if (history) batch << process_feedback_(extent_, frame + 1u).dispatch(make_uint2(extent_.x, extent_.y * 2u));
    }
    if (triangle_count_ != 0u) batch << bake_triangles_().dispatch(triangle_count_);
    const uint weight_groups = (light_count_ + light_build_threads * light_power_block - 1u) /
                               (light_build_threads * light_power_block);
    const uint proxy_groups = (light_count_ + light_build_threads - 1u) / light_build_threads;
    batch << compute_weights_().dispatch(weight_groups * light_build_threads);
    batch << reduce_weight_sum_().dispatch(1u);
    batch << build_proxies_().dispatch(proxy_groups * light_build_threads);
    batch << prefix_proxies_().dispatch(light_build_threads);
    batch << create_proxy_jobs_().dispatch(light_count_);
    batch << fill_proxies_().dispatch(uint(proxy_tasks_.size()));
    if (feedback_enabled_) batch << clear_feedback_(extent_, 0u).dispatch(make_uint2(extent_.x, extent_.y * 2u));
    previous_frame_ = frame;
    return batch;
}

NativeLightSample NativeLightSampler::sample(const Float3 &receiver, const Float &u_select,
                                            const Float2 &u_shape) const noexcept {
    NativeLightSample result;
    const Uint total = control_.read(0u, false);
    $if(total != 0u) {
        const Uint proxy = min(cast<uint>(u_select * cast<float>(total)), total - 1u);
        result.light_index = proxy_indices_.read(proxy, false);
        result.selection_pdf = cast<float>(proxy_counts_.read(result.light_index, false)) / cast<float>(total);
        const Uint4 lo = lights_.read(result.light_index * 2u, false);
        const Uint4 hi = lights_.read(result.light_index * 2u + 1u, false);
        $if(result.light_index < env_nodes) {
            const Uint dim = hi.y >> 16u;
            const Float2 uv = (make_float2(cast<float>(hi.x >> 16u), cast<float>(hi.x & 65535u)) + u_shape) / cast<float>(dim);
            const Float3 direction = oct_decode(uv);
            result.position = receiver + direction * 100000.f;
            result.normal = -direction;
            result.radiance = lighting_dsl::radiance(lo, hi);
            result.solid_angle_pdf = cast<float>(dim * dim) / (4.f * Pi);
            result.sampleable_by_bsdf = true;
        } $else {
            const auto s = lighting_dsl::sample_triangle(lo, hi, u_shape, receiver);
            result.position = s.position; result.solid_angle_pdf = s.pdf;
            result.normal = s.normal; result.radiance = s.radiance;
            result.sampleable_by_bsdf = s.sampleable;
        };
        const Float3 delta = result.position - receiver;
        result.distance = length(delta); result.direction = delta / result.distance;
        result.valid = result.selection_pdf > 0.f && result.solid_angle_pdf > 0.f;
    };
    return result;
}

Float3 NativeLightSampler::environment_radiance(const Float3 &world_dir, const Float &mip) const noexcept {
    Float3 value = cube_sample(environment_cube_, world_dir, 0u);
    $if(mip >= 2.f) { value = cube_sample(environment_cube_, world_dir, 2u); };
    return value;
}

NativeEmission NativeLightSampler::environment(const Float3 &world_dir, const Uint &diffuse_bounces) const noexcept {
    NativeEmission result;
    if (!has_environment_) return result;
    result.radiance = environment_radiance(world_dir, select(diffuse_bounces > 1u, 2.f, 0.f));
    const Float2 uv = oct_encode(world_dir);
    // The disjoint 4096-node quadtree has the same directional lookup semantics
    // as the native lookup texture; the private map is produced below.
    const Uint2 xy = min(cast<uint2>(uv * float(env_dim)), make_uint2(env_dim - 1u));
    // Environment lookup occupies a dedicated plane after the native nodes.
    const Uint light = environment_nodes_.read(env_nodes + xy.y * env_dim + xy.x, false).x;
    result.light_index = light;
    const Uint total = control_.read(0u, false);
    $if(total != 0u) {
        const Uint4 hi = lights_.read(light * 2u + 1u, false);
        const Uint dim = hi.y >> 16u;
        result.light_pdf = cast<float>(proxy_counts_.read(light, false)) / cast<float>(total) * cast<float>(dim * dim) / (4.f * Pi);
    };
    return result;
}

NativeEmission NativeLightSampler::emissive(const Interaction &hit, const Float3 &receiver) const noexcept {
    NativeEmission result;
    const Uint base = instance_bases_.read(hit.inst_id, false);
    $if(base != InvalidUI32) {
        const Uint light = base + hit.prim_id;
        result.light_index = light;
        const Uint4 lo = lights_.read(light * 2u, false), hi = lights_.read(light * 2u + 1u, false);
        const auto swl = spectrum()->sample_wavelength(renderer().sampler());
        const LightEvalContext point{hit};
        const auto a = geometry().compute_light_eval_context(hit.inst_id, hit.prim_id, make_float2(1.f, 0.f));
        const auto b = geometry().compute_light_eval_context(hit.inst_id, hit.prim_id, make_float2(0.f, 1.f));
        const auto c = geometry().compute_light_eval_context(hit.inst_id, hit.prim_id, make_float2(0.f));
        const auto instance = geometry().data()->instances().read(hit.inst_id);
        Transform transform{instance->o2w()};
        const Float3 tx = transform.apply_vector(make_float3(1.f, 0.f, 0.f));
        const Float3 ty = transform.apply_vector(make_float3(0.f, 1.f, 0.f));
        const Float3 tz = transform.apply_vector(make_float3(0.f, 0.f, 1.f));
        const Float3 native_face = cross(b.pos - a.pos, c.pos - a.pos) * select(dot(cross(tx, ty), tz) < 0.f, -1.f, 1.f);
        const Bool front_facing = dot(native_face, receiver - hit.pos) > 0.f;
        // Extract the authored radiance on its allowed side, then apply native's
        // geometric front-facing rule, independently of Corona's emissive-ng fix.
        const LightSampleContext reference{hit.pos + hit.ng, hit.ng, hit.ng};
        renderer().light_sampler()->dispatch_light(hit.light_id(), [&](const Light *emitter) {
            if (emitter->match(LightType::Area)) {
                const Float3 raw = emitter->evaluate_wi(reference, point, swl, LightEvalMode::L).L.vec3();
                result.radiance = select(front_facing, raw, make_float3(0.f));
            }
        });
        const Uint total = control_.read(0u, false);
        $if(total != 0u) {
            const Float conditional = lighting_dsl::triangle_pdf(lo, hi, receiver, hit.pos);
            result.light_pdf = cast<float>(proxy_counts_.read(light, false)) / cast<float>(total) * conditional;
        };
    };
    return result;
}

Float NativeLightSampler::mis_from_light(const NativeLightSample &light, const Float &bsdf_pdf) const noexcept {
    const Float light_pdf = light.selection_pdf * light.solid_angle_pdf;
    const Float other_pdf = select(light.sampleable_by_bsdf, clamp(bsdf_pdf, 0.f, 65504.f), 0.f);
    return light_pdf / (light_pdf + other_pdf);
}

Float NativeLightSampler::mis_from_bsdf(const Float &mixture_light_pdf, const Float &bsdf_pdf) const noexcept {
    const Float4 rounded = lighting_dsl::half_round(make_float4(clamp(bsdf_pdf, 0.f, 65504.f), 0.f, 0.f, 0.f));
    const Float packed_pdf = rounded.x;
    return select(bsdf_pdf == 0.f, 1.f, packed_pdf / (packed_pdf + mixture_light_pdf));
}

void NativeLightSampler::record_nee(const Uint2 &pixel, const Bool &indirect,
                                   const NativeLightSample &light,
                                   const Float &pixel_contribution_average,
                                   const Float &random) const noexcept {
    if (!feedback_enabled_) return;
    const Uint width = control_.read(2u, false), height = control_.read(3u, false);
    const Uint index = pixel.y * width + pixel.x + select(indirect, width * height, 0u);
    const Float weight = pixel_contribution_average / pow(light.selection_pdf, .65f);
    const Uint2 updated = lighting_dsl::feedback_add(feedback_.read(index), light.light_index, weight, random);
    feedback_.write(index, updated);
}

void NativeLightSampler::record_bsdf(const Uint2 &pixel, const Bool &indirect,
                                    const Uint &light_index,
                                    const Float &pixel_average_without_mis,
                                    const Float &bsdf_mis, const Float &random) const noexcept {
    if (!feedback_enabled_) return;
    const Uint width = control_.read(2u, false), height = control_.read(3u, false);
    const Uint index = pixel.y * width + pixel.x + select(indirect, width * height, 0u);
    // Native's bsdfMISWeight argument is intentionally unused.
    const Float weight = pixel_average_without_mis * .3f;
    const Uint2 updated = lighting_dsl::feedback_add(feedback_.read(index), light_index, weight, random);
    feedback_.write(index, updated);
}

void NativeLightSampler::download_validation(const fs::path &directory) const {
    fs::create_directories(directory);
    vector<uint> control(8u), counts(light_count_);
    vector<uint4> lights(size_t(light_count_) * 2u), nodes(env_nodes);
    vector<float4> coarse_environment(21u);
    struct CubeProbe { uint mip, face, x, y; uint2 bits{}; };
    vector<CubeProbe> probes;
    for (uint mip : {0u, 2u}) for (uint face = 0u; face < 6u; ++face) {
        const uint dim = cube_dim >> mip;
        for (uint2 xy : {make_uint2(dim / 2u), make_uint2(0u), make_uint2(dim - 1u, 0u),
                         make_uint2(0u, dim - 1u), make_uint2(dim - 1u)})
            probes.push_back({mip, face, xy.x, xy.y});
    }
    auto &stream = pipeline()->stream();
    stream << control_.download(control.data()) << proxy_counts_.download(counts.data())
           << lights_.download(lights.data()) << environment_nodes_.view(0u, env_nodes).download(nodes.data())
           << environment_mips_.view(mip_offset(env_dim, 8u), 21u).download(coarse_environment.data());
    for (auto &probe : probes) {
        const uint dim = cube_dim >> probe.mip;
        const uint index = mip_offset(cube_dim, probe.mip, 6u) + probe.face * dim * dim + probe.y * dim + probe.x;
        stream << environment_cube_.view(index, 1u).download(&probe.bits);
    }
    stream << synchronize() << commit();
    vector<uint> proxies(control[0]);
    if (!proxies.empty()) stream << proxy_indices_.view(0u, proxies.size()).download(proxies.data()) << synchronize() << commit();
    const auto binary = [&]<typename T>(const char *name, const vector<T> &values) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char *>(values.data()), std::streamsize(values.size() * sizeof(T)));
    };
    binary("lights32.u32", lights);
    binary("proxy-counts.u32", counts);
    binary("proxy-indices.u32", proxies);
    binary("control.u32", control);
    binary("environment-nodes.u32", nodes);
    binary("environment-coarse-rgba.f32", coarse_environment);
    DataWrap summary;
    summary["native_polymorphic_light_bytes"] = 32u;
    summary["light_count"] = light_count_;
    summary["environment_quad_slots"] = env_nodes;
    summary["triangle_slots"] = triangle_count_;
    summary["sampling_proxy_count"] = control[0];
    summary["power_weight_sum"] = std::bit_cast<float>(control[1]);
    summary["importance_sampling_type"] = feedback_enabled_ ? 2u : 1u;
    summary["temporal_feedback_enabled"] = feedback_enabled_;
    summary["valid_feedback_count"] = control[5];
    uint nonzero_environment = 0u, nonzero_triangles = 0u;
    for (uint i = 0u; i < light_count_; ++i)
        if (counts[i] != 0u) { if (i < env_nodes) ++nonzero_environment; else ++nonzero_triangles; }
    summary["positive_proxy_environment_lights"] = nonzero_environment;
    summary["positive_proxy_triangle_lights"] = nonzero_triangles;
    summary["cube_half4_probe"] = DataWrap::array();
    for (const auto &probe : probes) {
        const auto rg = unpack_storage_half2_host(probe.bits.x), ba = unpack_storage_half2_host(probe.bits.y);
        summary["cube_half4_probe"].push_back({{"mip", probe.mip}, {"face", probe.face}, {"x", probe.x}, {"y", probe.y},
            {"rgba", {rg.x, rg.y, ba.x, ba.y}}, {"packed_u32", {probe.bits.x, probe.bits.y}}});
    }
    summary["remaining_resource_differences"] = {"Manual cube seam filtering", "Triangle emissive texture uses center LOD0 instead of native anisotropic SampleGrad", "Native BC6 cube compression must be disabled in controlled comparison"};
    std::ofstream file(directory / "summary.json");
    file << summary.dump(2);
}
}// namespace vision::rtxpt_gi
