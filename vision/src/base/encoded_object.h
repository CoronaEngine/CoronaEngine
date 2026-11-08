//
// Created by Zero on 2023/6/18.
//

#pragma once

#include "core/stl.h"
#include "dsl/data/encodable.h"
#include "node.h"
#include "mgr/global.h"
#include "base/using.h"

namespace vision {


/**
 * Provides serialization interface for integrators,
 * cameras and other monolithic objects
 * that do not need to be added to polymorphic lists
 */
class EncodedObject : public Encodable {
protected:
    RegistrableManaged<buffer_ty> datas_{};
    weak_ptr<Pipeline> owner_pipeline_{};

protected:
    EncodedObject();
    // Replacement construction can run under another cached pipeline's context.
    void restore_owner(const EncodedObject &previous) noexcept;

public:
    [[nodiscard]] RegistrableManaged<buffer_ty> &datas() noexcept { return datas_; }
    [[nodiscard]] const RegistrableManaged<buffer_ty> &datas() const noexcept { return datas_; }

    /**
     * Serialize the data to managed memory
     * for upload to device memory
     */
    virtual void encode_data() noexcept;

    /**
     * encode data, initialize device buffer and register buffer to resource array
     */
    virtual void prepare_data() noexcept;

    /**
     * update data to managed memory
     * tips: Called on the host side code
     */
    virtual void update_data() noexcept;

    virtual void update_device_data() noexcept = 0;

    /**
     * load data from device memory
     * tips: Called on the device side code
     */
    virtual void load_data() noexcept;
    virtual void upload_immediately() noexcept;
    [[nodiscard]] virtual BufferUploadCommand *upload_sync() noexcept;
    [[nodiscard]] virtual BufferUploadCommand *upload() noexcept;
    virtual ~EncodedObject();
};

}// namespace vision
