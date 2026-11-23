/// LatestWins coalescing must merge a multi-field transform instead of keeping only the last
/// command for a key.
///
/// The editor's transform patch sets position, rotation and scale as three separate commands
/// (API::Geometry::set_position / set_rotation / set_scale each enqueue one single-field command).
/// CoalescePolicy::LatestWins keeps one command per transform handle, so replacing the pending
/// command wholesale dropped every field but the last: reading the transform back showed the stale
/// position while scale appeared to have applied - which is exactly how setActorTransform failed.
///
/// TransformUpdateKey::merge makes the carrier merge field by field. This test drives the three
/// single-field commands the editor actually issues and asserts all three land.
#include <corona/shared_data_hub.h>

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "TransformCommandMergeTests failed: %s\n", message);
        std::exit(1);
    }
}

bool near(float left, float right) { return left > right - 1e-5f && left < right + 1e-5f; }

Corona::TransformUpdateCommand command_for(Corona::EntityId handle, Corona::TransformField field) {
    Corona::TransformUpdateCommand command{};
    command.transform_handle = handle;
    command.fields = field;
    return command;
}

}  // namespace

int main() {
    auto& hub = Corona::SharedDataHub::instance();
    const Corona::EntityId handle = 0x7f01;

    // One field per command, exactly as the three Geometry setters do.
    auto position = command_for(handle, Corona::TransformField::Position);
    position.position = ktm::fvec3{0.0f, 2.0f, -3.0f};
    hub.enqueue_transform_update(position);

    auto rotation = command_for(handle, Corona::TransformField::Rotation);
    rotation.euler_rotation = ktm::fvec3{10.0f, 20.0f, 30.0f};
    hub.enqueue_transform_update(rotation);

    auto scale = command_for(handle, Corona::TransformField::Scale);
    scale.scale = ktm::fvec3{2.0f, 1.0f, 3.0f};
    hub.enqueue_transform_update(scale);

    const auto view = hub.transform_view(handle);
    require(view.has_value(), "a pending transform must be visible through the view");
    require(near(view->position.x, 0.0f) && near(view->position.y, 2.0f) && near(view->position.z, -3.0f),
            "the position written before rotation and scale must survive coalescing");
    require(near(view->euler_rotation.x, 10.0f) && near(view->euler_rotation.y, 20.0f) &&
                near(view->euler_rotation.z, 30.0f),
            "the rotation written between position and scale must survive coalescing");
    require(near(view->scale.x, 2.0f) && near(view->scale.y, 1.0f) && near(view->scale.z, 3.0f),
            "the last written scale must still apply");

    // A matrix already carries TRS, so it replaces what was pending - unchanged behaviour.
    auto matrix = command_for(handle, Corona::TransformField::Matrix);
    matrix.position = ktm::fvec3{1.0f, 1.0f, 1.0f};
    matrix.matrix = {2.0f, 0.0f, 0.0f, 0.0f,
                     0.0f, 2.0f, 0.0f, 0.0f,
                     0.0f, 0.0f, 2.0f, 0.0f,
                     1.0f, 1.0f, 1.0f, 1.0f};
    hub.enqueue_transform_update(matrix);
    const auto replaced = hub.transform_view(handle);
    require(replaced.has_value(), "a replaced transform must still be visible");
    require(near(replaced->position.x, 1.0f),
            "a matrix command must replace the pending decomposed position");
    hub.clear_transform_updates(handle);

    std::printf("TransformCommandMergeTests passed\n");
    return 0;
}
