#pragma once

#include <array>
#include <optional>

namespace Corona::Systems::UI {

/// The transform fields a caller supplied. Upstream this is a typed V8 patch or one of the
/// non-Web adapters; anything the caller left out is absent here.
struct TransformPatchValues {
    std::optional<std::array<float, 3>> position;
    std::optional<std::array<float, 3>> rotation;
    std::optional<std::array<float, 3>> scale;
};

/// A complete translation/rotation/scale, the only shape `Geometry::set_transform` accepts.
struct TransformTriple {
    std::array<float, 3> position;
    std::array<float, 3> rotation;
    std::array<float, 3> scale;
};

/// Complete a partial patch against the live device state.
///
/// The device (the geometry's transform row) is the only carrier that sees every writer: the
/// editor, physics and the render/network snapshots. The authored cache on `NativeEditorActor` is
/// written on load and by patches and is never read back from the device, so completing a
/// scale-only patch from that cache snapped an actor physics had moved back to its authored
/// position - the fields the patch did not name were taken from a value nothing had updated.
///
/// Missing fields therefore come from the device. The cache keeps its own job: the initial value
/// for an actor that has no geometry yet.
inline TransformTriple complete_transform(const TransformPatchValues& patch,
                                          const TransformTriple& device) {
    return {
        patch.position ? *patch.position : device.position,
        patch.rotation ? *patch.rotation : device.rotation,
        patch.scale ? *patch.scale : device.scale,
    };
}

}  // namespace Corona::Systems::UI
