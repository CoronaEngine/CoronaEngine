#pragma once

#include <cstddef>

namespace Corona::Systems::UI {

// Applies queued collaborative editor mutations on the engine/UI thread.
// The editor page is not required to be mounted for this work to progress.
void tick_collaborative_editor_runtime(std::size_t batch_limit = 8);

}  // namespace Corona::Systems::UI
