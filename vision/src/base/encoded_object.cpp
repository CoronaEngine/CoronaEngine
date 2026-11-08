//
// Created by Zero on 2025/3/23.
//

#include "encoded_object.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"

namespace vision {
using namespace ocarina;
EncodedObject::EncodedObject() {
    if (auto rp = Global::instance().pipeline_shared()) {
        datas_.set_bindless_array(rp->bindless_array());
        rp->register_encoded_object(this);
        owner_pipeline_ = rp;
    } else if (auto *bindless = Global::instance().scene_bindless_array()) {
        datas_.set_bindless_array(*bindless);
    }
}

EncodedObject::~EncodedObject() {
    if (auto rp = owner_pipeline_.lock()) {
        rp->deregister_encoded_object(this);
    }
}

void EncodedObject::restore_owner(const EncodedObject &previous) noexcept {
    auto owner = previous.owner_pipeline_.lock();
    if (auto current = owner_pipeline_.lock(); current != owner) {
        if (current) current->deregister_encoded_object(this);
        owner_pipeline_ = owner;
        if (owner) owner->register_encoded_object(this);
    }
}

void EncodedObject::encode_data() noexcept {
    auto size = aligned_size();
    datas_.host_buffer().resize(size / sizeof(buffer_ty));
    encode(datas_);
}

void EncodedObject::prepare_data() noexcept {
    encode_data();
    datas().reset_device_buffer_immediately(Global::instance().device(),
                                            ocarina::format("EncodedObject: {}::data_", typeid(*this).name()));
    datas().register_self();
}

void EncodedObject::update_data() noexcept {
    encode(datas_);
}

void EncodedObject::load_data() noexcept {
    DataAccessor da = {0, datas_};
    DynamicArray<buffer_ty> array = da.load_dynamic_array<buffer_ty>(aligned_size() / 4);
    decode(array);
}

void EncodedObject::upload_immediately() noexcept {
    if (datas().host_buffer().empty()) {
        return;
    }
    datas().upload_immediately();
}

BufferUploadCommand *EncodedObject::upload_sync() noexcept {
    return datas().upload_sync();
}

BufferUploadCommand *EncodedObject::upload() noexcept {
    return datas().upload();
}

}// namespace vision
