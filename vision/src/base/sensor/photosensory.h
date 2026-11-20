//
// Created by Zero on 09/09/2022.
//

#pragma once

#include "math/basic_types.h"
#include "base/node.h"
#include "base/encoded_object.h"
#include "base/scattering/sample.h"
#include "math/transform.h"
#include "base/scattering/medium.h"
#include "filter.h"
#include "hotfix/hotfix.h"
#include "base/using.h"

namespace vision {
struct SensorSample {
    Float2 p_film;
    Float2 p_lens;
    Float time;
    Float filter_weight{1.f};
    SensorSample() = default;
    explicit SensorSample(const Uint2 &pixel)
        : p_film(pixel + 0.5f) {}
};

class Photosensory : public Node, public GUIRenderable, public EncodedObject, public Observer {
public:
    using Desc = SensorDesc;

protected:
    TFilter filter_{};
    EncodedData<uint> medium_id_{InvalidUI32};

public:
    Photosensory() = default;
    explicit Photosensory(const SensorDesc &desc);
    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override;
    OC_ENCODABLE_FUNC(EncodedObject, filter_)
    void restore(RuntimeObject *old_obj) noexcept override {
        Node::restore(old_obj);
        VS_HOTFIX_MOVE_ATTRS(filter_, medium_id_, datas_)
        if (old_obj_) restore_owner(*old_obj_);
    }
    VS_MAKE_GUI_STATUS_FUNC(Node, filter_)
    bool render_UI(Widgets *widgets) noexcept override;
    void render_sub_UI(Widgets *widgets) noexcept override;
    void prepare() noexcept override;
    void upload_immediately() noexcept override {
        EncodedObject::upload_immediately();
    }
    [[nodiscard]] auto &filter() noexcept { return filter_; }
    [[nodiscard]] auto &filter() const noexcept { return filter_; }
    [[nodiscard]] virtual RayState generate_ray(const SensorSample &ss) const noexcept = 0;
};

}// namespace vision

