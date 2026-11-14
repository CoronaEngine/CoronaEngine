#pragma once

#include "material.h"

namespace vision {

// A short-lived evaluation view for many incoming directions at one outgoing
// direction. Shared cache variables are declared before material dispatch and
// assigned by the selected case; the consumer itself runs outside that switch.
// It borrows all original lobes, spectra and distributions; create a new view
// after changing the outgoing direction, shading frames or material parameters.
// Keep it inside with_prepared_evaluation's callback and the current DSL scope.
class PreparedMaterialEvaluator {
private:
    struct Slot {
        Frame<Float3> frame{make_float3(0.f), make_float3(0.f), make_float3(0.f)};
        Float3 wo{make_float3(0.f)};
        Float g1{0.f};
        Float abs_cos_wo{0.f};

        void prepare_frame(const ShadingFrame &original, const Float3 &world_wo) noexcept {
            // Assign existing outer-scope DSL variables. Reconstructing these
            // members in a dispatch case would leave branch-local references.
            frame.x = original.nx();
            frame.y = original.ny();
            frame.z = original.nz();
            wo = frame.to_local(world_wo);
        }
    };

    [[nodiscard]] static Slot *acquire_slot(vector<std::unique_ptr<Slot>> &slots, uint index) {
        if (index == slots.size()) { slots.push_back(std::make_unique<Slot>()); }
        return slots[index].get();
    }

    struct Leaf {
        const Lobe *lobe;
        bool eta_overload;
        bool own_frame{};
        Slot *outgoing{};
        Slot *pdf_slot{};
        const MicrofacetReflection *reflection{};
        const GGXMicrofacet *ggx{};

        Leaf(const Lobe &original, Slot *common_outgoing, bool with_eta,
             MaterialEvalMode mode, vector<std::unique_ptr<Slot>> &slots, uint &next_slot)
            : lobe(&original), eta_overload(with_eta) {
            own_frame = !common_outgoing && lobe->supports_prepared_local_evaluation();
            if ((common_outgoing || own_frame) && BxDF::match_PDF(mode)) {
                reflection = lobe->prepared_reflection_pdf();
                if (reflection) { ggx = static_cast<const GGXMicrofacet *>(reflection->microfacet()); }
            }
            // DFS indices restart for each root. Only one root is active per
            // lane, so these stable slots are shared across all root trees.
            Slot *slot = own_frame || reflection ? acquire_slot(slots, next_slot++) : nullptr;
            outgoing = common_outgoing ? common_outgoing : slot;
            if (reflection) { pdf_slot = slot; }
        }

        void prepare(const Float3 &world_wo) const noexcept {
            if (own_frame) { outgoing->prepare_frame(lobe->shading_frame(), world_wo); }
            if (reflection) {
                pdf_slot->abs_cos_wo = abs_cos_theta(outgoing->wo);
                // Preserve the original anisotropic GGX G1, including its
                // grazing-angle behavior. Only its evaluation is moved.
                pdf_slot->g1 = microfacet::bsdf_G1<D>(outgoing->wo, ggx->alpha_x(), ggx->alpha_y(), GGX);
            }
        }

        [[nodiscard]] ScatterEval evaluate_original_local(const Float3 &wi, MaterialEvalMode mode,
                                                          const Uint &flag, TransportMode tm) const noexcept {
            return eta_overload
                       ? lobe->evaluate_local(outgoing->wo, wi, mode, flag, tm, nullptr)
                       : lobe->evaluate_local(outgoing->wo, wi, mode, flag, tm);
        }

        [[nodiscard]] ScatterEval evaluate(const Float3 &world_wo, const Float3 &world_wi,
                                           const Float3 *common_wi, MaterialEvalMode mode,
                                           const Uint &flag, TransportMode tm,
                                           bool individual) const noexcept {
            if (!outgoing) {
                // Preserve custom world-space implementations without assuming
                // that their wrapper is Lobe's ordinary frame transform.
                return eta_overload
                           ? lobe->evaluate(world_wo, world_wi, mode, flag, tm, nullptr)
                           : lobe->evaluate(world_wo, world_wi, mode, flag, tm);
            }
            const Float3 wi = common_wi ? *common_wi : outgoing->frame.to_local(world_wi);
            ScatterEval se{*lobe->swl()};
            if (reflection && BxDF::match_PDF(mode)) {
                if (BxDF::match_F(mode)) {
                    // Keep all original virtual F work: Fresnel, kr, correlated
                    // G2, layer behavior and PureReflection/Metal compensation.
                    se = evaluate_original_local(wi, MaterialEvalMode::F, flag, tm);
                } else {
                    se.flags = reflection->flags();
                }
                const Float3 wh = normalize(outgoing->wo + wi);
                const Float abs_dot_wo_h = abs_dot(outgoing->wo, wh);
                // Same association as original PDF_wh followed by the original
                // reflection Jacobian. Do not cancel absdot or fold G1/|wo.z|.
                const Float pdf_wh = ggx->bsdf_D(wh) * pdf_slot->g1 * abs_dot_wo_h / pdf_slot->abs_cos_wo;
                const Float pdf = ggx->PDF_wi_reflection(pdf_wh, outgoing->wo, wh);
                const Bool valid = reflection->valid(outgoing->wo, wi, make_float3(0.f, 0.f, 1.f));
                se.pdfs = select(valid, pdf, 0.f);
            } else {
                se = evaluate_original_local(wi, mode, flag, tm);
            }
            if (individual) { se.f *= abs_cos_theta(wi); }
            return se;
        }
    };

    struct Node;

    struct Child {
        const WeightedLobe *weighted;
        std::unique_ptr<Node> node;
    };

    struct Node {
        const Lobe *lobe;
        std::unique_ptr<Leaf> leaf;
        vector<Child> children;

        Node(const Lobe &original, Slot *common_outgoing, bool eta_overload,
             MaterialEvalMode mode, vector<std::unique_ptr<Slot>> &slots, uint &next_slot)
            : lobe(&original) {
            if (const auto *set = lobe->prepared_lobe_set()) {
                set->for_each([&](const WeightedLobe &weighted) {
                    children.push_back(Child{&weighted,
                                             std::make_unique<Node>(*weighted, common_outgoing, true, mode, slots, next_slot)});
                });
            } else {
                leaf = std::make_unique<Leaf>(original, common_outgoing, eta_overload, mode, slots, next_slot);
            }
        }

        void prepare(const Float3 &world_wo) const noexcept {
            if (leaf) {
                leaf->prepare(world_wo);
            } else {
                for (const auto &child : children) { child.node->prepare(world_wo); }
            }
        }

        [[nodiscard]] ScatterEval evaluate(const Float3 &world_wo, const Float3 &world_wi,
                                           const Float3 *common_wo, const Float3 *common_wi,
                                           MaterialEvalMode mode, const Uint &flag,
                                           TransportMode tm, bool individual) const noexcept {
            if (leaf) { return leaf->evaluate(world_wo, world_wi, common_wi, mode, flag, tm, individual); }
            ScatterEval ret{*lobe->swl()};
            for (const auto &child : children) {
                const WeightedLobe &weighted = *child.weighted;
                ScatterEval se = child.node->evaluate(world_wo, world_wi, common_wo, common_wi,
                                                       mode, flag, tm, individual);
                // Preserve every tree level's original polymorphic validity,
                // weights, addition order and unconditional flags accumulation.
                const Float factor = individual
                                         ? weighted->valid_world_factor(world_wo, world_wi)
                                         : weighted->valid_factor(*common_wo, *common_wi);
                se.f *= weighted.weight() * factor;
                se.pdfs *= weighted.sample_weight() * factor;
                ret.f += se.f;
                ret.pdfs += se.pdfs;
                ret.flags |= se.flags;
            }
            return ret;
        }
    };

    const MaterialEvaluator &owner_;
    const SampledWavelengths *swl_;
    Float3 world_wo_;
    Float3 ng_;
    MaterialEvalMode mode_;
    Uint flag_;
    TransportMode tm_;
    bool individual_;
    std::unique_ptr<Slot> common_outgoing_;
    vector<std::unique_ptr<Slot>> slots_;
    unordered_map<const Lobe *, std::unique_ptr<Node>> roots_;

    PreparedMaterialEvaluator(const MaterialEvaluator &owner, const SampledWavelengths &swl,
                              const ShadingFrame &frame, const Float3 &ng, const Float3 &world_wo,
                              MaterialEvalMode mode, const Uint &flag,
                              TransportMode tm, bool individual)
        : owner_(owner), swl_(&swl), world_wo_(world_wo), ng_(ng), mode_(mode), flag_(flag), tm_(tm), individual_(individual) {
        if (!individual_) {
            common_outgoing_ = std::make_unique<Slot>();
            common_outgoing_->prepare_frame(frame, world_wo_);
        }
        // Construct all metadata and declare max-per-root shared slots before
        // emitting any selected-case assignments. Nodes borrow Slot pointers;
        // no DSL-valued cache is copied into a root-specific metadata object.
        for (const auto &root : owner_) {
            uint next_slot = 0u;
            roots_.emplace(root.get(), std::make_unique<Node>(*root, common_outgoing_.get(), false,
                                                             mode_, slots_, next_slot));
        }
        owner_.dispatch([&](const Lobe *root) { roots_.at(root)->prepare(world_wo_); });
    }

    friend class MaterialEvaluator;

public:
    PreparedMaterialEvaluator(const PreparedMaterialEvaluator &) = delete;
    PreparedMaterialEvaluator &operator=(const PreparedMaterialEvaluator &) = delete;

    [[nodiscard]] ScatterEval evaluate(const Float3 &world_wi) const noexcept {
        return evaluate(world_wi, mode_);
    }

    // The construction mode controls which outgoing-only work is prepared.
    // An All view can evaluate candidates with PDF and the winner with All.
    [[nodiscard]] ScatterEval evaluate(const Float3 &world_wi, MaterialEvalMode mode) const noexcept {
        OC_ASSERT((static_cast<uint>(mode) & ~static_cast<uint>(mode_)) == 0u);
        optional<Float3> common_wi;
        if (common_outgoing_) { common_wi.emplace(common_outgoing_->frame.to_local(world_wi)); }
        ScatterEval ret{*swl_};
        // Keep only BSDF evaluation in the material switch. The caller's light
        // sampling, visibility and reservoir loops are emitted once outside it.
        owner_.dispatch([&](const Lobe *root) {
            ret = roots_.at(root)->evaluate(world_wo_, world_wi,
                                            common_outgoing_ ? &common_outgoing_->wo : nullptr,
                                            common_wi ? &*common_wi : nullptr,
                                            mode, flag_, tm_, individual_);
        });
        const Bool discard = same_hemisphere(world_wo_, world_wi, ng_) == BxDFFlag::is_transmission(ret.flags);
        ret.pdfs = select(discard, 0.f, ret.pdfs);
        if (common_wi) { ret.f *= abs_cos_theta(*common_wi); }
        return ret;
    }
};

} // namespace vision
