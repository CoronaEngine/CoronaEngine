#pragma once

#include <corona/kernel/entity_id.h>
#include <cstdint>
#include <string>

namespace Corona::Systems::UI {

// CEF UI thread only. Resolves a GUID in the active archived scene and keeps
// the native handle private to the scene owner.
EntityId resolve_native_actor_handle_by_guid(const std::string& scene_id,
                                                   const std::string& archive_session,
                                                   const std::string& actor_guid,
                                                   std::string& error_code,
                                                   std::string& error_message);

}  // namespace Corona::Systems::UI
