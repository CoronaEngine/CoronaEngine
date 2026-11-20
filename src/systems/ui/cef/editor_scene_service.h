#pragma once

#include <corona/kernel/entity_id.h>
#include <cstdint>
#include <string>

namespace Corona::Systems::UI {

/// Write the live scene once, when the process is about to stop.
///
/// Both the Web's `corona.app.close_process` and the OS window button reach the exit request, so
/// this is the single save site for a quit. Returns true when a write was queued or a previous
/// one already happened; false when there is nothing to write (no scene, read-only) or the write
/// failed. A false result never blocks the exit.
bool save_scene_for_quit();

}  // namespace Corona::Systems::UI
