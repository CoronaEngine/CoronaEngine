/**
 * @file test_interaction_boundary.cpp
 * @brief SharedDataHub 跨线程交互边界契约测试（命令 + 快照）
 *
 * 这是第一个链接 CoronaEngine 的测试目标：SharedDataHub 的命令/快照语义
 * 只有在引擎库里才能被真正执行，P0 的 kernel 测试覆盖不到。
 *
 * 覆盖：
 * - 相机：播种快照、已提交值与待提交值分离、LatestWins 合并、释放后摘行
 * - 变换：命令叠加、快照发布、按 transform_handle 合并、销毁时丢弃待处理命令
 */

#include <corona/shared_data_hub.h>
#include <corona/systems/mechanics/mechanics_commands.h>
#include <corona/systems/optics/optics_commands.h>
#include <corona/systems/acoustics/acoustics_commands.h>

#include <cstdio>
#include <cstdint>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_impl(bool ok, const char* text, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, text);
    }
}

#define CHECK(cond) check_impl((cond), #cond, __FILE__, __LINE__)

using Corona::SharedDataHub;

ktm::fvec3 vec3(float x, float y, float z) {
    ktm::fvec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

void test_camera_seed_and_committed_read() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();
    CHECK(handle != 0);

    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->position = vec3(1.0f, 2.0f, 3.0f);
        camera->fov = 61.0f;
        camera->width = 800;
        camera->height = 600;
    }

    CHECK(!hub.camera_row(handle).has_value());

    hub.seed_camera_snapshot_from_storage(handle);
    const auto row = hub.camera_row(handle);
    CHECK(row.has_value());
    CHECK(row->position.x == 1.0f && row->position.y == 2.0f && row->position.z == 3.0f);
    CHECK(row->fov == 61.0f);
    CHECK(row->width == 800);
    CHECK(row->height == 600);

    hub.remove_camera_snapshot_row(handle);
    CHECK(!hub.camera_row(handle).has_value());
    hub.camera_storage().deallocate(handle);
}

void test_camera_pending_overlay() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->position = vec3(0.0f, 0.0f, 0.0f);
        camera->fov = 45.0f;
        camera->width = 800;
        camera->height = 600;
    }
    hub.seed_camera_snapshot_from_storage(handle);

    Corona::CameraMoveCommand move{};
    move.camera_handle = handle;
    move.position = vec3(10.0f, 20.0f, 30.0f);
    move.forward = vec3(0.0f, 0.0f, 1.0f);
    move.world_up = vec3(0.0f, 1.0f, 0.0f);
    move.fov = 70.0f;
    hub.enqueue_camera_move(move);

    CHECK(hub.camera_view(handle).position.x == 10.0f);
    CHECK(hub.camera_view(handle).fov == 70.0f);
    CHECK(hub.camera_row(handle)->position.x == 0.0f);
    CHECK(hub.camera_row(handle)->fov == 45.0f);

    Corona::CameraStateUpdateCommand state{};
    state.camera_handle = handle;
    state.fields = Corona::CameraStateUpdateField::Size;
    state.width = 1024;
    state.height = 768;
    hub.enqueue_camera_state_update(state);
    CHECK(hub.camera_view(handle).width == 1024);
    CHECK(hub.camera_row(handle)->width == 800);

    for (const auto& incoming : hub.drain_camera_moves()) {
        if (auto camera = hub.camera_storage().try_acquire_write(incoming.camera_handle)) {
            camera->position = incoming.position;
            camera->forward = incoming.forward;
            camera->world_up = incoming.world_up;
            camera->fov = incoming.fov;
        }
    }
    for (const auto& update : hub.drain_camera_state_updates()) {
        if (auto camera = hub.camera_storage().try_acquire_write(update.camera_handle)) {
            if (Corona::has_camera_state_field(update.fields,
                                               Corona::CameraStateUpdateField::Size)) {
                camera->width = update.width;
                camera->height = update.height;
            }
        }
    }
    hub.publish_camera_snapshot_from_storage();

    CHECK(hub.camera_view(handle).position.x == 10.0f);
    CHECK(hub.camera_row(handle)->position.x == 10.0f);
    CHECK(hub.camera_row(handle)->width == 1024);

    hub.remove_camera_snapshot_row(handle);
    hub.camera_storage().deallocate(handle);
}

void test_camera_move_coalescing() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();

    for (int i = 0; i < 5; ++i) {
        Corona::CameraMoveCommand move{};
        move.camera_handle = handle;
        move.position = vec3(static_cast<float>(i), 0.0f, 0.0f);
        move.fov = 45.0f;
        hub.enqueue_camera_move(move);
    }
    CHECK(hub.camera_view(handle).position.x == 4.0f);

    const auto moves = hub.drain_camera_moves();
    CHECK(moves.size() == 1);
    CHECK(moves[0].position.x == 4.0f);

    hub.camera_storage().deallocate(handle);
}

void test_transform_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.model_transform_storage().allocate();
    CHECK(handle != 0);
    {
        auto transform = hub.model_transform_storage().acquire_write(handle);
        transform->position = vec3(5.0f, 5.0f, 5.0f);
    }

    CHECK(!hub.transform_row(handle).has_value());
    CHECK(!hub.transform_view(handle).has_value());

    hub.publish_transform_snapshot_from_storage();
    CHECK(hub.transform_row(handle).has_value());
    CHECK(hub.transform_row(handle)->position.x == 5.0f);

    Corona::TransformUpdateCommand update{};
    update.transform_handle = handle;
    update.fields = Corona::TransformField::Position;
    update.position = vec3(9.0f, 0.0f, 0.0f);
    hub.enqueue_transform_update(update);
    hub.enqueue_transform_update(update);

    CHECK(hub.transform_view(handle)->position.x == 9.0f);
    CHECK(hub.transform_row(handle)->position.x == 5.0f);

    const auto drained = hub.drain_transform_updates();
    CHECK(drained.size() == 1);

    hub.enqueue_transform_update(update);
    hub.clear_transform_updates(handle);
    CHECK(hub.transform_view(handle)->position.x == 5.0f);
    CHECK(hub.drain_transform_updates().empty());

    hub.model_transform_storage().deallocate(handle);
}

void test_mechanics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::MechanicsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.mechanics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.mechanics_storage().acquire_write(handle);
        device->mass = 3.0f;
        device->restitution = 0.5f;
    }

    CHECK(!Corona::mechanics_row(handle).has_value());

    Corona::publish_mechanics_snapshot_from_storage();
    CHECK(Corona::mechanics_row(handle).has_value());
    CHECK(Corona::mechanics_row(handle)->mass == 3.0f);
    CHECK(Corona::mechanics_row(handle)->restitution == 0.5f);

    // 命令：pending 立刻可见，已提交值不变
    Corona::MechanicsUpdateCommand command{};
    command.mechanics_handle = handle;
    command.fields = Corona::MechanicsField::Mass | Corona::MechanicsField::BodyType;
    command.mass = 7.5f;
    command.body_type = Corona::BodyType::Static;
    channel.commands().push(command);

    CHECK(Corona::mechanics_view(handle)->mass == 7.5f);
    CHECK(Corona::mechanics_view(handle)->body_type == Corona::BodyType::Static);
    CHECK(Corona::mechanics_row(handle)->mass == 3.0f);
    CHECK(Corona::mechanics_row(handle)->body_type == Corona::BodyType::Dynamic);

    // LatestWins：同 handle 再推一条，合并成最后一条
    command.mass = 8.5f;
    channel.commands().push(command);
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    CHECK(drained[0].mass == 8.5f);
    CHECK(Corona::has_mechanics_field(drained[0].fields, Corona::MechanicsField::BodyType));

    // 模拟 Mechanics 应用后再发布：pending 视图与已提交值收敛
    for (const auto& incoming : drained) {
        if (auto device = hub.mechanics_storage().try_acquire_write(incoming.mechanics_handle)) {
            if (Corona::has_mechanics_field(incoming.fields, Corona::MechanicsField::Mass)) {
                device->mass = incoming.mass;
            }
            if (Corona::has_mechanics_field(incoming.fields, Corona::MechanicsField::BodyType)) {
                device->body_type = incoming.body_type;
            }
        }
    }
    Corona::publish_mechanics_snapshot_from_storage();
    CHECK(Corona::mechanics_view(handle)->mass == 8.5f);
    CHECK(Corona::mechanics_row(handle)->mass == 8.5f);

    channel.commands().clear();
    hub.mechanics_storage().deallocate(handle);
}

void test_optics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::OpticsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.optics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.optics_storage().acquire_write(handle);
        device->metallic = 0.25f;
        device->roughness = 0.75f;
    }

    CHECK(!Corona::optics_row(handle).has_value());
    Corona::publish_optics_snapshot_from_storage();
    CHECK(Corona::optics_row(handle).has_value());
    CHECK(Corona::optics_row(handle)->metallic == 0.25f);
    CHECK(Corona::optics_row(handle)->roughness == 0.75f);

    // 命令：pending 立刻可见，已提交值不变
    Corona::OpticsUpdateCommand command{};
    command.optics_handle = handle;
    command.fields = Corona::OpticsField::Metallic | Corona::OpticsField::Visible;
    command.metallic = 0.9f;
    command.visible = false;
    channel.commands().push(command);

    CHECK(Corona::optics_view(handle)->metallic == 0.9f);
    CHECK(Corona::optics_view(handle)->visible == false);
    CHECK(Corona::optics_row(handle)->metallic == 0.25f);
    CHECK(Corona::optics_row(handle)->visible == true);

    // LatestWins：同 handle 再推一条，合并成最后一条
    command.metallic = 0.95f;
    channel.commands().push(command);
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    CHECK(drained[0].metallic == 0.95f);
    CHECK(Corona::has_optics_field(drained[0].fields, Corona::OpticsField::Visible));

    for (const auto& incoming : drained) {
        if (auto device = hub.optics_storage().try_acquire_write(incoming.optics_handle)) {
            if (Corona::has_optics_field(incoming.fields, Corona::OpticsField::Metallic)) {
                device->metallic = incoming.metallic;
            }
            if (Corona::has_optics_field(incoming.fields, Corona::OpticsField::Visible)) {
                device->visible = incoming.visible;
            }
        }
    }
    Corona::publish_optics_snapshot_from_storage();
    CHECK(Corona::optics_view(handle)->metallic == 0.95f);
    CHECK(Corona::optics_row(handle)->metallic == 0.95f);
    CHECK(Corona::optics_row(handle)->visible == false);

    channel.commands().clear();
    hub.optics_storage().deallocate(handle);
}

void test_acoustics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::AcousticsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.acoustics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.acoustics_storage().acquire_write(handle);
        device->volume = 0.4f;
        device->resource_id = 42;
    }

    Corona::publish_acoustics_snapshot_from_storage();
    CHECK(Corona::acoustics_row(handle).has_value());
    CHECK(Corona::acoustics_row(handle)->volume == 0.4f);
    CHECK(Corona::acoustics_row(handle)->resource_id == 42);

    // 先绑资源再读：pending 必须可见（脚本常见用法：set_audio_resource 后立刻 play）
    Corona::AcousticsUpdateCommand command{};
    command.acoustics_handle = handle;
    command.fields = Corona::AcousticsField::ResourceId | Corona::AcousticsField::Volume;
    command.resource_id = 99;
    command.volume = 0.8f;
    channel.commands().push(command);

    CHECK(Corona::acoustics_view(handle)->resource_id == 99);
    CHECK(Corona::acoustics_view(handle)->volume == 0.8f);
    CHECK(Corona::acoustics_row(handle)->resource_id == 42);  // 已提交值不变

    // 应用 + 发布后收敛
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    for (const auto& incoming : drained) {
        if (auto device = hub.acoustics_storage().try_acquire_write(incoming.acoustics_handle)) {
            if (Corona::has_acoustics_field(incoming.fields, Corona::AcousticsField::ResourceId)) {
                device->resource_id = incoming.resource_id;
            }
            if (Corona::has_acoustics_field(incoming.fields, Corona::AcousticsField::Volume)) {
                device->volume = incoming.volume;
            }
        }
    }
    Corona::publish_acoustics_snapshot_from_storage();
    CHECK(Corona::acoustics_row(handle)->resource_id == 99);
    CHECK(Corona::acoustics_view(handle)->resource_id == 99);

    // 无快照、只有 pending 时也要能读出来
    const auto fresh = hub.acoustics_storage().allocate();
    Corona::AcousticsUpdateCommand bind{};
    bind.acoustics_handle = fresh;
    bind.fields = Corona::AcousticsField::ResourceId;
    bind.resource_id = 7;
    channel.commands().push(bind);
    CHECK(Corona::acoustics_view(fresh)->resource_id == 7);
    channel.commands().clear();
    hub.acoustics_storage().deallocate(fresh);

    channel.commands().clear();
    hub.acoustics_storage().deallocate(handle);
}

}  // namespace

int main() {
    std::printf("== Interaction boundary (command + snapshot) tests ==\n");
    test_camera_seed_and_committed_read();
    test_camera_pending_overlay();
    test_camera_move_coalescing();
    test_transform_command_and_snapshot();
    test_mechanics_command_and_snapshot();
    test_optics_command_and_snapshot();
    test_acoustics_command_and_snapshot();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
