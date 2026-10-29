//
// Created by Zero on 06/09/2022.
//

#include "project_desc.h"

namespace vision {

void SceneDesc::init_material_descs(const DataWrap &materials) noexcept {
    for (uint i = 0; i < materials.size(); ++i) {
        MaterialDesc md;
        md.init(materials[i]);
        material_descs.push_back(md);
    }
}

void SceneDesc::init_shape_descs(const DataWrap &shapes) noexcept {
    for (const auto &shape : shapes) {
        ShapeDesc sd;
        sd.init(shape);
        shape_descs.push_back(sd);
    }
}

void SceneDesc::init_medium_descs(const DataWrap &mediums) noexcept {
    mediums_desc.global = mediums.value("global", "");
    mediums_desc.process = mediums.value("process", true);
    DataWrap lst = mediums.value("list", DataWrap::object());
    for (const auto &elm : lst) {
        MediumDesc desc;
        desc.init(elm);
        mediums_desc.mediums.push_back(desc);
    }
}

void SceneDesc::init_light_descs(const DataWrap &lights) noexcept {
    for (const auto &light : lights) {
        LightDesc desc;
        desc.init(light);
        light_descs.push_back(desc);
    }
}



}// namespace vision
