#pragma once

#include <corona/kernel/entity_id.h>
#include <cstdint>

namespace Corona::Systems::UI {

void probe_cef_shared_texture(EntityId raw_handle, int coded_width, int coded_height);

}  // namespace Corona::Systems::UI
