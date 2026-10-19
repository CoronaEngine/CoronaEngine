#pragma once

#include <cef_task.h>

#include <corona/resource/resource_manager.h>

#include <cstdint>

namespace Corona::Systems::UI {

/// Reads a resource on behalf of a caller that may be the CEF UI thread.
///
/// The CEF UI thread paints the editor and dispatches every editor input, so a
/// blocking resource read there freezes the whole editor while the engine render
/// loop keeps running.  That is not hypothetical: the editor hung permanently
/// inside `SceneTools.get_scene_snapshot` and again inside
/// `SceneTools.get_actor_skeleton_leaves`, both waiting for a Scene entry lock that
/// the LOD residency pass holds exclusively across a full mesh/LOD sweep.
///
/// On the CEF UI thread the read is therefore best effort: "no data this time" is a
/// valid answer, and the caller reports it instead of hanging.  Other callers
/// (Python script runtime, AI tools) keep the waiting semantics they rely on.
template <typename T = Corona::Resource::IResource>
[[nodiscard]] Corona::Resource::ReadHandle<T> read_resource_for_ui(std::uint64_t rid) {
    auto& manager = Corona::Resource::ResourceManager::get_instance();
    if (CefCurrentlyOn(TID_UI)) {
        return manager.try_acquire_read_nowait<T>(rid);
    }
    return manager.acquire_read<T>(rid);
}

}  // namespace Corona::Systems::UI
