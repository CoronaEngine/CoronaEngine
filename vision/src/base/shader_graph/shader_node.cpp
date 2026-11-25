//
// Created by Zero on 23/01/2023.
//

#include "shader_node.h"

#include <utility>
#include "base/import/param_schema.h"
#include "base/mgr/pipeline.h"
#include "window/GUI/widgets.h"

namespace vision {

const ShaderNodeSlot *ShaderNodeSlotSet::input_slot(string_view name) const noexcept {
    const ShaderNodeSlot *result = nullptr;
    for_each_slot<false>([&](const ShaderNodeSlot &slot) {
        if (slot.attr_name() == name) result = addressof(slot);
    });
    return result;
}

const ShaderNodeSlot &ShaderNodeSlotSet::get_slot(int index) const noexcept {
    return *(reinterpret_cast<const ShaderNodeSlot *>(size_t(this) + slot_infos_[index].offset));
}

size_t ShaderNodeSlotSet::ShaderNodeSlotSet::slot_num() const noexcept {
    return slot_infos_.size();
}

ShaderNodeSlot &ShaderNodeSlotSet::get_slot(int index) noexcept {
    return *const_cast<ShaderNodeSlot *>(reinterpret_cast<const ShaderNodeSlot *>(size_t(this) + slot_infos_[index].offset));
}

void ShaderNodeSlotSet::register_slot(const ShaderNodeSlot &slot) noexcept {
    SlotInfo info{};
#ifndef NDEBUG
    info.slot_name = slot.attr_name();
#endif
    info.offset = static_cast<uint>(size_t(&slot) - size_t(this));
    slot_infos_.push_back(ocarina::move(info));
}

void SlotsShaderNode::restore(vision::RuntimeObject *old_obj) noexcept {
    VS_HOTFIX_MOVE_ATTRS(slot_infos_)
    for (int i = 0; i < slot_num(); ++i) {
        ShaderNodeSlot &slot = get_slot(i);
        ShaderNodeSlot &old_slot = old_obj_->get_slot(i);
        slot = ocarina::move(old_slot);
    }
}

void SlotsShaderNode::validate_params(const ShaderNodeDesc &desc, const ParamSchema &schema) const noexcept {
    string context = "shadernode";
    if (!desc.name.empty()) {
        context += " '" + desc.name + "'";
    }
    string type = string(impl_type());
    if (!type.empty()) {
        context += " [" + type + "]";
    }
    schema.validate(desc, context);
}

uint64_t SlotsShaderNode::compute_hash() const noexcept {
    uint64_t ret = Hash64::default_seed;
    reduce_slots(ret, [&](uint64_t hash, const ShaderNodeSlot &slot) {
        return hash64(hash, slot.hash());
    });
    return ret;
}

uint64_t SlotsShaderNode::compute_topology_hash() const noexcept {
    uint64_t ret = Hash64::default_seed;
    reduce_slots(ret, [&](uint64_t hash, const ShaderNodeSlot &slot) {
        return hash64(hash, slot.topology_hash());
    });
    return ret;
}

uint SlotsShaderNode::compacted_size() const noexcept {
    return reduce_slots(0u, [&](uint size, const ShaderNodeSlot &slot) {
        return size + slot->compacted_size();
    });
}

uint SlotsShaderNode::cal_offset(ocarina::uint prev_size) const noexcept {
    auto ret = reduce_slots(prev_size, [&](uint size, const ShaderNodeSlot &slot) {
        auto ofs = slot->cal_offset(size);
        return ofs;
    });
    return ret;
}

uint SlotsShaderNode::alignment() const noexcept {
    return reduce_slots(0u, [&](uint align, const ShaderNodeSlot &slot) {
        return ocarina::max(align, slot->alignment());
    });
}

bool SlotsShaderNode::has_device_value() const noexcept {
    return reduce_slots(true, [&](bool b, const ShaderNodeSlot &slot) {
        return b && slot->has_device_value();
    });
}

void SlotsShaderNode::invalidate() const noexcept {
    for_each_slot([&](const ShaderNodeSlot &slot) {
        slot->invalidate();
    });
}

void SlotsShaderNode::after_decode() const noexcept {
    for_each_slot([&](const ShaderNodeSlot &slot) {
        slot->after_decode();
    });
}

void SlotsShaderNode::encode(RegistrableManaged<buffer_ty> &data) const noexcept {
    for_each_slot([&](const ShaderNodeSlot &slot) {
        slot->encode(data);
    });
}

void SlotsShaderNode::decode(const DataAccessor *da) const noexcept {
    for_each_slot([&](const ShaderNodeSlot &slot) {
        slot->decode(da);
    });
}

void SlotsShaderNode::decode(const DynamicArray<ocarina::buffer_ty> &array) const noexcept {
    for_each_slot([&](const ShaderNodeSlot &slot) {
        slot->decode(array);
    });
}

///#endregion

void SlotsShaderNode::reset_status() noexcept {
    for_each_slot([&](ShaderNodeSlot &slot) {
        slot.reset_status();
    });
    Node::reset_status();
}

bool SlotsShaderNode::has_changed() noexcept {
    return Node::has_changed() || reduce_slots(false, [&](bool b, ShaderNodeSlot &slot) {
               return b || slot->has_changed();
           });
}

bool SlotsShaderNode::render_UI(Widgets *widgets) noexcept {
    render_sub_UI(widgets);
    bool ret = widgets->use_tree(ocarina::format("{} detail", name_), [&] {
        for_each_slot([&](ShaderNodeSlot &slot) {
            slot.render_UI(widgets);
        });
    });
    return ret;
}

void SlotsShaderNode::render_sub_UI(Widgets *widgets) noexcept {
    ShaderNode::render_sub_UI(widgets);
}

///#region SlotBase
SlotBase::SlotBase(int, std::string channels, AttrTag attr_tag)
    : dim_(channels.size()),
#ifndef NDEBUG
      channels_(channels),
#endif
      channel_mask_(calculate_mask(ocarina::move(channels))),
      attr_tag_(attr_tag) {
}

uint SlotBase::calculate_mask(string channels) noexcept {
    uint ret{};
    channels = to_lower(channels);
    static map<char, uint> dict{
        {'x', 0u},
        {'y', 1u},
        {'z', 2u},
        {'w', 3u},
        {'r', 0u},
        {'g', 1u},
        {'b', 2u},
        {'a', 3u},
    };
    for (char channel : channels) {
        ret = (ret << 4) | dict[channel];
    }
    return ret;
}
///#endregion

///#region Slot
ShaderNodeSlot::ShaderNodeSlot(SP<vision::ShaderNode> input, std::string channels, AttrTag attr_tag, string key)
    : SlotBase(0, std::move(channels), attr_tag),
      node_(std::move(input)),
      output_key_(std::move(key)) {
    OC_ASSERT(dim_ <= 4);
}

uint64_t ShaderNodeSlot::compute_hash() const noexcept {
    return hash64(channel_mask_, dim_, (node_ ? node_->hash() : 0u), attr_tag_);
}

uint64_t ShaderNodeSlot::compute_topology_hash() const noexcept {
    return hash64(channel_mask_, dim_, (node_ ? node_->topology_hash() : 0u), attr_tag_, output_key_);
}

ShaderNodeSlot ShaderNodeSlot::create_slot(const vision::SlotDesc &desc) {
    SP<ShaderNode> shader_node = Node::create_shared<ShaderNode>(desc.node);
    return ShaderNodeSlot(shader_node, desc.channels, desc.attr_tag, desc.output_key);
}

ShaderNode &ShaderNodeSlot::set(const vision::ShaderNodeSlot &other) noexcept {
    string old_name = attr_name_;
    *this = other;
    if (other.attr_name_.empty()) {
        attr_name_ = old_name;
    }
    reset_topology_hash();
    reset_hash();
    return *node_;
}

void ShaderNodeSlot::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    if (node_) {
        HotfixSystem::replace_objects(constructor, std::tuple{addressof(node_)});
    }
}

void ShaderNodeSlot::reset_status() noexcept {
    if (node_) {
        node_->reset_status();
    }
}

bool ShaderNodeSlot::has_changed() noexcept {
    if (node_) {
        return node_->has_changed();
    }
    return false;
}

void ShaderNodeSlot::render_sub_UI(Widgets *widgets) noexcept {
    if (node_) {
        node_->render_sub_UI(widgets);
    }
}

bool ShaderNodeSlot::render_UI(Widgets *widgets) noexcept {
    if (node_) {
        if (!output_key_.empty()) {
            widgets->text(ocarina::format("{} output key: {}", attr_name_, output_key_));
        }
        if (!attr_name_.empty()) {
            node_->set_name(attr_name_);
        }
        return node_->render_UI_by_tag(widgets, attr_tag_);
    }
    return false;
}

AttrEvalContext ShaderNodeSlot::evaluate(const AttrEvalContext &ctx,
                                         const SampledWavelengths &swl) const noexcept {
    if (!node_) {
        return ctx;
    }
    if (!output_key_.empty()) {
        return node_->evaluate(output_key_, ctx, swl);
    }
    switch (dim_) {
        case 1: {
            switch (channel_mask_) {
#include "slot_swizzle_1.inl.h"
            }
        }
        case 2: {
            switch (channel_mask_) {
#include "slot_swizzle_2.inl.h"
            }
        }
        case 3: {
            switch (channel_mask_) {
#include "slot_swizzle_3.inl.h"
            }
        }
        case 4: {
            switch (channel_mask_) {
#include "slot_swizzle_4.inl.h"
            }
        }
    }
    return node_->evaluate(ctx, swl);
}

vector<float> ShaderNodeSlot::average() const noexcept {
    switch (dim_) {
        case 1: {
            switch (channel_mask_) {
#include "slot_average_swizzle_1.inl.h"
            }
        }
        case 2: {
            switch (channel_mask_) {
#include "slot_average_swizzle_2.inl.h"
            }
        }
        case 3: {
            switch (channel_mask_) {
#include "slot_average_swizzle_3.inl.h"
            }
        }
        case 4: {
            switch (channel_mask_) {
#include "slot_average_swizzle_4.inl.h"
            }
        }
    }
    OC_ASSERT(false);
    return {};
}

float ShaderNodeSlot::luminance() const noexcept {
    switch (dim_) {
        case 1: return average()[0];
        case 2:
            OC_ERROR("float2 not is a color attribute !");
            return 0;
        case 3:
        case 4:
            auto a = average();
            return ocarina::luminance(make_float3(a[0], a[1], a[2]));
    }
    OC_ASSERT(false);
    return 0;
}

ColorDecode ShaderNodeSlot::eval_albedo_spectrum(const AttrEvalContext &ctx, const SampledWavelengths &swl) const noexcept {
    OC_ASSERT(dim_ == 3 || dim_ == 0);
    Float3 val = evaluate(ctx, swl)->as_vec3();
    return node_->spectrum()->decode_to_albedo(val, swl);
}

ColorDecode ShaderNodeSlot::eval_unbound_spectrum(const AttrEvalContext &ctx, const SampledWavelengths &swl) const noexcept {
    OC_ASSERT(dim_ == 3 || dim_ == 0);
    Float3 val = evaluate(ctx, swl)->as_vec3();
    return node_->spectrum()->decode_to_unbound_spectrum(val, swl);
}

ColorDecode ShaderNodeSlot::eval_illumination_spectrum(const AttrEvalContext &ctx, const SampledWavelengths &swl) const noexcept {
    OC_ASSERT(dim_ == 3 || dim_ == 0);
    Float3 val = evaluate(ctx, swl)->as_vec3();
    return node_->spectrum()->decode_to_illumination(val, swl);
}
///#endregion

void ShaderNode::initialize_slots(const vision::ShaderNodeDesc &desc) noexcept {
}

ShaderNode &ShaderNode::set_graph(const SP<ShaderGraph> &graph) noexcept {
    graph_ = graph;
    return *this;
}

ShaderGraph &ShaderNode::graph() const noexcept {
    auto graph = graph_.lock();
    OC_ASSERT(graph != nullptr);
    return *graph;
}

ShaderNode &ShaderNode::add_to(ShaderGraph &graph) noexcept {
    return add_to(name(), graph);
}

ShaderNode &ShaderNode::add_to(const std::string &name, vision::ShaderGraph &graph) noexcept {
    graph.add_node(name, shared_from_this());
    return *this;
}

}// namespace vision
