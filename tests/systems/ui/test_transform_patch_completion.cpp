/// A partial transform patch must be completed from the live device, not from the authored cache.
///
/// `Geometry::set_transform` takes a whole TRS, so the fields a patch leaves out need a source.
/// The editor used to take them from `NativeEditorActor::position/rotation/scale`, a cache that is
/// written on load and by patches but never read back from the device. The device is the only
/// carrier every writer touches (editor, physics, render/network snapshots), so a scale-only patch
/// snapped an actor that physics had moved back to its authored position.
///
/// This pins the rule: missing fields come from what the device currently reports.
#include "cef/transform_patch.h"

#include <cstdio>
#include <cstdlib>

namespace {

using Corona::Systems::UI::complete_transform;
using Corona::Systems::UI::TransformPatchValues;
using Corona::Systems::UI::TransformTriple;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "TransformPatchCompletionTests failed: %s\n", message);
        std::exit(1);
    }
}

bool near(float left, float right) { return left > right - 1e-5f && left < right + 1e-5f; }

/// What the device reports: physics moved the actor to +5 on X, and the angle is odd so a
/// copy of the authored value cannot look like a match by accident.
const TransformTriple device{{5.0f, 0.0f, 0.0f}, {0.0f, 45.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};

}  // namespace

int main() {
    // A scale-only patch: the two fields it does not name must come from the device (the moved
    // value), and the one it names must win.
    TransformPatchValues scale_only;
    scale_only.scale = std::array<float, 3>{2.0f, 2.0f, 2.0f};
    auto completed = complete_transform(scale_only, device);
    require(near(completed.position[0], 5.0f), "an unnamed position must come from the device");
    require(near(completed.rotation[1], 45.0f), "an unnamed rotation must come from the device");
    require(near(completed.scale[0], 2.0f), "a named field must still win");

    // Every field present: the device has nothing to contribute.
    TransformPatchValues full;
    full.position = std::array<float, 3>{1.0f, 2.0f, 3.0f};
    full.rotation = std::array<float, 3>{10.0f, 20.0f, 30.0f};
    full.scale = std::array<float, 3>{4.0f, 5.0f, 6.0f};
    completed = complete_transform(full, device);
    require(near(completed.position[1], 2.0f) && near(completed.rotation[2], 30.0f) &&
                near(completed.scale[0], 4.0f),
            "a complete patch passes through untouched");

    // Nothing present: the device passes through untouched (a caller that sent no vector is
    // refused before this point, but the rule itself has to stay total).
    completed = complete_transform(TransformPatchValues{}, device);
    require(near(completed.position[0], 5.0f) && near(completed.rotation[1], 45.0f) &&
                near(completed.scale[0], 1.0f),
            "an empty patch reports the device unchanged");

    std::puts("TransformPatchCompletionTests passed");
    return 0;
}
