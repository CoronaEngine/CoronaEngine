#pragma once

#include "base/illumination/light.h"

namespace vision {
class IDirectionalLight : public Environment {
public:
    using Environment::Environment;
    virtual void set_direction(float3 outgoing) noexcept = 0;
};
} // namespace vision
