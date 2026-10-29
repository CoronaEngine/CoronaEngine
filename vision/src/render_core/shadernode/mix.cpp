//
// Created by Zero on 07/03/2023.
//

#include "base/shader_graph/shader_graph.h"

namespace vision {
//{
//    "type" : "lerp",
//    "param" : {
//        "t" : {
//            "channels" : "x",
//            "node" : {
//                "type" : "number",
//                "param" : {
//                    "value": [0.5]
//                }
//            }
//        },
//        "A" : {
//            "channels" : "xyz",
//            "node" : {
//                "type" : "image",
//                "param" : {
//                    "fn" : "checker.jpg",
//                    "color_space" : "srgb"
//                }
//            }
//        },
//        "B" : {
//            "channels" : "xyz",
//            "node" : {
//                "type": "number",
//                "param": {
//                    "value": [0.9,1,0.9]
//                }
//            }
//        }
//    }
//}
class MixNode : public SlotsShaderNode {
private:
    VS_MAKE_SLOT(t)
    VS_MAKE_SLOT(A)
    VS_MAKE_SLOT(B)

public:
    MixNode() = default;
    explicit MixNode(const ShaderNodeDesc &desc) : SlotsShaderNode(desc) {}
    void initialize_slots(const ShaderNodeDesc &desc) noexcept override {
        VS_INIT_SLOT(t, .5f, Number);
        VS_INIT_SLOT(A, ocarina::make_float3(0.f), Number);
        VS_INIT_SLOT(B, ocarina::make_float3(1.f), Number);
    }
    VS_MAKE_PLUGIN_NAME_FUNC
    VS_HOTFIX_MAKE_RESTORE(SlotsShaderNode, t_, A_, B_)
    [[nodiscard]] bool is_uniform() const noexcept override {
        return t_->is_uniform() && A_->is_uniform() && B_->is_uniform();
    }
    [[nodiscard]] bool is_constant() const noexcept override {
        return t_->is_constant() && A_->is_constant() && B_->is_constant();
    }
    [[nodiscard]] uint64_t compute_topology_hash() const noexcept override {
        return hash64(t_.topology_hash(), A_.topology_hash(), B_.topology_hash());
    }
    [[nodiscard]] uint64_t compute_hash() const noexcept override {
        return hash64(t_.hash(), A_.hash(), B_.hash());
    }
    [[nodiscard]] ocarina::vector<float> average() const noexcept override {
        return ocarina::lerp(t_.average(), A_.average(), B_.average());
    }
    [[nodiscard]] AttrEvalContext evaluate(const AttrEvalContext &ctx,
                                          const SampledWavelengths &swl) const noexcept override {
        return ocarina::lerp(t_.evaluate(ctx, swl).array,
                             A_.evaluate(ctx, swl).array,
                             B_.evaluate(ctx, swl).array);
    }
};
}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX(vision, MixNode)
