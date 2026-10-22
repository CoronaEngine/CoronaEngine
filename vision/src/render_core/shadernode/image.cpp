//
// Created by Zero on 09/09/2022.
//

#include "base/shader_graph/shader_node.h"
#include "base/import/param_schema.h"
#include "rhi/common.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/global.h"

namespace vision {
using namespace ocarina;

class ImageNode : public SlotsShaderNode {
private:
#define IMAGE_SLOTS_NO_DEFAULT(X) \
    X(vector, Number, 3u, false)

#define IMAGE_DECLARE_SLOT_(name, tag, dim, required) VS_MAKE_SLOT(name)
    IMAGE_SLOTS_NO_DEFAULT(IMAGE_DECLARE_SLOT_)
#undef IMAGE_DECLARE_SLOT_

    RegistrableTexture3D *texture_{};
    EncodedData<uint> tex_id_{};
    EncodedData<uint> gamma_correct_{false};
    ShaderNodeDesc desc_;
    EncodedData<float> scale_{1.f};
    EncodedData<float4x4> uv_transform_{make_float4x4(1.f)};
    EncodedData<uint> wrap_mode_{0u};
    EncodedData<uint> nearest_{0u};
    mutable optional<float_array> cache_;

    [[nodiscard]] uint64_t compute_hash() const noexcept override {
        // Image identity is part of material identity. Hashing only the vector
        // slot would merge different source images into the same material.
        return hash64(SlotsShaderNode::compute_hash(), desc_.hash(), scale_.hv());
    }
    [[nodiscard]] uint64_t compute_topology_hash() const noexcept override {
        return hash64(SlotsShaderNode::compute_topology_hash(), channel_num());
    }

    [[nodiscard]] RegistrableTexture3D &obtain_texture(const ShaderNodeDesc &desc) noexcept {
        auto *rp = pipeline();
        if (rp->scene().geometry().has_gpu_resource()) {
            return rp->image_pool().obtain_texture(
                desc,
                rp->scene().geometry().bindless_array(),
                rp->scene().geometry().gpu_resource()->device());
        }
        return rp->image_pool().obtain_texture(desc, rp->bindless_array(), rp->device());
    }

public:
    ImageNode() = default;
    explicit ImageNode(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc),
          desc_(desc),
          scale_(desc["scale"].as_float(1.f)) {
        const auto uv=desc.parameter_set().contains("uv_transform")
            ? desc["uv_transform"].as_vector<float>() : std::vector<float>{};
        if (uv.size()==9) {
            auto& m=uv_transform_.hv();
            m[0]=make_float4(uv[0],uv[3],0,0);
            m[1]=make_float4(uv[1],uv[4],0,0);
            m[3]=make_float4(uv[2],uv[5],0,1);
        }
        const auto wrap=desc["wrap"].as_string("repeat");
        wrap_mode_=wrap=="clamp" ? 1u : wrap=="mirror" ? 2u : wrap=="black" ? 3u : 0u;
        nearest_=desc["filter"].as_string("bilinear")=="nearest" ? 1u : 0u;
        texture_ = &obtain_texture(desc);
        tex_id_ = texture_->index();
    }
    VS_MAKE_GUI_STATUS_FUNC(ShaderNode, vector_)
    OC_ENCODABLE_FUNC(ShaderNode, vector_, tex_id_, scale_, uv_transform_, wrap_mode_, nearest_)
    VS_MAKE_PLUGIN_NAME_FUNC
    VS_HOTFIX_MAKE_RESTORE(ShaderNode, vector_, texture_, tex_id_, desc_, scale_, uv_transform_, wrap_mode_, nearest_)

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define IMAGE_REGISTER_PARAM_(name, tag, dim, required) ret.add_slot(#name, tag, dim, required);
            IMAGE_SLOTS_NO_DEFAULT(IMAGE_REGISTER_PARAM_)
#undef IMAGE_REGISTER_PARAM_
            ret.add_plain("fn", ParamType::String);
            ret.add_plain("scale", ParamType::Float);
            ret.add_plain("color_space", ParamType::String);
            ret.add_plain("source_encoding", ParamType::String);
            ret.add_plain("wrap", ParamType::String);
            ret.add_plain("filter", ParamType::String);
            // Bridge metadata is also retained in the descriptor/hash.
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define IMAGE_INIT_SLOT_(name, tag, dim, required) VS_INIT_SLOT_NO_DEFAULT(name, tag);
        IMAGE_SLOTS_NO_DEFAULT(IMAGE_INIT_SLOT_)
#undef IMAGE_INIT_SLOT_
    }

    void reload(Widgets *widgets) noexcept {
        fs::path path = texture_->host_tex().path();
        if (Widgets::open_file_dialog(path)) {
            desc_.set_value("fn", path.string());
            desc_.reset_hash();
            texture_ = &obtain_texture(desc_);
            texture_->upload_immediately();
            tex_id_.hv() = texture_->index().hv();
            changed_ = true;
        }
    }

    void render_sub_UI(Widgets *widgets) noexcept override {
        vector_.render_UI(widgets);
        changed_ |= widgets->drag_float("scale", addressof(scale_.hv()), 0.05, 0);
        widgets->button_click("reload", [&] {
            reload(widgets);
        });
        widgets->image(texture_->host_tex());
    }

    bool render_UI(Widgets *widgets) noexcept override {
        bool ret = widgets->use_tree(ocarina::format("{} detail", name_), [&] {
            render_sub_UI(widgets);
        });
        return ret;
    }

    [[nodiscard]] AttrEvalContext evaluate(const string &key, const AttrEvalContext &ctx,
                                           const SampledWavelengths &swl) const noexcept override {
        float_array value = evaluate(ctx, swl).array;
        if (key == "Alpha") {
            if (channel_num() < 4) {
                return float_array::create(0);
            }
            return value.w();
        } else if (key == "Color") {
            return value.xyz();
        }
        return value;
    }

    [[nodiscard]] uint channel_num() const noexcept { return texture_->host_tex().channel_num(); }

    [[nodiscard]] AttrEvalContext evaluate(const AttrEvalContext &ctx,
                                           const SampledWavelengths &swl) const noexcept override {

        if (!cache_) {
            AttrEvalContext ctx_processed = vector_.evaluate(ctx, swl);
            Float2 uv=transform_point(*uv_transform_,make_float3(ctx_processed.uv(),0.f)).xy();
            Bool outside=any(uv<make_float2(0.f)) || any(uv>make_float2(1.f));
            uv=select(*wrap_mode_==1u,clamp(uv,make_float2(0.000001f),make_float2(.999999f)),uv);
            uv=select(*wrap_mode_==2u,make_float2(1.f)-abs(fract(uv*.5f)*2.f-make_float2(1.f)),uv);
            const auto size=texture_->host_tex().resolution();
            const auto extent=make_float2(float(size.x),float(size.y));
            uv=select(*nearest_!=0u,(floor(uv*extent)+make_float2(.5f))/extent,uv);
            float_array value = texture_->sample(channel_num(), uv);
            value = value * *scale_ * select(*wrap_mode_==3u && outside,0.f,1.f);
            cache_.emplace(value);
        }
        return *cache_;
    }

    void on_after_decode() const noexcept override {
        cache_.reset();
    }

    [[nodiscard]] ocarina::vector<float> average() const noexcept override {
        return texture_->host_tex().average_vector();
    }
    [[nodiscard]] uint2 resolution() const noexcept override {
        return texture_->device_tex()->resolution().xy();
    }
    void for_each_pixel(const function<Image::foreach_signature> &func) const noexcept override {
        texture_->host_tex().for_each_pixel(func);
    }
};
}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, ImageNode)
