//
// Created by Zero on 10/09/2022.
//

#pragma once

#include <utility>
#include "core/stl.h"
#include "core/util/hash.h"
#include "math/basic_types.h"
#include "parameter_set.h"
#include "math/geometry.h"
#include "core/runtime/dynamic_module.h"
#include "base/using.h"

namespace vision {

class Scene;

struct NameID {
public:
    using map_ty = map<string, uint>;

public:
    string name;
    uint id{InvalidUI32};
    bool valid() { return id != InvalidUI32; }
    void fill_id(const map_ty &name_to_id) {
        if (name_to_id.contains(name)) {
            id = name_to_id.at(name);
        } else {
            id = InvalidUI32;
        }
    }
};

enum AttrTag {
    Number,
    Albedo,
    Unbound,
    Illumination
};

class TempHashable : public RTTI {
private:
    mutable uint64_t hash_{0u};
    mutable bool hash_computed_{false};
    mutable uint64_t topology_hash_{0u};
    mutable bool topology_hash_computed_{false};

protected:
    [[nodiscard]] virtual uint64_t compute_hash() const noexcept {
        return hash64(class_name(), reinterpret_cast<uint64_t>(this));
    }
    [[nodiscard]] virtual uint64_t compute_topology_hash() const noexcept {
        return hash64(class_name());
    }

public:
    void reset_hash() const noexcept { hash_computed_ = false; }
    void reset_topology_hash() const noexcept { topology_hash_computed_ = false; }

    [[nodiscard]] uint64_t hash() const noexcept {
        if (!hash_computed_) {
            hash_ = hash64(class_name(), compute_hash());
            hash_computed_ = true;
        }
        return hash_;
    }

    [[nodiscard]] uint64_t topology_hash() const noexcept {
        if (!topology_hash_computed_) {
            topology_hash_ = hash64(class_name(), compute_topology_hash());
            topology_hash_computed_ = true;
        }
        return topology_hash_;
    }

    template<typename T>
    static uint64_t compute_hash(uint64_t hash) {
        return hash64(typeid(std::remove_cvref_t<T>).name(), hash);
    }
    ~TempHashable() = default;
};

struct NodeDesc : public TempHashable {
protected:
    string_view type_;
    ParameterSet parameter_{DataWrap::object()};

public:
    string sub_type;
    string construct_name = "constructor";
    string name;

protected:
    [[nodiscard]] uint64_t compute_hash() const noexcept override {
        return hash64(type_, hash64(sub_type));
    }

public:
    NodeDesc() = default;
    NodeDesc(string_view type, string name)
        : type_(type), sub_type(std::move(name)) {}
    explicit NodeDesc(string_view type) : type_(type) {}
    [[nodiscard]] string parameter_string() const noexcept;
    [[nodiscard]] ParameterSet operator[](const string &key) const noexcept { return parameter_[key]; }
    [[nodiscard]] ParameterSet value(const string &key) const noexcept { return parameter_.value(key); }
    [[nodiscard]] const ParameterSet &parameter_set() const noexcept { return parameter_; }
    template<typename... Args>
    void set_value(Args &&...args) noexcept {
        parameter_.set_value(OC_FORWARD(args)...);
    }
    template<typename... Args>
    [[nodiscard]] bool contains(Args &&...args) const noexcept {
        return parameter_.contains(OC_FORWARD(args)...);
    }
    void set_parameter(const ParameterSet &ps) noexcept;
    [[nodiscard]] bool has_attr(const string &key) const noexcept {
        return parameter_.contains(key);
    }
    virtual void init(const ParameterSet &ps) noexcept {
        if (ps.data().is_object()) {
            string func_name = ps["construct_name"].as_string("");
            if (!func_name.empty()) {
                construct_name = func_name;
            }
            name = ps["name"].as_string();
        }
    };
    void init(const char *str) noexcept {
        init(ParameterSet(DataWrap::parse(str)));
    }
    void init() noexcept {
        init(ParameterSet{DataWrap::object()});
    }
    [[nodiscard]] string file_name() const noexcept;
    [[nodiscard]] string plugin_name() const noexcept {
        return "vision-" + to_lower(string(type_)) + "-" + to_lower(sub_type);
    }
    void set_type(string_view type) noexcept { type_ = type; }
    [[nodiscard]] virtual bool operator==(const NodeDesc &other) const noexcept {
        return hash() == other.hash();
    }
};
#define VISION_DESC_COMMON(type)      \
    type##Desc() : NodeDesc(#type) {} \
    explicit type##Desc(string name) : NodeDesc(#type, std::move(name)) {}

struct TransformDesc : public NodeDesc {
public:
    float4x4 mat{make_float4x4(1.f)};

public:
    void init(const ParameterSet &ps) noexcept override;
};

struct SlotDesc;
struct ShaderNodeDesc;

struct AttrDesc : public NodeDesc {
public:
    using NodeDesc::NodeDesc;

public:
    template<typename T>
    [[nodiscard]] SlotDesc slot(const string &key, T default_value,
                                AttrTag tag = AttrTag::Number) const noexcept;
};

struct GraphDesc : public AttrDesc {
public:
    using AttrDesc::AttrDesc;

public:
    std::map<std::string, ShaderNodeDesc> node_map;

public:
    void init_node_map(const ParameterSet &ps) noexcept;
    void init(const ParameterSet &ps) noexcept override;
    template<typename T>
    [[nodiscard]] SlotDesc slot(const string &key, T default_value,
                                AttrTag tag = AttrTag::Number) const noexcept;

    [[nodiscard]] SlotDesc slot(const string &key, const DataWrap &data,
                                AttrTag tag = AttrTag::Number) const noexcept;
};

struct ShaderNodeDesc : public AttrDesc {
public:
    AttrTag node_tag{};
    using Super = AttrDesc;

protected:
    [[nodiscard]] uint64_t compute_hash() const noexcept override;

public:
    ShaderNodeDesc() : Super("ShaderNode") {}
    explicit ShaderNodeDesc(AttrTag tag, const string &s_type = "constant")
        : Super("ShaderNode"), node_tag(tag) {
        sub_type = s_type;
        parameter_.set_json(DataWrap::object());
    }
    ShaderNodeDesc(string name, AttrTag tag)
        : Super("ShaderNode", std::move(name)), node_tag(tag) {
        sub_type = "constant";
        parameter_.set_json(DataWrap::object());
    }
    template<typename Arg>
    requires is_scalar_v<Arg>
    ShaderNodeDesc(Arg v, AttrTag tag)
        : Super("ShaderNode"), node_tag(tag) {
        sub_type = "number";
        parameter_.set_json(DataWrap::object());
        parameter_.set_value("value", v);
    }
    template<typename T, size_t N>
    ShaderNodeDesc(Vector<T, N> v, AttrTag tag)
        : Super("ShaderNode"), node_tag(tag) {
        sub_type = "number";
        parameter_.set_json(DataWrap::object());
        if constexpr (N == 2) {
            parameter_.set_value("value", {v.x, v.y});
        } else if constexpr (N == 3) {
            parameter_.set_value("value", {v.x, v.y, v.z});
        } else if constexpr (N == 4) {
            parameter_.set_value("value", {v.x, v.y, v.z, v.w});
        }
    }
    ShaderNodeDesc(const DataWrap &data, AttrTag tag)
        : Super("ShaderNode"), node_tag(tag) {
        sub_type = "number";
        parameter_.set_json(DataWrap::object());
        parameter_.set_value("value", data);
    }

    [[nodiscard]] SP<SlotDesc> slot(const string &key,
                                    AttrTag type = AttrTag::Number) const noexcept;

    void init(const ParameterSet &ps) noexcept override;
    void init(const ParameterSet &ps, fs::path scene_path) noexcept {
        init(ps);
    }
};

struct SlotDesc : public NodeDesc {
public:
    [[nodiscard]] static string default_channels(uint dim) noexcept {
        switch (dim) {
            case 1: return "x";
            case 2: return "xy";
            case 3: return "xyz";
            case 4: return "xyzw";
            default: break;
        }
        return "";
    }
    string channels;
    string output_key;
    AttrTag attr_tag{};
    ShaderNodeDesc node;
    [[nodiscard]] uint dim() const noexcept { return channels.size(); }
    VISION_DESC_COMMON(Slot)
    SlotDesc(ShaderNodeDesc node, uint dim, AttrTag tag)
        : node(std::move(node)), channels(default_channels(dim)),
          attr_tag(tag) {}

    SlotDesc(AttrTag tag, uint dim)
        : node(tag), channels(default_channels(dim)),
          attr_tag(tag) {}

    void init(const ParameterSet &ps) noexcept override;
    void init(const ParameterSet &ps, fs::path scene_path) noexcept {
        init(ps);
    }
};

template<typename T>
[[nodiscard]] SlotDesc AttrDesc::slot(const string &key, T default_value,
                                      AttrTag tag) const noexcept {
    ShaderNodeDesc node{default_value, tag};
    SlotDesc slot_desc{node, type_dimension_v<T>, tag};
    slot_desc.init(parameter_[key]);
    slot_desc.node.name = key;
    return slot_desc;
}

template<typename T>
[[nodiscard]] SlotDesc GraphDesc::slot(const string &key, T default_value,
                                       AttrTag tag) const noexcept {
    return AttrDesc::slot(key, default_value, tag);
}

struct UpsamplerDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Upsampler)
    void init(const ParameterSet &ps) noexcept override;
};

struct RadianceCacheDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(RadianceCache)
    void init(const ParameterSet &ps) noexcept override;
};



struct PassDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Pass)
    void init(const ParameterSet &ps) noexcept override;
};

struct RasterizerDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Rasterizer)
    void init(const ParameterSet &ps) noexcept override;
};

struct UVUnwrapperDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(UVUnwrapper)
    void init(const ParameterSet &ps) noexcept override;
};


struct ToneMapperDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(ToneMapper)
    void init(const ParameterSet &ps) noexcept override;
};

struct FrameBufferDesc : public NodeDesc {
public:
    ToneMapperDesc tone_mapper;
    UpsamplerDesc upsampler_desc;

public:
    VISION_DESC_COMMON(FrameBuffer)
    void init(const ParameterSet &ps) noexcept override;
};

struct PipelineDesc : public NodeDesc {
public:
    mutable Device *device{nullptr};
    UVUnwrapperDesc unwrapper_desc;
    FrameBufferDesc frame_buffer_desc;
    RasterizerDesc rasterizer_desc;

public:
    VISION_DESC_COMMON(Pipeline)
    void init(const ParameterSet &ps) noexcept override;
};

struct FilterDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Filter)
    void init(const ParameterSet &ps) noexcept override;
};

struct DenoiserDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Denoiser)
    void init(const ParameterSet &ps) noexcept override;
};

struct LightDesc : public GraphDesc {
public:
    TransformDesc o2w;

    LightDesc() : GraphDesc("Light") {}
    explicit LightDesc(string name) : GraphDesc("Light", std::move(name)) {}
    void init(const ParameterSet &ps) noexcept override;
    [[nodiscard]] bool valid() const noexcept {
        return !sub_type.empty();
    }
};

struct ShapeDesc : public NodeDesc {
public:
    TransformDesc o2w;
    mutable LightDesc emission;

public:
    VISION_DESC_COMMON(Shape)
    void init(const ParameterSet &ps) noexcept override;
    [[nodiscard]] bool operator==(const ShapeDesc &other) const noexcept;
};

struct SamplerDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Sampler)
    void init(const ParameterSet &ps) noexcept override;
};

struct SensorDesc : public NodeDesc {
public:
    TransformDesc transform_desc;
    FilterDesc filter_desc;
    NameID medium;

public:
    VISION_DESC_COMMON(Sensor)
    void init(const ParameterSet &ps) noexcept override;
};

struct IntegratorDesc : public NodeDesc {
public:
    DenoiserDesc denoiser_desc;
    RadianceCacheDesc cache_desc;

public:
    VISION_DESC_COMMON(Integrator)
    void init(const ParameterSet &ps) noexcept override;
};

struct MediumDesc : public NodeDesc {
public:
    ShaderNodeDesc sigma_a{Unbound};
    ShaderNodeDesc sigma_s{Unbound};
    ShaderNodeDesc g{Number};
    ShaderNodeDesc scale{Number};

public:
    VISION_DESC_COMMON(Medium)
    void init(const ParameterSet &ps) noexcept override;
};

struct MaterialDesc : public GraphDesc {
public:
    SP<MaterialDesc> mat0;
    SP<MaterialDesc> mat1;

public:
    MaterialDesc() : GraphDesc("Material") {}
    explicit MaterialDesc(string name) : GraphDesc("Material", std::move(name)) {}
    void init(const ParameterSet &ps) noexcept override;
};

struct LightSamplerDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(LightSampler)
    void init(const ParameterSet &ps) noexcept override;
};

struct WarperDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Warper)
    void init(const ParameterSet &ps) noexcept override;
};

struct SpectrumDesc : public NodeDesc {
public:
    VISION_DESC_COMMON(Spectrum)
    void init(const ParameterSet &ps) noexcept override;
};

struct OutputDesc : public NodeDesc {
public:
    /// Export/runtime output controls. FrameBuffer owns resolution.
    string fn;
    uint spp{0u};
    bool save_exit{false};
    bool denoise{false};
    VISION_DESC_COMMON(Output)
    void init(const ParameterSet &ps) noexcept override;
};

struct RenderSettingDesc : public NodeDesc {
    PolymorphicMode polymorphic_mode{};
    float min_world_radius{};
    float ray_offset_factor{1.f};
    VISION_DESC_COMMON(RenderSetting)
    void init(const ParameterSet &ps) noexcept override;
};
}// namespace vision
